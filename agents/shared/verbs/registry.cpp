//   Copyright 2026 William Isted and contributors
//
//   Licensed under the Apache License, Version 2.0 (the "License");
//   you may not use this file except in compliance with the License.
//   You may obtain a copy of the License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
//   Unless required by applicable law or agreed to in writing, software
//   distributed under the License is distributed on an "AS IS" BASIS,
//   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//   See the License for the specific language governing permissions and
//   limitations under the License.

// `registry.*` namespace verb handlers (Windows-only).
//
// Implements the verbs whose contracts are the spec JSON under
// protocol/spec/verbs/windows/registry.*.json (the single source of truth):
//
//   registry.key.read     (R)  {path} -> {subkeys:[str], values:[{name,type}]}
//                              x-errors: ["not_found","permission_denied",
//                                         "invalid_args"]
//   registry.value.read   (R)  {path,value} -> {type,data}
//                              x-errors: ["not_found","permission_denied",
//                                         "invalid_args"]
//   registry.value.create (C)  {path,value,type,data} -> OK 0
//                              x-errors: ["already_exists","permission_denied",
//                                         "invalid_args","not_supported"]
//   registry.value.update (U)  {path,value,type,data} -> OK 0
//                              x-errors: ["not_found","permission_denied",
//                                         "invalid_args","not_supported"]
//   registry.value.delete (D)  {path,value} -> OK 0
//                              x-errors: ["not_found","permission_denied",
//                                         "invalid_args"]
//   registry.key.delete   (D)  {path,recursive} -> OK 0
//                              x-errors: ["not_found","not_empty",
//                                         "permission_denied","invalid_args"]
//
// PHASE 2.1 — NAMED-ARG MIGRATION. These handlers no longer index `req.args`
// positionally. Each declares its input_schema property list (IN SCHEMA
// ORDER) and reads each value by NAME through the shared SchemaArgs resolver
// (schema_args.hpp), with the schema's property ORDER used as the positional
// fallback when the caller invoked the verb positionally (the v2.2 reference
// client packs positional calls as `{"_args":[...]}`). The `window.*`
// namespace was the pattern-setter; this file follows it (and system.cpp /
// process.cpp / clipboard.cpp) exactly. Validation emits the same
// ErrorCode::InvalidArgs + {"message":...} ergonomics as before.
//
// MULTI-VERB DISPATCH. The capability map (agents/shared/capabilities.cpp)
// deliberately points multiple wire verb names at three shared symbols —
// `read` (registry.key.read / registry.value.read), `write`
// (registry.value.create / registry.value.update) and `delete_`
// (registry.value.delete / registry.key.delete). Each handler branches on
// req.verb so its input_schema, output shape, and x-errors are exactly the
// SPEC's for the dispatched verb — there is no shared "mode flag" arg any
// more (the pre-2.1 `--value` / `--recursive` scanning is gone).
//
// REG_BINARY base64 — Phase 2b split:
//   * READ path (registry.value.read / registry.key.read): a REG_BINARY
//     value's `data` is base64-encoded per registry.value.read.json
//     x-output-schema. Implemented in full here (output side, not the
//     deferred input side) via a file-local encoder.
//   * WRITE path (registry.value.create / registry.value.update): decoding
//     base64 `data` back into bytes for a REG_BINARY write is the Phase-2b
//     binary side-channel. The arg's PRESENCE and STRING shape are validated
//     here; the decode is deferred with an explicit ERR not_supported
//     ({"reason":"reg_binary_write_phase2b"}) — within both write verbs'
//     declared x-errors. string / dword / qword / multi_sz writes are
//     implemented in full now. See report.
//
// Paths use the standard `HKLM\Software\...` form. Roots accepted: HKLM,
// HKCU, HKCR, HKU, HKCC (or their long-form equivalents).

#include "../base64.hpp"
#include "../connection.hpp"
#include "../errors.hpp"
#include "../json.hpp"
#include "../log.hpp"
#include "args.hpp"
#include "schema_args.hpp"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>  // SHDeleteKeyW — XP-compatible recursive registry key delete

namespace remote_hands::registry_verbs {

// The Phase-2.1 named-argument resolver and its invalid_args helper live in
// the shared header (schema_args.hpp) so every namespace reads through one
// definition. Pull them into this TU's unqualified name lookup; behaviour is
// identical to window.cpp / system.cpp / process.cpp / clipboard.cpp.
using wire::SchemaArgs;
using wire::invalid_args;

namespace {

// ---------------------------------------------------------------------------
// Path parsing

HKEY parse_root(std::string_view name) {
    if (name == "HKLM" || name == "HKEY_LOCAL_MACHINE")    return HKEY_LOCAL_MACHINE;
    if (name == "HKCU" || name == "HKEY_CURRENT_USER")     return HKEY_CURRENT_USER;
    if (name == "HKCR" || name == "HKEY_CLASSES_ROOT")     return HKEY_CLASSES_ROOT;
    if (name == "HKU"  || name == "HKEY_USERS")            return HKEY_USERS;
    if (name == "HKCC" || name == "HKEY_CURRENT_CONFIG")   return HKEY_CURRENT_CONFIG;
    return nullptr;
}

std::wstring utf8_to_wide(std::string_view s) {
    if (s.empty()) return {};
    const int wlen = MultiByteToWideChar(
        CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), wlen);
    return out;
}

std::string wide_to_utf8(const wchar_t* w, std::size_t len) {
    if (len == 0) return {};
    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, w, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(len),
                        out.data(), needed, nullptr, nullptr);
    return out;
}

bool split_path(std::string_view full, HKEY& root_out, std::wstring& subkey_out) {
    const auto sep = full.find('\\');
    if (sep == std::string_view::npos || sep == 0) return false;

    root_out = parse_root(full.substr(0, sep));
    if (!root_out) return false;

    subkey_out = utf8_to_wide(full.substr(sep + 1));
    return true;
}

// Resolve the required `path` property and split it into (root, subkey).
// Emits the spec-declared invalid_args on a missing/unparseable path. The
// pre-2.1 multi-token path-reconstruction is gone: the v2.2 header quoting
// (PROTOCOL.md §1.2.5) and the MCP named-args object both deliver `path` as
// one string, so the value is taken verbatim.
bool resolve_path(Connection& conn, SchemaArgs& args, std::string_view verb,
                  HKEY& root_out, std::wstring& subkey_out) {
    std::optional<std::string> path = args.str("path");
    if (!path || path->empty()) {
        invalid_args(conn, std::string(verb) + " requires 'path' "
                           "(e.g. HKLM\\Software\\...)");
        return false;
    }
    if (!split_path(*path, root_out, subkey_out)) {
        conn.writer().write_err(ErrorCode::InvalidArgs,
                                "{\"message\":\"unrecognised registry path\"}");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Type mapping

DWORD parse_reg_type(std::string_view s) {
    if (s == "REG_SZ")               return REG_SZ;
    if (s == "REG_EXPAND_SZ")        return REG_EXPAND_SZ;
    if (s == "REG_LINK")             return REG_LINK;
    if (s == "REG_DWORD")            return REG_DWORD;
    if (s == "REG_DWORD_BIG_ENDIAN") return REG_DWORD_BIG_ENDIAN;
    if (s == "REG_QWORD")            return REG_QWORD;
    if (s == "REG_BINARY")           return REG_BINARY;
    if (s == "REG_MULTI_SZ")         return REG_MULTI_SZ;
    if (s == "REG_NONE")             return REG_NONE;
    // Sentinel for "not a recognised type string" — distinct from a valid
    // REG_NONE request. (REG_NONE == 0, so a separate sentinel is needed.)
    return 0xFFFFFFFFu;
}

const char* reg_type_to_string(DWORD t) {
    switch (t) {
        case REG_SZ:                return "REG_SZ";
        case REG_EXPAND_SZ:         return "REG_EXPAND_SZ";
        case REG_LINK:              return "REG_LINK";
        case REG_DWORD:             return "REG_DWORD";
        case REG_DWORD_BIG_ENDIAN:  return "REG_DWORD_BIG_ENDIAN";
        case REG_QWORD:             return "REG_QWORD";
        case REG_BINARY:            return "REG_BINARY";
        case REG_MULTI_SZ:          return "REG_MULTI_SZ";
        case REG_NONE:              return "REG_NONE";
        default:                    return "REG_NONE";
    }
}

// REG_BINARY base64 — the encoder used to live in this TU as a file-local
// helper. Promoted to the shared header `base64.hpp` (R6) so vision.describe
// can encode PNG bytes for its OpenAI-compatible image_url payload without a
// third copy. Byte-identical output to the pre-promotion file-local copy
// (the algorithm in base64.hpp was lifted verbatim from this TU's copy).

// Serialise a value's data per registry.value.read.json x-output-schema:
//   REG_SZ / REG_EXPAND_SZ / REG_LINK : the literal string
//   REG_DWORD / REG_DWORD_BIG_ENDIAN  : decimal-stringified integer
//   REG_QWORD                         : decimal-stringified integer
//   REG_BINARY / REG_NONE / other     : base64
//   REG_MULTI_SZ                      : the wide block transcoded to UTF-8
//                                       with embedded NULs preserved and a
//                                       trailing NUL ("null-byte-separated,
//                                       with a trailing null").
// The result is appended as a JSON string value (json::append_string escapes
// control bytes incl. embedded NUL as \u0000, so the MULTI_SZ separators
// survive the wire).
void append_value_data(std::string& out, DWORD type,
                       const std::vector<BYTE>& data) {
    json::append_string(out, "data");
    out += ':';

    switch (type) {
        case REG_SZ:
        case REG_EXPAND_SZ:
        case REG_LINK: {
            const auto* w = reinterpret_cast<const wchar_t*>(data.data());
            std::size_t wlen = data.size() / sizeof(wchar_t);
            while (wlen > 0 && w[wlen - 1] == L'\0') --wlen;  // trim trailing NUL
            json::append_string(out, wide_to_utf8(w, wlen));
            break;
        }
        case REG_DWORD:
        case REG_DWORD_BIG_ENDIAN: {
            DWORD v = 0;
            if (data.size() >= sizeof(v)) std::memcpy(&v, data.data(), sizeof(v));
            if (type == REG_DWORD_BIG_ENDIAN) {
                v = ((v & 0x000000ffu) << 24) | ((v & 0x0000ff00u) << 8) |
                    ((v & 0x00ff0000u) >> 8)  | ((v & 0xff000000u) >> 24);
            }
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%lu", v);
            json::append_string(out, buf);
            break;
        }
        case REG_QWORD: {
            std::uint64_t v = 0;
            if (data.size() >= sizeof(v)) std::memcpy(&v, data.data(), sizeof(v));
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%llu",
                          static_cast<unsigned long long>(v));
            json::append_string(out, buf);
            break;
        }
        case REG_MULTI_SZ: {
            // Transcode the whole wide block (including embedded NUL
            // separators and the trailing NUL) to UTF-8; append_string
            // escapes NUL as \u0000 so the structure survives the wire.
            const auto* w = reinterpret_cast<const wchar_t*>(data.data());
            const std::size_t wlen = data.size() / sizeof(wchar_t);
            json::append_string(out, wide_to_utf8(w, wlen));
            break;
        }
        default: {
            // REG_BINARY, REG_NONE and any unmodelled type -> base64
            // (registry.value.read.json: "REG_BINARY: base64").
            json::append_string(out,
                                base64_encode(data.data(), data.size()));
            break;
        }
    }
}

// Map a Win32 LSTATUS from a registry open/query/set/delete into the spec
// error vocabulary for the calling verb. ERROR_ACCESS_DENIED -> the spec's
// `permission_denied` (the windows-modern x-families text: "HKLM writes
// typically require admin; ERR permission_denied without elevation"). The
// pre-2.1 handlers funnelled every non-not-found status into `not_supported`,
// which is OUTSIDE registry.key.read / registry.value.read / .delete /
// .key.delete x-errors — corrected to spec-declared codes; reported.
void write_status_err(Connection& conn, LSTATUS status) {
    if (status == ERROR_FILE_NOT_FOUND ||
        status == ERROR_PATH_NOT_FOUND) {
        conn.writer().write_err(ErrorCode::NotFound);
        return;
    }
    if (status == ERROR_ACCESS_DENIED) {
        char detail[48];
        std::snprintf(detail, sizeof(detail),
                      "{\"win32_error\":%ld}", status);
        conn.writer().write_err(ErrorCode::PermissionDenied, detail);
        return;
    }
    // Any other Win32 failure: closest spec-declared code is
    // permission_denied (all six registry verbs declare it). Carries the
    // raw status for diagnosis.
    char detail[48];
    std::snprintf(detail, sizeof(detail), "{\"win32_error\":%ld}", status);
    conn.writer().write_err(ErrorCode::PermissionDenied, detail);
}

// ---------------------------------------------------------------------------
// Encode caller `data` (a string per input_schema) into the registry byte
// representation for `type`. Returns false (and writes the spec-declared
// ERR) on a malformed value or a deferred/unsupported type.
//
// `verb` selects the x-errors set; create & update share an identical set
// ({already_exists|not_found, permission_denied, invalid_args,
// not_supported}) for the encode-failure cases handled here (invalid_args,
// not_supported).
bool encode_write_data(Connection& conn, std::string_view verb,
                       DWORD type, const std::string& data_str,
                       std::vector<BYTE>& bytes_out, DWORD& size_out) {
    switch (type) {
        case REG_SZ:
        case REG_EXPAND_SZ:
        case REG_LINK: {
            const std::wstring wstr = utf8_to_wide(data_str);
            size_out = static_cast<DWORD>((wstr.size() + 1) * sizeof(wchar_t));
            bytes_out.resize(size_out);
            std::memcpy(bytes_out.data(), wstr.c_str(), size_out);
            return true;
        }
        case REG_DWORD:
        case REG_DWORD_BIG_ENDIAN: {
            // input_schema: "decimal-stringified integer (or 0xHEX)".
            // strtoul base 0 honours an optional 0x prefix; reject junk so a
            // bad value surfaces as invalid_args, not a silent 0.
            char* end = nullptr;
            const unsigned long parsed =
                std::strtoul(data_str.c_str(), &end, 0);
            if (data_str.empty() || end != data_str.c_str() + data_str.size()) {
                invalid_args(conn, std::string(verb) +
                             " 'data' must be a decimal or 0xHEX integer "
                             "for REG_DWORD");
                return false;
            }
            DWORD v = static_cast<DWORD>(parsed);
            if (type == REG_DWORD_BIG_ENDIAN) {
                v = ((v & 0x000000ffu) << 24) | ((v & 0x0000ff00u) << 8) |
                    ((v & 0x00ff0000u) >> 8)  | ((v & 0xff000000u) >> 24);
            }
            bytes_out.resize(sizeof(v));
            std::memcpy(bytes_out.data(), &v, sizeof(v));
            size_out = sizeof(v);
            return true;
        }
        case REG_QWORD: {
            char* end = nullptr;
            const unsigned long long parsed =
                std::strtoull(data_str.c_str(), &end, 0);
            if (data_str.empty() || end != data_str.c_str() + data_str.size()) {
                invalid_args(conn, std::string(verb) +
                             " 'data' must be a decimal or 0xHEX integer "
                             "for REG_QWORD");
                return false;
            }
            const std::uint64_t v = static_cast<std::uint64_t>(parsed);
            bytes_out.resize(sizeof(v));
            std::memcpy(bytes_out.data(), &v, sizeof(v));
            size_out = sizeof(v);
            return true;
        }
        case REG_MULTI_SZ: {
            // input_schema: "lines separated by \n (agent converts to
            // null-separated)". Build a double-NUL-terminated wide block.
            std::wstring block;
            std::size_t start = 0;
            while (start <= data_str.size()) {
                const std::size_t nl = data_str.find('\n', start);
                const std::size_t end =
                    (nl == std::string::npos) ? data_str.size() : nl;
                const std::string line = data_str.substr(start, end - start);
                if (!line.empty()) {
                    block += utf8_to_wide(line);
                    block += L'\0';
                }
                if (nl == std::string::npos) break;
                start = nl + 1;
            }
            block += L'\0';  // terminating empty string for the block
            size_out = static_cast<DWORD>(block.size() * sizeof(wchar_t));
            bytes_out.resize(size_out);
            std::memcpy(bytes_out.data(), block.data(), size_out);
            return true;
        }
        case REG_BINARY: {
            // PHASE 2b — base64 `data` -> bytes decode is the binary
            // side-channel. The arg's presence + string shape are validated
            // by the caller (str() succeeded). The decode itself is deferred.
            // not_supported is within both create's and update's x-errors.
            conn.writer().write_err(
                ErrorCode::NotSupported,
                "{\"reason\":\"reg_binary_write_phase2b\","
                "\"message\":\"REG_BINARY base64 write is deferred to "
                "Phase 2b; string/dword/qword/multi_sz are supported\"}");
            return false;
        }
        case REG_NONE:
        default: {
            // REG_NONE write (and any unmodelled type that slipped past the
            // enum check) carries no encodable payload in this slice.
            conn.writer().write_err(
                ErrorCode::NotSupported,
                "{\"reason\":\"unsupported_write_type\"}");
            return false;
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// read — serves registry.key.read and registry.value.read (branch on verb).

void read(Connection& conn, const wire::Request& req) {
    const bool is_value = (req.verb == "registry.value.read");

    // input_schema property lists IN SCHEMA ORDER:
    //   registry.value.read : ["path","value"]
    //   registry.key.read   : ["path"]
    SchemaArgs args(req, is_value
                             ? std::initializer_list<std::string_view>{"path",
                                                                       "value"}
                             : std::initializer_list<std::string_view>{"path"});
    if (args.reject_unknown(conn)) return;

    HKEY root = nullptr;
    std::wstring subkey;
    if (!resolve_path(conn, args, req.verb, root, subkey)) return;

    HKEY hkey = nullptr;
    LSTATUS status = RegOpenKeyExW(root, subkey.c_str(), 0,
                                   KEY_READ | KEY_WOW64_64KEY, &hkey);
    if (status != ERROR_SUCCESS) {
        write_status_err(conn, status);
        return;
    }

    if (is_value) {
        // registry.value.read input_schema: required ["path","value"].
        std::optional<std::string> value = args.str("value");
        if (!value) {
            RegCloseKey(hkey);
            invalid_args(conn,
                         "registry.value.read requires 'value' (the value "
                         "name; \"\" for the (Default) value)");
            return;
        }
        const std::wstring value_name = utf8_to_wide(*value);

        DWORD type = 0;
        DWORD size = 0;
        status = RegQueryValueExW(hkey, value_name.c_str(), nullptr,
                                  &type, nullptr, &size);
        if (status != ERROR_SUCCESS) {
            RegCloseKey(hkey);
            write_status_err(conn, status);
            return;
        }
        std::vector<BYTE> buf(size);
        status = RegQueryValueExW(hkey, value_name.c_str(), nullptr,
                                  &type, buf.empty() ? nullptr : buf.data(),
                                  &size);
        RegCloseKey(hkey);
        if (status != ERROR_SUCCESS) {
            write_status_err(conn, status);
            return;
        }

        // x-output-schema: {type, data} both required,
        // additionalProperties:false.
        std::string body = "{";
        json::append_kv_string(body, "type", reg_type_to_string(type));
        body += ',';
        append_value_data(body, type, buf);
        body += '}';
        conn.writer().write_ok(body);
        return;
    }

    // registry.key.read x-output-schema:
    //   { subkeys:[str], values:[{name,type}] } both required,
    //   additionalProperties:false. Names + TYPES of values only — NO data
    //   (use registry.value.read for data). This is the v2.2 shape; the
    //   pre-2.1 {values:{name:{type,data}}} map is gone.
    std::string body = "{\"subkeys\":[";
    bool first = true;
    for (DWORD i = 0; ; ++i) {
        wchar_t namebuf[256];
        DWORD namelen = static_cast<DWORD>(std::size(namebuf));
        status = RegEnumKeyExW(hkey, i, namebuf, &namelen,
                               nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS) break;
        if (!first) body += ',';
        first = false;
        json::append_string(body, wide_to_utf8(namebuf, namelen));
    }
    body += "],\"values\":[";
    first = true;
    for (DWORD i = 0; ; ++i) {
        wchar_t namebuf[16384];  // max value-name length is 16383 chars
        DWORD namelen = static_cast<DWORD>(std::size(namebuf));
        DWORD type    = 0;
        status = RegEnumValueW(hkey, i, namebuf, &namelen, nullptr,
                               &type, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS && status != ERROR_MORE_DATA) break;
        if (!first) body += ',';
        first = false;
        body += '{';
        json::append_kv_string(body, "name", wide_to_utf8(namebuf, namelen));
        body += ',';
        json::append_kv_string(body, "type", reg_type_to_string(type));
        body += '}';
    }
    body += "]}";
    RegCloseKey(hkey);
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// write — serves registry.value.create and registry.value.update.
//
// Both: input_schema ["path","value","type","data"] (all required). The
// create-vs-update distinction is made explicit at this layer via a probe
// (Win32 RegSetValueExW itself does not distinguish):
//   create : RegCreateKeyExW (parent key created if missing) + probe; the
//            value being present -> ERR already_exists.
//   update : RegOpenKeyExW (key must exist) + probe; the value being absent
//            (or the key missing) -> ERR not_found.

void write(Connection& conn, const wire::Request& req) {
    const bool is_create = (req.verb == "registry.value.create");
    const std::string_view verb =
        is_create ? "registry.value.create" : "registry.value.update";

    SchemaArgs args(req, {"path", "value", "type", "data"});
    if (args.reject_unknown(conn)) return;

    HKEY root = nullptr;
    std::wstring subkey;
    if (!resolve_path(conn, args, verb, root, subkey)) return;

    // value (required string; "" => the (Default) value).
    std::optional<std::string> value = args.str("value");
    if (!value) {
        invalid_args(conn, std::string(verb) +
                     " requires 'value' (the value name; \"\" for the "
                     "(Default) value)");
        return;
    }
    const std::wstring value_name = utf8_to_wide(*value);

    // type (required string, enum).
    std::optional<std::string> type_str = args.str("type");
    if (!type_str) {
        invalid_args(conn, std::string(verb) +
                     " requires 'type' (REG_SZ, REG_DWORD, REG_QWORD, "
                     "REG_MULTI_SZ, REG_BINARY, ...)");
        return;
    }
    const DWORD type = parse_reg_type(*type_str);
    if (type == 0xFFFFFFFFu) {
        std::string detail = "{";
        json::append_kv_string(detail, "message",
                               std::string(verb) +
                               " 'type' is not a recognised registry type");
        detail += ',';
        json::append_kv_string(detail, "type", *type_str);
        detail += '}';
        conn.writer().write_err(ErrorCode::InvalidArgs, detail);
        return;
    }

    // data (required string; encoding per `type`).
    std::optional<std::string> data = args.str("data");
    if (!data) {
        invalid_args(conn, std::string(verb) +
                     " requires 'data' (a string, formatted per 'type')");
        return;
    }

    HKEY hkey = nullptr;
    if (is_create) {
        // RegCreateKeyExW creates the parent key if missing
        // (registry.value.create.json: "Creates the parent key if missing").
        LSTATUS status = RegCreateKeyExW(
            root, subkey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
            KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY,
            nullptr, &hkey, nullptr);
        if (status != ERROR_SUCCESS) {
            write_status_err(conn, status);
            return;
        }
        // Probe: value already present -> already_exists.
        DWORD probe_type = 0;
        LSTATUS probe = RegQueryValueExW(hkey, value_name.c_str(), nullptr,
                                         &probe_type, nullptr, nullptr);
        if (probe == ERROR_SUCCESS) {
            RegCloseKey(hkey);
            // registry.value.create.json x-errors: ["already_exists",...].
            conn.writer().write_err(
                ErrorCode::AlreadyExists,
                "{\"message\":\"value already exists; use "
                "registry.value.update to overwrite\"}");
            return;
        }
        if (probe != ERROR_FILE_NOT_FOUND) {
            RegCloseKey(hkey);
            write_status_err(conn, probe);
            return;
        }
    } else {
        // update: the key MUST already exist.
        LSTATUS status = RegOpenKeyExW(
            root, subkey.c_str(), 0,
            KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, &hkey);
        if (status != ERROR_SUCCESS) {
            write_status_err(conn, status);
            return;
        }
        // Probe: value absent -> not_found (registry.value.update.json:
        // "Returns ERR not_found when the key is missing OR the value is
        // absent").
        DWORD probe_type = 0;
        LSTATUS probe = RegQueryValueExW(hkey, value_name.c_str(), nullptr,
                                         &probe_type, nullptr, nullptr);
        if (probe == ERROR_FILE_NOT_FOUND) {
            RegCloseKey(hkey);
            conn.writer().write_err(ErrorCode::NotFound);
            return;
        }
        if (probe != ERROR_SUCCESS) {
            RegCloseKey(hkey);
            write_status_err(conn, probe);
            return;
        }
    }

    std::vector<BYTE> bytes;
    DWORD bytes_size = 0;
    if (!encode_write_data(conn, verb, type, *data, bytes, bytes_size)) {
        RegCloseKey(hkey);  // encode_write_data already wrote the ERR
        return;
    }

    LSTATUS status = RegSetValueExW(hkey, value_name.c_str(), 0, type,
                                    bytes.empty() ? nullptr : bytes.data(),
                                    bytes_size);
    RegCloseKey(hkey);
    if (status != ERROR_SUCCESS) {
        write_status_err(conn, status);
        return;
    }
    conn.writer().write_ok();  // x-output-schema: null (OK 0)
}

// ---------------------------------------------------------------------------
// delete_ — serves registry.value.delete and registry.key.delete.

void delete_(Connection& conn, const wire::Request& req) {
    const bool is_value = (req.verb == "registry.value.delete");
    const std::string_view verb =
        is_value ? "registry.value.delete" : "registry.key.delete";

    // input_schema property lists IN SCHEMA ORDER:
    //   registry.value.delete : ["path","value"]
    //   registry.key.delete   : ["path","recursive"]
    SchemaArgs args(req, is_value
                             ? std::initializer_list<std::string_view>{"path",
                                                                       "value"}
                             : std::initializer_list<std::string_view>{
                                   "path", "recursive"});
    if (args.reject_unknown(conn)) return;

    HKEY root = nullptr;
    std::wstring subkey;
    if (!resolve_path(conn, args, verb, root, subkey)) return;

    if (is_value) {
        // registry.value.delete input_schema: required ["path","value"].
        std::optional<std::string> value = args.str("value");
        if (!value) {
            invalid_args(conn,
                         "registry.value.delete requires 'value' (the value "
                         "name; \"\" for the (Default) value)");
            return;
        }
        const std::wstring value_name = utf8_to_wide(*value);

        HKEY hkey = nullptr;
        LSTATUS status = RegOpenKeyExW(root, subkey.c_str(), 0,
                                       KEY_SET_VALUE | KEY_WOW64_64KEY, &hkey);
        if (status != ERROR_SUCCESS) {
            write_status_err(conn, status);
            return;
        }
        status = RegDeleteValueW(hkey, value_name.c_str());
        RegCloseKey(hkey);
        if (status != ERROR_SUCCESS) {
            write_status_err(conn, status);
            return;
        }
        conn.writer().write_ok();  // x-output-schema: null (OK 0)
        return;
    }

    // registry.key.delete: optional `recursive` (default false).
    bool recursive = false;
    if (args.present("recursive")) {
        std::optional<bool> r = args.boolean("recursive");
        if (!r) {
            invalid_args(conn,
                         "registry.key.delete 'recursive' must be a boolean");
            return;
        }
        recursive = *r;
    }

    LSTATUS status;
    if (recursive) {
        // RegDeleteTreeW is Vista+; SHDeleteKeyW is XP-compatible and
        // equivalent for recursive subtree deletion (shlwapi already linked,
        // and the legacy family targets _WIN32_WINNT=0x0501).
        status = SHDeleteKeyW(root, subkey.c_str());
    } else {
        // Non-recursive: RegDeleteKeyW refuses a key that still has subkeys
        // on NT (returns ERROR_ACCESS_DENIED / a non-success status). That is
        // exactly the spec's `not_empty` case: "Without [recursive], deleting
        // a key with subkeys returns ERR not_empty." Detect the has-subkeys
        // condition explicitly so it maps to the right code rather than
        // permission_denied.
        HKEY probe = nullptr;
        if (RegOpenKeyExW(root, subkey.c_str(), 0,
                          KEY_READ | KEY_WOW64_64KEY, &probe)
                == ERROR_SUCCESS) {
            DWORD subkeys = 0;
            RegQueryInfoKeyW(probe, nullptr, nullptr, nullptr, &subkeys,
                             nullptr, nullptr, nullptr, nullptr, nullptr,
                             nullptr, nullptr);
            RegCloseKey(probe);
            if (subkeys > 0) {
                // registry.key.delete.json x-errors: [...,"not_empty",...].
                conn.writer().write_err(
                    ErrorCode::NotEmpty,
                    "{\"message\":\"key has subkeys; pass recursive:true to "
                    "delete the whole subtree\"}");
                return;
            }
        }
        status = RegDeleteKeyW(root, subkey.c_str());
    }

    if (status != ERROR_SUCCESS) {
        write_status_err(conn, status);
        return;
    }
    conn.writer().write_ok();  // x-output-schema: null (OK 0)
}

}  // namespace remote_hands::registry_verbs
