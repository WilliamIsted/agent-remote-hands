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

// `file.*` and `directory.*` namespace verb handlers (one translation unit;
// the wire-namespace split lives in agents/shared/capabilities.cpp).
//
// Implements the verbs whose contracts are the spec JSON under
// protocol/spec/verbs/common/{file,directory}.*.json (the single source of
// truth). Registered handlers (see capabilities.cpp):
//
//   file.read         (R)  read    {path,encoding,offset,length}
//   file.write        (U)  write   {path,content,encoding,atomic}
//   file.write_at     (U)  write_at{path,offset,content,encoding,truncate}
//   file.stat         (R)  stat    {path}
//   file.delete       (D)  delete_ {path}
//   file.exists       (R)  exists  {path}
//   file.wait         (R)  wait    {glob,timeout_ms}
//   file.rename       (U)  rename  {src,dst,overwrite,cross_fs}
//   directory.list    (R)  list          {path,recursive,pattern,limit,
//                                          offset,sort,reverse,full}
//   directory.stat    (R)  directory_stat   {path}
//   directory.exists  (R)  directory_exists {path}
//   directory.create  (C)  mkdir            {path,parents,mode}
//   directory.rename  (U)  directory_rename {src,dst,overwrite,cross_fs}
//   directory.delete  (D)  directory_remove {path,recursive}
//
// PHASE 2.1 — NAMED-ARG MIGRATION. These handlers no longer index `req.args`
// positionally with ad-hoc `--flag` scanning or whitespace-joined multi-token
// path reconstruction. Each declares its input_schema property list (IN
// SCHEMA ORDER) and reads each value by NAME through the shared SchemaArgs
// resolver (schema_args.hpp), with the schema's property ORDER used as the
// positional fallback when the caller invoked the verb positionally (the v2.2
// reference client packs positional calls as `{"_args":[...]}`). The
// `window.*` namespace was the pattern-setter; this file follows it (and
// system.cpp / process.cpp / clipboard.cpp / registry.cpp) exactly.
// Validation emits the same ErrorCode::InvalidArgs + {"message":...}
// ergonomics as before. v2.2 header quoting (PROTOCOL.md 1.2.5) and the MCP
// named-args object both deliver `path` as one string, taken verbatim.
//
// ISSUE #93 — `flags` array. file.stat / file.exists / directory.list /
// directory.stat emit a `flags` array: the Win32 FILE_ATTRIBUTE_* set sourced
// zero-cost from GetFileAttributesEx / WIN32_FIND_DATA output, mapped to the
// spec's exact flag enum tokens (readonly, hidden, system, archive, ...).
//
// BINARY PAYLOAD — PHASE 2b. file.read / file.write / file.write_at carry
// content via a schema `content` string + `encoding` discriminator (the v2.2
// binary side-channel). All NON-payload args (path / offset / length /
// encoding / atomic / truncate) are refactored to named here; the actual
// content decode/encode is the Phase-2b side-channel and is DEFERRED — the
// arg shape is validated, then the spec-declared deferral code is emitted
// (each of these three verbs' x-errors is exactly
// ["not_found","permission_denied","invalid_args"], so the deferral uses
// invalid_args with an explicit {"reason":...} — not_supported is NOT in
// their x-errors). See report. The positional-path implementation that read
// raw payload bytes off the wire is gone (it predated the schema `content`
// model and never matched the v2.2 contract).
//
// ERROR-CODE DISCIPLINE. Every emission is within the DISPATCHED verb's spec
// x-errors. `not_a_directory` (file.delete, directory.list, directory.stat,
// directory.rename, directory.delete) is emitted via ErrorCode::NotADirectory
// and `cross_device` (file.rename, directory.rename) via ErrorCode::CrossDevice
// — both added to agents/shared/errors.hpp during this migration.
//
// Paths are UTF-8 on the wire; converted to wide-char internally.
// FILETIME values are converted to Unix epoch seconds.

#include "../connection.hpp"
#include "../errors.hpp"
#include "../json.hpp"
#include "../log.hpp"
#include "../text_util.hpp"
#include "args.hpp"
#include "schema_args.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>   // PathMatchSpecW — directory.list `pattern` name filter
#include <winhttp.h>   // file.download — HTTP(S) client (XP SP3+: WinHTTP 5.1)

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "winhttp.lib")

namespace remote_hands::file_verbs {

// The Phase-2.1 named-argument resolver and its invalid_args helper live in
// the shared header (schema_args.hpp) so every namespace reads through one
// definition. Pull them into this TU's unqualified name lookup; behaviour is
// identical to window.cpp / system.cpp / process.cpp / clipboard.cpp /
// registry.cpp.
using wire::SchemaArgs;
using wire::invalid_args;

namespace {

constexpr long long kFiletimeUnixDelta = 116444736000000000LL;  // 100ns intervals

long long filetime_to_unix(const FILETIME& ft) {
    ULARGE_INTEGER u{};
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (static_cast<long long>(u.QuadPart) - kFiletimeUnixDelta) / 10000000LL;
}

// Spec type enum for file.stat / file.exists / file.wait / directory.list:
// "file" | "directory" | "link" | "other". (file.exists additionally uses
// "absent" when exists is false; that string is emitted at the call site.)
// Reparse points -> "link"; directories -> "directory"; everything else with
// a valid attribute set -> "file" (regular file). INVALID_FILE_ATTRIBUTES is
// the caller's "absent" signal and is handled before this is reached.
std::string_view attribute_type(DWORD attrs) {
    if (attrs == INVALID_FILE_ATTRIBUTES)        return "other";
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT)    return "link";
    if (attrs & FILE_ATTRIBUTE_DIRECTORY)        return "directory";
    return "file";
}

// Issue #93 — append a `flags` array of the spec's FILE_ATTRIBUTE_* tokens
// currently set in `attrs`. Tokens are exactly the spec enum strings; the
// cloud-file flags (recall_on_*) are Windows 10+ and named via numeric
// constants so the legacy/XP toolchain (no Win10 SDK headers required) still
// compiles. Emitted in the spec enum's declared order. The key is appended
// (so the caller positions the comma); the value is a JSON array.
void append_flags_array(std::string& out, DWORD attrs) {
    // Win10-only attribute bits, defined numerically for toolchains whose
    // SDK predates them (FILE_ATTRIBUTE_RECALL_ON_* — winnt.h, Win 8/10).
    constexpr DWORD kAttrRecallOnOpen        = 0x00040000;  // RECALL_ON_OPEN
    constexpr DWORD kAttrRecallOnDataAccess  = 0x00400000;  // RECALL_ON_DATA_ACCESS

    json::append_string(out, "flags");
    out += ":[";
    bool first = true;
    auto emit = [&](bool set, std::string_view token) {
        if (!set) return;
        if (!first) out += ',';
        first = false;
        json::append_string(out, token);
    };
    emit((attrs & FILE_ATTRIBUTE_READONLY)      != 0, "readonly");
    emit((attrs & FILE_ATTRIBUTE_HIDDEN)        != 0, "hidden");
    emit((attrs & FILE_ATTRIBUTE_SYSTEM)        != 0, "system");
    emit((attrs & FILE_ATTRIBUTE_ARCHIVE)       != 0, "archive");
    emit((attrs & FILE_ATTRIBUTE_TEMPORARY)     != 0, "temporary");
    emit((attrs & FILE_ATTRIBUTE_COMPRESSED)    != 0, "compressed");
    emit((attrs & FILE_ATTRIBUTE_ENCRYPTED)     != 0, "encrypted");
    emit((attrs & FILE_ATTRIBUTE_SPARSE_FILE)   != 0, "sparse_file");
    emit((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0, "reparse_point");
    emit((attrs & FILE_ATTRIBUTE_OFFLINE)       != 0, "offline");
    emit((attrs & FILE_ATTRIBUTE_NOT_CONTENT_INDEXED) != 0, "not_indexed");
    emit((attrs & kAttrRecallOnDataAccess)      != 0, "recall_on_data_access");
    emit((attrs & kAttrRecallOnOpen)            != 0, "recall_on_open");
    out += ']';
}

// Resolve the required `path` property. Emits the spec-declared invalid_args
// on a missing/empty path (invalid_args is in EVERY file/directory verb's
// x-errors). The pre-2.1 multi-token path-reconstruction is gone.
bool resolve_path(Connection& conn, SchemaArgs& args, std::string_view verb,
                  std::string& path_out) {
    std::optional<std::string> path = args.str("path");
    if (!path || path->empty()) {
        invalid_args(conn, std::string(verb) + " requires 'path'");
        return false;
    }
    path_out = std::move(*path);
    return true;
}

// Map a Win32 GetLastError() into the spec error vocabulary for a file/dir
// open/query failure, constrained to the codes the calling verb declares.
// Every file/dir x-errors set that uses this contains BOTH not_found and
// permission_denied (read/write/stat/delete/rename/list). The pre-2.1
// helper funnelled non-not-found failures into `not_supported`, which is
// OUTSIDE every one of these verbs' x-errors — corrected to the spec-declared
// permission_denied (carrying the raw win32 status for diagnosis); reported.
void write_open_err(Connection& conn, std::string_view path) {
    const DWORD err = GetLastError();
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND ||
        err == ERROR_INVALID_NAME   || err == ERROR_BAD_NETPATH    ||
        err == ERROR_BAD_PATHNAME) {
        std::string detail = "{";
        json::append_kv_string(detail, "path", path);
        detail += '}';
        conn.writer().write_err(ErrorCode::NotFound, detail);
        return;
    }
    char detail[96];
    std::snprintf(detail, sizeof(detail), "{\"win32_error\":%lu}", err);
    conn.writer().write_err(ErrorCode::PermissionDenied, detail);
}

// Decode standard base64 (RFC 4648) into bytes. Skips whitespace; stops at
// '='. File-local — the shared layer has no base64 helper and the scope is
// file.cpp only (same rationale as registry.cpp's local base64_encode and
// vision.cpp's local base64_decode). Returns false on a malformed alphabet
// character so a bad payload surfaces as the caller's invalid_args.
bool base64_decode(std::string_view s, std::vector<unsigned char>& out) {
    auto char_val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out.clear();
    out.reserve((s.size() * 3) / 4);
    int val = 0, bits = 0;
    for (char c : s) {
        if (c == '=') break;
        if (c == ' ' || c == '\r' || c == '\n' || c == '\t') continue;
        const int v = char_val(c);
        if (v < 0) return false;
        val = (val << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>(val >> bits));
            val &= (1 << bits) - 1;
        }
    }
    return true;
}

// Resolve file.create's content to the exact bytes to write.
//
// Two delivery shapes, both spec-aligned:
//   1. schema `content` (string) + `encoding` discriminator
//      (PROTOCOL.md §1.6 wire framing: file verbs carry `content` plus an
//      `encoding`; when encoding=binary the string is base64). Non-binary
//      encodings transcode the decoded JSON text via the named Windows code
//      page; binary base64-decodes.
//   2. `content_b64` — the conformance harness / reference-client binary
//      side-channel (wire.py _args_to_dict packs a `payload=` kwarg here;
//      mcp_session keeps it named-only). Always base64 of the raw bytes.
//
// On success `bytes_out` holds the file content and `encoding_echo` is the
// encoding string to echo in the response. Returns false having already
// written the spec-declared invalid_args.
bool resolve_create_content(Connection& conn, SchemaArgs& args,
                            const wire::Request& req,
                            std::vector<unsigned char>& bytes_out,
                            std::string& encoding_echo) {
    // Side-channel first: if content_b64 is present it is the authoritative
    // raw-bytes carrier (the harness sends a numeric `content` placeholder —
    // the byte length — alongside it, so content_b64 must win).
    if (const mcp::JsonValue* cb = wire::arg_node(req, "content_b64");
        cb != nullptr && cb->is_string()) {
        if (!base64_decode(cb->as_string(), bytes_out)) {
            invalid_args(conn,
                         "file.create 'content_b64' is not valid base64");
            return false;
        }
        encoding_echo = "binary";
        return true;
    }

    if (!args.present("content")) {
        invalid_args(conn, "file.create requires 'content'");
        return false;
    }
    std::optional<std::string> content = args.str("content");
    if (!content) {
        invalid_args(conn, "file.create 'content' must be a string");
        return false;
    }

    std::string encoding = "utf-8";   // schema default
    if (args.present("encoding")) {
        auto enc = args.str("encoding");
        if (!enc) {
            invalid_args(conn, "file.create 'encoding' must be a string");
            return false;
        }
        encoding = *enc;
    }
    encoding_echo = encoding;

    if (encoding == "binary") {
        if (!base64_decode(*content, bytes_out)) {
            invalid_args(conn,
                         "file.create 'content' is not valid base64 "
                         "(encoding=binary)");
            return false;
        }
        return true;
    }

    // Text encodings. `content` is already-decoded UTF-8 JSON text. Map the
    // spec encoding enum to a Win32 code page / UTF-16 transform.
    if (encoding == "utf-8" || encoding == "ascii" ||
        encoding == "latin-1" || encoding == "cp1252") {
        if (encoding == "utf-8") {
            bytes_out.assign(content->begin(), content->end());
            return true;
        }
        // Transcode UTF-8 -> UTF-16 -> target single-byte code page.
        const UINT cp = (encoding == "ascii")   ? 20127u   // US-ASCII
                       : (encoding == "latin-1") ? 28591u   // ISO-8859-1
                                                 : 1252u;   // cp1252
        const int wlen = MultiByteToWideChar(
            CP_UTF8, 0, content->data(),
            static_cast<int>(content->size()), nullptr, 0);
        std::wstring w(static_cast<std::size_t>(wlen > 0 ? wlen : 0), L'\0');
        if (wlen > 0) {
            MultiByteToWideChar(CP_UTF8, 0, content->data(),
                                static_cast<int>(content->size()),
                                w.data(), wlen);
        }
        const int blen = WideCharToMultiByte(cp, 0, w.data(),
                                             static_cast<int>(w.size()),
                                             nullptr, 0, nullptr, nullptr);
        bytes_out.assign(static_cast<std::size_t>(blen > 0 ? blen : 0), 0);
        if (blen > 0) {
            WideCharToMultiByte(
                cp, 0, w.data(), static_cast<int>(w.size()),
                reinterpret_cast<char*>(bytes_out.data()), blen,
                nullptr, nullptr);
        }
        return true;
    }
    if (encoding == "utf-16le" || encoding == "utf-16be") {
        const int wlen = MultiByteToWideChar(
            CP_UTF8, 0, content->data(),
            static_cast<int>(content->size()), nullptr, 0);
        std::wstring w(static_cast<std::size_t>(wlen > 0 ? wlen : 0), L'\0');
        if (wlen > 0) {
            MultiByteToWideChar(CP_UTF8, 0, content->data(),
                                static_cast<int>(content->size()),
                                w.data(), wlen);
        }
        bytes_out.clear();
        bytes_out.reserve(w.size() * 2);
        const bool be = (encoding == "utf-16be");
        for (wchar_t ch : w) {
            const unsigned u = static_cast<unsigned short>(ch);
            if (be) {
                bytes_out.push_back(static_cast<unsigned char>(u >> 8));
                bytes_out.push_back(static_cast<unsigned char>(u & 0xFF));
            } else {
                bytes_out.push_back(static_cast<unsigned char>(u & 0xFF));
                bytes_out.push_back(static_cast<unsigned char>(u >> 8));
            }
        }
        return true;
    }

    invalid_args(conn,
                 "file.create 'encoding' must be one of utf-8|utf-16le|"
                 "utf-16be|ascii|latin-1|cp1252|binary");
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// file.read — input_schema (schema order): ["path","encoding","offset","length"]
// x-errors: ["not_found","permission_denied","invalid_args"].
//
// PHASE 2b — the `content` output is the binary side-channel (decoded text in
// the requested encoding, or base64 for `encoding: binary`). Path / encoding
// / offset / length are validated here as named args; the byte read +
// encode is deferred. not_supported is NOT in this verb's x-errors, so the
// deferral is surfaced via the spec-declared invalid_args with an explicit
// {"reason":...}.

void read(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"path", "encoding", "offset", "length"});
    if (args.reject_unknown(conn)) return;

    std::string path;
    if (!resolve_path(conn, args, "file.read", path)) return;

    if (args.present("encoding")) {
        auto enc = args.str("encoding");
        if (!enc) {
            invalid_args(conn, "file.read 'encoding' must be a string");
            return;
        }
    }
    if (args.present("offset")) {
        auto off = args.integer("offset");
        if (!off || *off < 0) {
            invalid_args(conn,
                         "file.read 'offset' must be a non-negative integer");
            return;
        }
    }
    if (args.present("length")) {
        auto len = args.integer("length");
        if (!len || *len < 0) {
            invalid_args(conn,
                         "file.read 'length' must be a non-negative integer");
            return;
        }
    }

    // PHASE 2b — content read/encode is the binary side-channel; deferred.
    invalid_args(conn,
                 "file.read content side-channel is deferred to Phase 2b "
                 "(reason: file_content_phase2b)");
}

// ---------------------------------------------------------------------------
// file.write — input_schema (schema order): ["path","content","encoding",
// "atomic"]. x-errors: ["not_found","permission_denied","invalid_args"].
//
// PHASE 2b — `content` is the binary side-channel. Path / encoding / atomic
// are validated as named args; the decode + write is deferred via the
// spec-declared invalid_args (not_supported is NOT in this verb's x-errors).

void write(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"path", "content", "encoding", "atomic"});
    if (args.reject_unknown(conn)) return;

    std::string path;
    if (!resolve_path(conn, args, "file.write", path)) return;

    // `content` is REQUIRED by file.write.json; validate presence + string
    // shape (the actual decode is the deferred side-channel).
    if (!args.present("content")) {
        invalid_args(conn, "file.write requires 'content'");
        return;
    }
    if (!args.str("content")) {
        invalid_args(conn, "file.write 'content' must be a string");
        return;
    }
    if (args.present("encoding") && !args.str("encoding")) {
        invalid_args(conn, "file.write 'encoding' must be a string");
        return;
    }
    if (args.present("atomic") && !args.boolean("atomic")) {
        invalid_args(conn, "file.write 'atomic' must be a boolean");
        return;
    }

    // PHASE 2b — content decode/write is the binary side-channel; deferred.
    invalid_args(conn,
                 "file.write content side-channel is deferred to Phase 2b "
                 "(reason: file_content_phase2b)");
}

// ---------------------------------------------------------------------------
// file.write_at — input_schema (schema order): ["path","offset","content",
// "encoding","truncate"]. x-errors:
// ["not_found","permission_denied","invalid_args"].
// x-conditional: truncate:true requires offset:0 (else invalid_args).
//
// PHASE 2b — `content` is the binary side-channel. Path / offset / encoding /
// truncate (incl. the offset==0 conditional) are validated as named args;
// the decode + random-access write is deferred via invalid_args.

void write_at(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"path", "offset", "content", "encoding", "truncate"});
    if (args.reject_unknown(conn)) return;

    std::string path;
    if (!resolve_path(conn, args, "file.write_at", path)) return;

    // offset — required, non-negative integer.
    if (!args.present("offset")) {
        invalid_args(conn, "file.write_at requires 'offset'");
        return;
    }
    auto offset = args.integer("offset");
    if (!offset || *offset < 0) {
        invalid_args(conn,
                     "file.write_at 'offset' must be a non-negative integer");
        return;
    }

    // content — required string (the deferred side-channel).
    if (!args.present("content")) {
        invalid_args(conn, "file.write_at requires 'content'");
        return;
    }
    if (!args.str("content")) {
        invalid_args(conn, "file.write_at 'content' must be a string");
        return;
    }

    if (args.present("encoding") && !args.str("encoding")) {
        invalid_args(conn, "file.write_at 'encoding' must be a string");
        return;
    }

    bool truncate = false;
    if (args.present("truncate")) {
        auto t = args.boolean("truncate");
        if (!t) {
            invalid_args(conn,
                         "file.write_at 'truncate' must be a boolean");
            return;
        }
        truncate = *t;
    }
    // x-conditional: truncate:true requires offset:0 -> invalid_args.
    if (truncate && *offset != 0) {
        invalid_args(conn,
                     "file.write_at 'truncate: true' requires 'offset: 0'");
        return;
    }

    // PHASE 2b — content decode + random-access write is the binary
    // side-channel; deferred.
    invalid_args(conn,
                 "file.write_at content side-channel is deferred to Phase 2b "
                 "(reason: file_content_phase2b)");
}

// ---------------------------------------------------------------------------
// file.create — input_schema (schema order): ["path","content","encoding",
// "atomic"]. required: ["path","content"]. x-crudx: C. x-errors:
// ["not_found","already_exists","permission_denied","invalid_args"].
// x-output-schema: {bytes_written, encoding} both required.
//
// CRUDX split of file.write: a Create-tier verb that REFUSES to overwrite
// (CREATE_NEW disposition / no MOVEFILE_REPLACE_EXISTING) — an existing
// target is already_exists, not a silent overwrite. Atomic by default: write
// a sibling temp file then MoveFileExW into place. Unlike file.write (whose
// content path is the deferred Phase-2b side-channel), file.create lands the
// bytes here — it is the explicit R2 Create-tier file-creation deliverable.

void create(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"path", "content", "encoding", "atomic"});
    if (args.reject_unknown(conn)) return;

    std::string path;
    if (!resolve_path(conn, args, "file.create", path)) return;

    std::vector<unsigned char> bytes;
    std::string encoding_echo;
    if (!resolve_create_content(conn, args, req, bytes, encoding_echo)) {
        return;   // helper already wrote invalid_args
    }

    bool atomic = true;   // schema default
    if (args.present("atomic")) {
        auto a = args.boolean("atomic");
        if (!a) {
            invalid_args(conn, "file.create 'atomic' must be a boolean");
            return;
        }
        atomic = *a;
    }

    const std::wstring wpath = text::utf8_to_wide(path);

    // Write helper: CREATE_NEW so an existing target fails (mapped to
    // already_exists). Returns the Win32 status (0 == success).
    auto write_new = [&](const std::wstring& target) -> DWORD {
        HANDLE h = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return GetLastError();
        DWORD written = 0;
        bool ok = true;
        if (!bytes.empty()) {
            ok = WriteFile(h, bytes.data(),
                           static_cast<DWORD>(bytes.size()),
                           &written, nullptr) != 0 &&
                 written == static_cast<DWORD>(bytes.size());
        }
        const DWORD werr = ok ? 0u : GetLastError();
        CloseHandle(h);
        return werr;
    };

    auto map_create_err = [&](DWORD err) {
        if (err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS) {
            conn.writer().write_err(
                ErrorCode::AlreadyExists,
                "{\"message\":\"file already exists; use file.write to "
                "overwrite\"}");
            return;
        }
        if (err == ERROR_PATH_NOT_FOUND || err == ERROR_FILE_NOT_FOUND ||
            err == ERROR_INVALID_NAME) {
            conn.writer().write_err(
                ErrorCode::NotFound,
                "{\"message\":\"parent directory does not exist "
                "(use directory.create first)\"}");
            return;
        }
        char detail[64];
        std::snprintf(detail, sizeof(detail), "{\"win32_error\":%lu}", err);
        conn.writer().write_err(ErrorCode::PermissionDenied, detail);
    };

    if (atomic) {
        // Sibling temp file then MoveFileExW WITHOUT MOVEFILE_REPLACE_EXISTING
        // so an existing target fails the rename (surfaced as already_exists),
        // matching file.create.json's create_file_w_atomic_new
        // implementation. Probe the final target up front so the
        // already_exists answer is precise and no temp file is left behind.
        if (GetFileAttributesW(wpath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            conn.writer().write_err(
                ErrorCode::AlreadyExists,
                "{\"message\":\"file already exists; use file.write to "
                "overwrite\"}");
            return;
        }
        std::wstring tmp = wpath;
        tmp += L".rh-tmp";
        // Best-effort clean of a stale temp from a previous aborted create.
        DeleteFileW(tmp.c_str());
        const DWORD werr = write_new(tmp);
        if (werr != 0) {
            DeleteFileW(tmp.c_str());   // don't leak the partial temp
            map_create_err(werr);
            return;
        }
        if (!MoveFileExW(tmp.c_str(), wpath.c_str(),
                         MOVEFILE_WRITE_THROUGH)) {
            const DWORD merr = GetLastError();
            DeleteFileW(tmp.c_str());   // don't leak the temp
            map_create_err(merr);
            return;
        }
    } else {
        const DWORD werr = write_new(wpath);
        if (werr != 0) {
            map_create_err(werr);
            return;
        }
    }

    std::string body = "{";
    json::append_kv_uint(body, "bytes_written",
                         static_cast<unsigned long long>(bytes.size()));
    body += ',';
    json::append_kv_string(body, "encoding", encoding_echo);
    body += '}';
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// file.download — input_schema (schema order): ["url","local_path","method",
// "headers","max_bytes","atomic","create_only","follow_redirects"].
// required: ["url","local_path"]. x-crudx: C. x-errors: ["not_found",
// "already_exists","permission_denied","size_limit_exceeded",
// "no_implementation_available","transfer_failed"]. x-output-schema:
// {bytes_written, status_code?, implementation_used} (bytes_written +
// implementation_used required).
//
// IMPLEMENTATION CHOICE — WinHTTP (winhttp.lib / winhttp.h). The spec's
// x-implementations name an external-tool chain (curl/wget/powershell-bits);
// the agent has no inline HTTP client of its own and shelling out to those
// tools is brittle (PATH, install hint surfacing, the no-self-installer
// constraint). WinHTTP is an OS library (no new third-party dep) present on
// EVERY family floor including the legacy floor:
//   * Windows XP SP3 / Server 2003 ship WinHTTP 5.1 (winhttp.dll + the
//     v7.1A-era SDK winhttp.h / winhttp.lib the v141_xp toolchain links).
//   * Modern Windows ships a newer WinHTTP with the same API surface used
//     here. WINVER=0x0501 (the legacy build) keeps the API to the XP-safe
//     subset (WinHttpOpen/Connect/OpenRequest/SendRequest/ReceiveResponse/
//     QueryDataAvailable/ReadData/QueryHeaders — all XP SP3).
// WinHTTP is also the Microsoft-recommended client for non-interactive /
// service contexts (unlike WinINet, which is unsupported from a service).
// `implementation_used` is reported as "winhttp".

void download(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"url", "local_path", "method", "headers",
                          "max_bytes", "atomic", "create_only",
                          "follow_redirects"});
    if (args.reject_unknown(conn)) return;

    std::optional<std::string> url = args.str("url");
    if (!url || url->empty()) {
        invalid_args(conn, "file.download requires 'url'");
        return;
    }
    std::optional<std::string> local_path = args.str("local_path");
    if (!local_path || local_path->empty()) {
        invalid_args(conn, "file.download requires 'local_path'");
        return;
    }

    std::string method = "GET";
    if (args.present("method")) {
        auto m = args.str("method");
        if (!m || (*m != "GET" && *m != "HEAD")) {
            invalid_args(conn,
                         "file.download 'method' must be GET or HEAD");
            return;
        }
        method = *m;
    }

    long long max_bytes = -1;   // -1 == no cap
    if (args.present("max_bytes")) {
        auto mb = args.integer("max_bytes");
        if (!mb || *mb < 0) {
            invalid_args(conn,
                         "file.download 'max_bytes' must be a "
                         "non-negative integer");
            return;
        }
        max_bytes = *mb;
    }

    bool atomic = true;
    if (args.present("atomic")) {
        auto a = args.boolean("atomic");
        if (!a) {
            invalid_args(conn, "file.download 'atomic' must be a boolean");
            return;
        }
        atomic = *a;
    }

    bool create_only = false;
    if (args.present("create_only")) {
        auto c = args.boolean("create_only");
        if (!c) {
            invalid_args(conn,
                         "file.download 'create_only' must be a boolean");
            return;
        }
        create_only = *c;
    }

    bool follow_redirects = true;
    if (args.present("follow_redirects")) {
        auto f = args.boolean("follow_redirects");
        if (!f) {
            invalid_args(conn,
                         "file.download 'follow_redirects' must be a "
                         "boolean");
            return;
        }
        follow_redirects = *f;
    }

    // Optional request headers — array of "Header-Name: value" strings.
    std::wstring extra_headers;
    if (const mcp::JsonValue* hn = wire::arg_node(req, "headers");
        hn != nullptr && hn->is_array()) {
        for (const mcp::JsonValue& el : hn->as_array()) {
            if (!el.is_string()) {
                invalid_args(conn,
                             "file.download 'headers' entries must be "
                             "strings (\"Name: value\")");
                return;
            }
            extra_headers += text::utf8_to_wide(el.as_string());
            extra_headers += L"\r\n";
        }
    }

    if (create_only &&
        GetFileAttributesW(text::utf8_to_wide(*local_path).c_str())
            != INVALID_FILE_ATTRIBUTES) {
        conn.writer().write_err(
            ErrorCode::AlreadyExists,
            "{\"message\":\"local_path already exists "
            "(create_only:true)\"}");
        return;
    }

    // --- Parse the URL via WinHttpCrackUrl --------------------------------
    const std::wstring wurl = text::utf8_to_wide(*url);
    URL_COMPONENTS uc{};
    uc.dwStructSize     = sizeof(uc);
    wchar_t host[256]   = {0};
    wchar_t urlpath[2048] = {0};
    wchar_t scheme[16]  = {0};
    uc.lpszHostName     = host;     uc.dwHostNameLength    = 255;
    uc.lpszUrlPath      = urlpath;  uc.dwUrlPathLength     = 2047;
    uc.lpszScheme       = scheme;   uc.dwSchemeLength      = 15;
    if (!WinHttpCrackUrl(wurl.c_str(),
                         static_cast<DWORD>(wurl.size()), 0, &uc)) {
        invalid_args(conn,
                     "file.download 'url' is not a valid absolute "
                     "http/https URL");
        return;
    }
    if (uc.nScheme != INTERNET_SCHEME_HTTP &&
        uc.nScheme != INTERNET_SCHEME_HTTPS) {
        invalid_args(conn,
                     "file.download 'url' scheme must be http or https");
        return;
    }
    const bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    // --- WinHTTP session/connect/request ----------------------------------
    HINTERNET hSession = WinHttpOpen(
        L"agent-remote-hands/0.3 (file.download; WinHTTP)",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        char d[64];
        std::snprintf(d, sizeof(d),
                      "{\"win32_error\":%lu}", GetLastError());
        conn.writer().write_err(ErrorCode::TransferFailed, d);
        return;
    }

    HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    if (!hConnect) {
        char d[64];
        std::snprintf(d, sizeof(d),
                      "{\"win32_error\":%lu}", GetLastError());
        WinHttpCloseHandle(hSession);
        conn.writer().write_err(ErrorCode::TransferFailed, d);
        return;
    }

    DWORD req_flags = https ? WINHTTP_FLAG_SECURE : 0u;
    const std::wstring wmethod = text::utf8_to_wide(method);
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, wmethod.c_str(), urlpath, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, req_flags);
    if (!hRequest) {
        char d[64];
        std::snprintf(d, sizeof(d),
                      "{\"win32_error\":%lu}", GetLastError());
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        conn.writer().write_err(ErrorCode::TransferFailed, d);
        return;
    }

    if (!follow_redirects) {
        DWORD opt = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_DISABLE_FEATURE,
                         &opt, sizeof(opt));
    }

    auto close_all = [&]() {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
    };

    const LPCWSTR hdr_ptr =
        extra_headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                              : extra_headers.c_str();
    const DWORD hdr_len =
        extra_headers.empty() ? 0u
                              : static_cast<DWORD>(extra_headers.size());
    if (!WinHttpSendRequest(hRequest, hdr_ptr, hdr_len,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest, nullptr)) {
        const DWORD err = GetLastError();
        close_all();
        if (err == ERROR_WINHTTP_NAME_NOT_RESOLVED ||
            err == ERROR_WINHTTP_CANNOT_CONNECT) {
            conn.writer().write_err(
                ErrorCode::TransferFailed,
                "{\"message\":\"host unreachable / name not resolved\"}");
            return;
        }
        char d[64];
        std::snprintf(d, sizeof(d), "{\"win32_error\":%lu}", err);
        conn.writer().write_err(ErrorCode::TransferFailed, d);
        return;
    }

    // HTTP status code.
    DWORD status = 0, slen = sizeof(status);
    WinHttpQueryHeaders(
        hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen,
        WINHTTP_NO_HEADER_INDEX);
    if (status == 404) {
        close_all();
        conn.writer().write_err(
            ErrorCode::NotFound,
            "{\"status_code\":404,\"message\":\"remote returned 404\"}");
        return;
    }
    if (status >= 400) {
        close_all();
        char d[96];
        std::snprintf(d, sizeof(d),
            "{\"status_code\":%lu,\"message\":\"HTTP error status\"}",
            status);
        conn.writer().write_err(ErrorCode::TransferFailed, d);
        return;
    }

    // --- Stream body to a temp file (atomic) or the target directly -------
    const std::wstring wdst = text::utf8_to_wide(*local_path);
    std::wstring target = wdst;
    if (atomic) {
        target += L".rh-tmp";
        DeleteFileW(target.c_str());
    }
    HANDLE hFile = CreateFileW(
        target.c_str(), GENERIC_WRITE, 0, nullptr,
        create_only && !atomic ? CREATE_NEW : CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        const DWORD ferr = GetLastError();
        close_all();
        if (ferr == ERROR_FILE_EXISTS || ferr == ERROR_ALREADY_EXISTS) {
            conn.writer().write_err(
                ErrorCode::AlreadyExists,
                "{\"message\":\"local_path already exists\"}");
            return;
        }
        if (ferr == ERROR_PATH_NOT_FOUND) {
            conn.writer().write_err(
                ErrorCode::NotFound,
                "{\"message\":\"local_path parent directory missing\"}");
            return;
        }
        char d[64];
        std::snprintf(d, sizeof(d), "{\"win32_error\":%lu}", ferr);
        conn.writer().write_err(ErrorCode::PermissionDenied, d);
        return;
    }

    unsigned long long total = 0;
    bool ok = true;
    bool over_limit = false;
    // HEAD: no body to read; an empty file is the documented behaviour.
    if (method != "HEAD") {
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &avail)) {
                ok = false;
                break;
            }
            if (avail == 0) break;
            std::vector<char> chunk(avail);
            DWORD got = 0;
            if (!WinHttpReadData(hRequest, chunk.data(), avail, &got) ||
                got == 0) {
                if (got == 0) break;   // clean EOF
                ok = false;
                break;
            }
            if (max_bytes >= 0 &&
                total + got > static_cast<unsigned long long>(max_bytes)) {
                over_limit = true;
                break;
            }
            DWORD wrote = 0;
            if (!WriteFile(hFile, chunk.data(), got, &wrote, nullptr) ||
                wrote != got) {
                ok = false;
                break;
            }
            total += got;
        }
    }
    CloseHandle(hFile);
    close_all();

    if (over_limit) {
        DeleteFileW(target.c_str());
        char d[96];
        std::snprintf(d, sizeof(d),
            "{\"limit\":%lld,\"message\":\"max_bytes exceeded\"}",
            max_bytes);
        conn.writer().write_err(ErrorCode::SizeLimitExceeded, d);
        return;
    }
    if (!ok) {
        DeleteFileW(target.c_str());
        conn.writer().write_err(
            ErrorCode::TransferFailed,
            "{\"message\":\"transfer aborted mid-body\"}");
        return;
    }

    if (atomic) {
        // create_only honoured at rename time (no MOVEFILE_REPLACE_EXISTING
        // -> existing target fails as already_exists). When overwrite is
        // allowed, replace existing.
        DWORD mv_flags = MOVEFILE_WRITE_THROUGH;
        if (!create_only) mv_flags |= MOVEFILE_REPLACE_EXISTING;
        if (!MoveFileExW(target.c_str(), wdst.c_str(), mv_flags)) {
            const DWORD merr = GetLastError();
            DeleteFileW(target.c_str());
            if (merr == ERROR_ALREADY_EXISTS ||
                merr == ERROR_FILE_EXISTS) {
                conn.writer().write_err(
                    ErrorCode::AlreadyExists,
                    "{\"message\":\"local_path already exists "
                    "(create_only:true)\"}");
                return;
            }
            char d[64];
            std::snprintf(d, sizeof(d),
                          "{\"win32_error\":%lu}", merr);
            conn.writer().write_err(ErrorCode::PermissionDenied, d);
            return;
        }
    }

    std::string body = "{";
    json::append_kv_uint(body, "bytes_written", total);
    body += ',';
    json::append_kv_int(body, "status_code",
                        static_cast<long long>(status));
    body += ',';
    json::append_kv_string(body, "implementation_used", "winhttp");
    body += '}';
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// directory.list — input_schema (schema order): ["path","recursive",
// "pattern","limit","offset","sort","reverse","full"].
// x-errors: ["not_found","not_a_directory","permission_denied",
// "invalid_args"]. The non-directory case emits ErrorCode::NotADirectory.

void list(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"path", "recursive", "pattern", "limit", "offset",
                          "sort", "reverse", "full"});
    if (args.reject_unknown(conn)) return;

    std::string path;
    if (!resolve_path(conn, args, "directory.list", path)) return;

    bool recursive = false;
    if (args.present("recursive")) {
        auto r = args.boolean("recursive");
        if (!r) {
            invalid_args(conn,
                         "directory.list 'recursive' must be a boolean");
            return;
        }
        recursive = *r;
    }

    std::string name_filter;  // empty => no filter
    if (args.present("pattern")) {
        auto p = args.str("pattern");
        if (!p) {
            invalid_args(conn, "directory.list 'pattern' must be a string");
            return;
        }
        name_filter = std::move(*p);
    }

    int limit = 200;
    if (args.present("limit")) {
        auto l = args.integer32("limit");
        if (!l || *l < 1) {
            invalid_args(conn,
                         "directory.list 'limit' must be an integer >= 1");
            return;
        }
        limit = *l;
    }

    int offset = 0;
    if (args.present("offset")) {
        auto o = args.integer32("offset");
        if (!o || *o < 0) {
            invalid_args(conn,
                         "directory.list 'offset' must be a non-negative "
                         "integer");
            return;
        }
        offset = *o;
    }

    std::string sort_key = "name";
    if (args.present("sort")) {
        auto s = args.str("sort");
        if (!s || (*s != "name" && *s != "mtime" &&
                   *s != "size" && *s != "ctime")) {
            invalid_args(conn,
                         "directory.list 'sort' must be name|mtime|size|ctime");
            return;
        }
        sort_key = std::move(*s);
    }

    bool reverse = false;
    if (args.present("reverse")) {
        auto r = args.boolean("reverse");
        if (!r) {
            invalid_args(conn,
                         "directory.list 'reverse' must be a boolean");
            return;
        }
        reverse = *r;
    }

    if (args.present("full")) {
        auto f = args.boolean("full");
        if (!f) {
            invalid_args(conn, "directory.list 'full' must be a boolean");
            return;
        }
        if (*f) limit = INT_MAX;
    }

    const std::wstring wroot = text::utf8_to_wide(path);

    // Existence + is-a-directory check up front so not_found /
    // not-a-directory surface as the spec codes (the bare FindFirstFile
    // approach cannot distinguish "missing dir" from "empty dir").
    const DWORD root_attrs = GetFileAttributesW(wroot.c_str());
    if (root_attrs == INVALID_FILE_ATTRIBUTES) {
        write_open_err(conn, path);
        return;
    }
    if ((root_attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        conn.writer().write_err(
            ErrorCode::NotADirectory,
            "{\"message\":\"directory.list 'path' is a file, not a directory\"}");
        return;
    }

    struct Entry {
        std::string name;        // relative path (forward-slash) when recursive
        std::string type;
        std::uint64_t size;
        long long  mtime_unix;
        long long  ctime_unix;
        long long  atime_unix;
        DWORD      attrs;
        bool       is_dir;
    };
    std::vector<Entry> entries;

    const std::wstring wfilter =
        name_filter.empty() ? std::wstring() : text::utf8_to_wide(name_filter);

    // Depth-first walk. `rel_prefix` is the forward-slash relative path of the
    // directory currently being scanned (empty at the root). FindFirst/Next is
    // a LOOP (no C-ABI callback), so no exception-barrier concern here.
    auto join = [](const std::wstring& a, const std::wstring& b) {
        std::wstring r = a;
        if (!r.empty() && r.back() != L'\\' && r.back() != L'/') r += L'\\';
        r += b;
        return r;
    };

    std::vector<std::pair<std::wstring, std::string>> stack;  // (dir, rel)
    stack.emplace_back(wroot, std::string());
    while (!stack.empty()) {
        const std::wstring dir = stack.back().first;
        const std::string  rel = stack.back().second;
        stack.pop_back();

        std::wstring pattern = join(dir, L"*");
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (std::wcscmp(fd.cFileName, L".") == 0 ||
                std::wcscmp(fd.cFileName, L"..") == 0) {
                continue;
            }

            const bool is_dir =
                (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const bool is_reparse =
                (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

            const bool name_ok =
                wfilter.empty() ||
                PathMatchSpecW(fd.cFileName, wfilter.c_str());

            std::string entry_name =
                text::wide_to_utf8(fd.cFileName, std::wcslen(fd.cFileName));
            std::string rel_name = rel.empty()
                ? entry_name
                : (rel + "/" + entry_name);

            if (name_ok) {
                ULARGE_INTEGER sz{};
                sz.LowPart  = fd.nFileSizeLow;
                sz.HighPart = fd.nFileSizeHigh;
                entries.push_back({
                    rel_name,
                    std::string(attribute_type(fd.dwFileAttributes)),
                    sz.QuadPart,
                    filetime_to_unix(fd.ftLastWriteTime),
                    filetime_to_unix(fd.ftCreationTime),
                    filetime_to_unix(fd.ftLastAccessTime),
                    fd.dwFileAttributes,
                    is_dir
                });
            }

            // Recurse into real subdirectories only (reparse points are NOT
            // followed — they appear as `link` entries, per the spec).
            if (recursive && is_dir && !is_reparse) {
                stack.emplace_back(join(dir, fd.cFileName), rel_name);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // Sort. name: directories before files, then case-insensitive.
    // mtime/size/ctime: highest-first (newest / largest).
    auto cmp = [&](const Entry& a, const Entry& b) -> bool {
        if (sort_key == "name") {
            if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
            for (std::size_t k = 0; k < a.name.size() && k < b.name.size(); ++k) {
                const auto ca =
                    std::tolower(static_cast<unsigned char>(a.name[k]));
                const auto cb =
                    std::tolower(static_cast<unsigned char>(b.name[k]));
                if (ca != cb) return ca < cb;
            }
            return a.name.size() < b.name.size();
        }
        if (sort_key == "mtime") return a.mtime_unix > b.mtime_unix;
        if (sort_key == "size")  return a.size       > b.size;
        if (sort_key == "ctime") return a.ctime_unix > b.ctime_unix;
        return false;
    };
    std::sort(entries.begin(), entries.end(), cmp);
    if (reverse) std::reverse(entries.begin(), entries.end());

    // Apply offset + limit and emit. x-output-schema per entry:
    // {name,type,size,mtime_unix_s,ctime_unix_s,atime_unix_s,flags} — all
    // required, additionalProperties:false.
    const auto start = static_cast<std::size_t>(offset);
    std::string body = "{\"entries\":[";
    bool first   = true;
    int  emitted = 0;
    for (std::size_t i = start; i < entries.size() && emitted < limit; ++i) {
        const auto& e = entries[i];
        if (!first) body += ',';
        first = false;
        body += '{';
        json::append_kv_string(body, "name", e.name);          body += ',';
        json::append_kv_string(body, "type", e.type);          body += ',';
        json::append_kv_uint(body, "size", e.size);            body += ',';
        json::append_kv_int(body, "mtime_unix_s", e.mtime_unix); body += ',';
        json::append_kv_int(body, "ctime_unix_s", e.ctime_unix); body += ',';
        json::append_kv_int(body, "atime_unix_s", e.atime_unix); body += ',';
        append_flags_array(body, e.attrs);
        body += '}';
        ++emitted;
    }
    body += "]}";
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// file.stat — input_schema: ["path"].
// x-errors: ["not_found","permission_denied","invalid_args"].
// x-output-schema: {type,size,mtime_unix_s,ctime_unix_s,atime_unix_s,flags}
// all required, additionalProperties:false.

void stat(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"path"});
    if (args.reject_unknown(conn)) return;

    std::string path;
    if (!resolve_path(conn, args, "file.stat", path)) return;

    const std::wstring wpath = text::utf8_to_wide(path);
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &info)) {
        write_open_err(conn, path);
        return;
    }

    ULARGE_INTEGER sz{};
    sz.LowPart  = info.nFileSizeLow;
    sz.HighPart = info.nFileSizeHigh;

    std::string body = "{";
    json::append_kv_string(body, "type",
                           attribute_type(info.dwFileAttributes));
    body += ',';
    json::append_kv_uint(body, "size", sz.QuadPart);
    body += ',';
    json::append_kv_int(body, "mtime_unix_s",
                        filetime_to_unix(info.ftLastWriteTime));
    body += ',';
    json::append_kv_int(body, "ctime_unix_s",
                        filetime_to_unix(info.ftCreationTime));
    body += ',';
    json::append_kv_int(body, "atime_unix_s",
                        filetime_to_unix(info.ftLastAccessTime));
    body += ',';
    append_flags_array(body, info.dwFileAttributes);
    body += '}';
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// file.delete — input_schema: ["path"].
// x-errors: ["not_found","permission_denied","not_a_directory",
// "invalid_args"]. A directory target emits ErrorCode::NotADirectory.
// x-output-schema: null — wire response is the literal OK 0.

void delete_(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"path"});
    if (args.reject_unknown(conn)) return;

    std::string path;
    if (!resolve_path(conn, args, "file.delete", path)) return;

    const std::wstring wpath = text::utf8_to_wide(path);
    const DWORD attrs = GetFileAttributesW(wpath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        write_open_err(conn, path);
        return;
    }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        // file.delete.json: "Returns ERR not_a_directory if path resolves to
        // a directory" (not_a_directory is in file.delete x-errors).
        conn.writer().write_err(
            ErrorCode::NotADirectory,
            "{\"message\":\"file.delete 'path' is a directory; "
            "use directory.delete\"}");
        return;
    }

    if (!DeleteFileW(wpath.c_str())) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            conn.writer().write_err(ErrorCode::NotFound);
            return;
        }
        char detail[64];
        std::snprintf(detail, sizeof(detail),
                      "{\"win32_error\":%lu}", err);
        conn.writer().write_err(ErrorCode::PermissionDenied, detail);
        return;
    }
    conn.writer().write_ok();  // x-output-schema: null (OK 0)
}

// ---------------------------------------------------------------------------
// file.exists — input_schema: ["path"].
// x-errors: ["permission_denied","invalid_args"]. (NOT not_found — a missing
// path is the success answer {exists:false,type:"absent",flags:[]}.)
// x-output-schema: {exists,type,flags} all required.

void exists(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"path"});
    if (args.reject_unknown(conn)) return;

    std::string path;
    if (!resolve_path(conn, args, "file.exists", path)) return;

    const std::wstring wpath = text::utf8_to_wide(path);
    const DWORD attrs = GetFileAttributesW(wpath.c_str());

    std::string body = "{";
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD err = GetLastError();
        // A genuine ACL denial is the spec's permission_denied; a
        // not-found / bad-path is the success answer "absent".
        if (err == ERROR_ACCESS_DENIED) {
            char detail[64];
            std::snprintf(detail, sizeof(detail),
                          "{\"win32_error\":%lu}", err);
            conn.writer().write_err(ErrorCode::PermissionDenied, detail);
            return;
        }
        json::append_kv_bool(body, "exists", false);
        body += ',';
        json::append_kv_string(body, "type", "absent");
        body += ',';
        json::append_string(body, "flags");
        body += ":[]";
    } else {
        json::append_kv_bool(body, "exists", true);
        body += ',';
        json::append_kv_string(body, "type", attribute_type(attrs));
        body += ',';
        append_flags_array(body, attrs);
    }
    body += '}';
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// file.wait — input_schema (schema order): ["glob","timeout_ms"].
// x-errors: ["timeout","permission_denied","invalid_args"].
// x-output-schema: {path,type} both required.
//
// Polls FindFirstFileW at 100 ms cadence until a match appears or the
// timeout expires.

void wait(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"glob", "timeout_ms"});
    if (args.reject_unknown(conn)) return;

    std::optional<std::string> glob = args.str("glob");
    if (!glob || glob->empty()) {
        invalid_args(conn, "file.wait requires 'glob'");
        return;
    }

    long long timeout = 30000;  // default per file.wait.json
    if (args.present("timeout_ms")) {
        auto t = args.integer("timeout_ms");
        if (!t || *t < 0) {
            invalid_args(conn,
                         "file.wait 'timeout_ms' must be a non-negative "
                         "integer");
            return;
        }
        timeout = *t;
    }

    const std::wstring pattern = text::utf8_to_wide(*glob);

    // Directory portion to prefix to FindFirstFile results.
    std::wstring dir_prefix;
    if (const auto sep = pattern.find_last_of(L"\\/");
        sep != std::wstring::npos) {
        dir_prefix = pattern.substr(0, sep + 1);
    }

    using clock = std::chrono::steady_clock;
    const auto deadline =
        clock::now() + std::chrono::milliseconds(timeout);

    while (true) {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            std::wstring full = dir_prefix;
            full.append(fd.cFileName);
            const DWORD matched_attrs = fd.dwFileAttributes;
            FindClose(h);

            std::string body = "{";
            json::append_kv_string(body, "path", text::wide_to_utf8(full));
            body += ',';
            json::append_kv_string(body, "type",
                                   attribute_type(matched_attrs));
            body += '}';
            conn.writer().write_ok(body);
            return;
        }

        if (clock::now() >= deadline) {
            conn.writer().write_err(ErrorCode::Timeout);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// ---------------------------------------------------------------------------
// directory.create — input_schema (schema order): ["path","parents","mode"].
// x-errors: ["already_exists","permission_denied","not_found",
// "invalid_args"]. `mode` is accepted then ignored on Windows (NTFS uses
// ACLs, not POSIX bits — fields_ignored:["mode"] in x-families).
// x-output-schema: {created} required.

void mkdir(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"path", "parents", "mode"});
    if (args.reject_unknown(conn)) return;

    std::string path;
    if (!resolve_path(conn, args, "directory.create", path)) return;

    bool parents = false;
    if (args.present("parents")) {
        auto p = args.boolean("parents");
        if (!p) {
            invalid_args(conn,
                         "directory.create 'parents' must be a boolean");
            return;
        }
        parents = *p;
    }

    if (args.present("mode")) {
        // Validated for type/range then ignored on Windows.
        auto m = args.integer32("mode");
        if (!m || *m < 0 || *m > 511) {
            invalid_args(conn,
                         "directory.create 'mode' must be an integer in "
                         "[0, 511]");
            return;
        }
    }

    const std::wstring wpath = text::utf8_to_wide(path);

    if (parents) {
        // mkdir -p: create each missing component from the root. Existing
        // intermediates are skipped, not errored. A final-component
        // already-existing directory is still already_exists.
        const DWORD final_attrs = GetFileAttributesW(wpath.c_str());
        if (final_attrs != INVALID_FILE_ATTRIBUTES &&
            (final_attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            conn.writer().write_err(
                ErrorCode::AlreadyExists,
                "{\"message\":\"directory already exists\"}");
            return;
        }
        std::wstring acc;
        std::size_t i = 0;
        while (i <= wpath.size()) {
            if (i == wpath.size() ||
                wpath[i] == L'\\' || wpath[i] == L'/') {
                if (!acc.empty()) {
                    // Skip a bare drive spec ("C:") — not a creatable dir.
                    const bool drive_only =
                        acc.size() == 2 && acc[1] == L':';
                    if (!drive_only) {
                        if (!CreateDirectoryW(acc.c_str(), nullptr)) {
                            const DWORD err = GetLastError();
                            if (err != ERROR_ALREADY_EXISTS) {
                                if (err == ERROR_PATH_NOT_FOUND ||
                                    err == ERROR_FILE_NOT_FOUND) {
                                    conn.writer().write_err(
                                        ErrorCode::NotFound);
                                    return;
                                }
                                char d[64];
                                std::snprintf(d, sizeof(d),
                                              "{\"win32_error\":%lu}", err);
                                conn.writer().write_err(
                                    ErrorCode::PermissionDenied, d);
                                return;
                            }
                        }
                    }
                }
                if (i < wpath.size()) acc += wpath[i];
            } else {
                acc += wpath[i];
            }
            ++i;
        }
        conn.writer().write_ok("{\"created\":true}");
        return;
    }

    if (!CreateDirectoryW(wpath.c_str(), nullptr)) {
        const DWORD err = GetLastError();
        if (err == ERROR_ALREADY_EXISTS) {
            conn.writer().write_err(
                ErrorCode::AlreadyExists,
                "{\"message\":\"directory already exists\"}");
            return;
        }
        if (err == ERROR_PATH_NOT_FOUND || err == ERROR_FILE_NOT_FOUND) {
            // A missing parent with parents:false is the spec's not_found.
            conn.writer().write_err(
                ErrorCode::NotFound,
                "{\"message\":\"parent directory does not exist; pass "
                "parents:true\"}");
            return;
        }
        char detail[64];
        std::snprintf(detail, sizeof(detail),
                      "{\"win32_error\":%lu}", err);
        conn.writer().write_err(ErrorCode::PermissionDenied, detail);
        return;
    }
    conn.writer().write_ok("{\"created\":true}");
}

// ---------------------------------------------------------------------------
// Shared rename body for file.rename and directory.rename. Both have
// input_schema (schema order) ["src","dst","overwrite","cross_fs"] and the
// same x-output-schema {renamed,fallback_used}. They differ in x-errors and
// in the source-type precondition:
//   file.rename      x-errors: not_found, already_exists, cross_device,
//                              permission_denied, invalid_args
//   directory.rename x-errors: not_found, already_exists, permission_denied,
//                              cross_device, not_a_directory, invalid_args
// `cross_device` (cross-FS without cross_fs) emits ErrorCode::CrossDevice;
// `not_a_directory` (directory.rename, non-directory src) emits
// ErrorCode::NotADirectory. Both are in the respective verbs' x-errors.
namespace {

void rename_impl(Connection& conn, const wire::Request& req,
                 std::string_view verb, bool require_directory) {
    SchemaArgs args(req, {"src", "dst", "overwrite", "cross_fs"});
    if (args.reject_unknown(conn)) return;

    std::optional<std::string> src = args.str("src");
    if (!src || src->empty()) {
        invalid_args(conn, std::string(verb) + " requires 'src'");
        return;
    }
    std::optional<std::string> dst = args.str("dst");
    if (!dst || dst->empty()) {
        invalid_args(conn, std::string(verb) + " requires 'dst'");
        return;
    }

    bool overwrite = false;
    if (args.present("overwrite")) {
        auto o = args.boolean("overwrite");
        if (!o) {
            invalid_args(conn,
                         std::string(verb) +
                         " 'overwrite' must be a boolean");
            return;
        }
        overwrite = *o;
    }

    bool cross_fs = false;
    if (args.present("cross_fs")) {
        auto c = args.boolean("cross_fs");
        if (!c) {
            invalid_args(conn,
                         std::string(verb) +
                         " 'cross_fs' must be a boolean");
            return;
        }
        cross_fs = *c;
    }

    const std::wstring wsrc = text::utf8_to_wide(*src);
    const std::wstring wdst = text::utf8_to_wide(*dst);

    const DWORD src_attrs = GetFileAttributesW(wsrc.c_str());
    if (src_attrs == INVALID_FILE_ATTRIBUTES) {
        conn.writer().write_err(ErrorCode::NotFound,
                                "{\"message\":\"src does not exist\"}");
        return;
    }
    const bool src_is_dir =
        (src_attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (require_directory && !src_is_dir) {
        conn.writer().write_err(
            ErrorCode::NotADirectory,
            "{\"message\":\"directory.rename 'src' is a file, "
            "not a directory\"}");
        return;
    }
    if (!require_directory && src_is_dir) {
        // file.rename: a directory src is the spec's invalid_args (the
        // file/dir split is explicit — "use directory.rename for
        // directories"). not_a_directory is not applicable here.
        invalid_args(conn,
                     "file.rename 'src' is a directory; use directory.rename");
        return;
    }

    // Without overwrite, an existing dst is already_exists (in both verbs'
    // x-errors). MoveFileExW without MOVEFILE_REPLACE_EXISTING also fails on
    // an existing dst, but the explicit probe yields the precise code.
    if (!overwrite &&
        GetFileAttributesW(wdst.c_str()) != INVALID_FILE_ATTRIBUTES) {
        conn.writer().write_err(
            ErrorCode::AlreadyExists,
            "{\"message\":\"dst already exists; pass overwrite:true\"}");
        return;
    }

    DWORD flags = MOVEFILE_WRITE_THROUGH;
    if (overwrite) flags |= MOVEFILE_REPLACE_EXISTING;
    if (cross_fs)  flags |= MOVEFILE_COPY_ALLOWED;

    // x-output-schema: fallback_used == "copy_delete" only when the
    // cross-filesystem copy+delete path was actually engaged. MoveFileExW
    // with MOVEFILE_COPY_ALLOWED only copies+deletes when src/dst are on
    // different volumes; a same-volume move (even with cross_fs) is an
    // atomic rename. Resolve the volume mount points BEFORE the move
    // (GetVolumePathNameW walks up to an existing parent, so a not-yet-
    // existing dst leaf is fine) and only claim "copy_delete" when we are
    // confident the volumes differ.
    auto volume_of = [](const std::wstring& p) -> std::wstring {
        wchar_t vol[MAX_PATH];
        if (GetVolumePathNameW(p.c_str(), vol, MAX_PATH)) return vol;
        return std::wstring();
    };
    const std::wstring vsrc = volume_of(wsrc);
    const std::wstring vdst = volume_of(wdst);
    const bool cross_volume =
        !vsrc.empty() && !vdst.empty() &&
        _wcsicmp(vsrc.c_str(), vdst.c_str()) != 0;

    if (!MoveFileExW(wsrc.c_str(), wdst.c_str(), flags)) {
        const DWORD err = GetLastError();
        if (err == ERROR_NOT_SAME_DEVICE) {
            conn.writer().write_err(
                ErrorCode::CrossDevice,
                "{\"message\":\"" + std::string(verb) +
                " src and dst are on different filesystems; "
                "pass cross_fs:true\"}");
            return;
        }
        if (err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS) {
            conn.writer().write_err(
                ErrorCode::AlreadyExists,
                "{\"message\":\"dst already exists; pass overwrite:true\"}");
            return;
        }
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            conn.writer().write_err(ErrorCode::NotFound);
            return;
        }
        char detail[64];
        std::snprintf(detail, sizeof(detail),
                      "{\"win32_error\":%lu}", err);
        conn.writer().write_err(ErrorCode::PermissionDenied, detail);
        return;
    }

    // x-output-schema: {renamed, fallback_used} both required.
    std::string body = "{";
    json::append_kv_bool(body, "renamed", true);
    body += ',';
    json::append_kv_string(body, "fallback_used",
                           (cross_fs && cross_volume) ? "copy_delete"
                                                      : "none");
    body += '}';
    conn.writer().write_ok(body);
}

}  // namespace

// ---------------------------------------------------------------------------
// file.rename — files only.

void rename(Connection& conn, const wire::Request& req) {
    rename_impl(conn, req, "file.rename", /*require_directory=*/false);
}

// ---------------------------------------------------------------------------
// directory.stat — input_schema: ["path"].
// x-errors: ["not_found","permission_denied","not_a_directory",
// "invalid_args"]. The non-directory case emits ErrorCode::NotADirectory.
// x-output-schema: {type,entry_count,mtime_unix_s,flags} all required;
// type is the const "directory".

void directory_stat(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"path"});
    if (args.reject_unknown(conn)) return;

    std::string path;
    if (!resolve_path(conn, args, "directory.stat", path)) return;

    const std::wstring wpath = text::utf8_to_wide(path);

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &fad)) {
        write_open_err(conn, path);
        return;
    }
    if ((fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        conn.writer().write_err(
            ErrorCode::NotADirectory,
            "{\"message\":\"directory.stat 'path' is a file, "
            "not a directory\"}");
        return;
    }

    std::wstring pattern = wpath;
    if (!pattern.empty() && pattern.back() != L'\\' &&
        pattern.back() != L'/') {
        pattern += L'\\';
    }
    pattern += L'*';

    std::uint64_t entry_count = 0;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (std::wcscmp(fd.cFileName, L".") == 0 ||
                std::wcscmp(fd.cFileName, L"..") == 0) {
                continue;
            }
            ++entry_count;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    std::string body = "{";
    json::append_kv_string(body, "type", "directory");
    body += ',';
    json::append_kv_uint(body, "entry_count", entry_count);
    body += ',';
    json::append_kv_int(body, "mtime_unix_s",
                        filetime_to_unix(fad.ftLastWriteTime));
    body += ',';
    append_flags_array(body, fad.dwFileAttributes);
    body += '}';
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// directory.exists — input_schema: ["path"].
// x-errors: ["permission_denied","invalid_args"]. (NOT not_found — a missing
// path is the success answer {exists:false}.)
// x-output-schema: {exists} required.

void directory_exists(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"path"});
    if (args.reject_unknown(conn)) return;

    std::string path;
    if (!resolve_path(conn, args, "directory.exists", path)) return;

    const std::wstring wpath = text::utf8_to_wide(path);
    const DWORD attrs = GetFileAttributesW(wpath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            char detail[64];
            std::snprintf(detail, sizeof(detail),
                          "{\"win32_error\":%lu}", err);
            conn.writer().write_err(ErrorCode::PermissionDenied, detail);
            return;
        }
        conn.writer().write_ok("{\"exists\":false}");
        return;
    }
    const bool is_dir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    conn.writer().write_ok(is_dir ? "{\"exists\":true}"
                                  : "{\"exists\":false}");
}

// ---------------------------------------------------------------------------
// directory.rename — directories only.

void directory_rename(Connection& conn, const wire::Request& req) {
    rename_impl(conn, req, "directory.rename", /*require_directory=*/true);
}

// ---------------------------------------------------------------------------
// directory.delete — input_schema (schema order): ["path","recursive"].
// x-errors: ["not_found","not_empty","permission_denied","not_a_directory",
// "invalid_args"]. A file target emits ErrorCode::NotADirectory.
// x-output-schema: {entries_removed} required.

namespace {

// Recursive removal of a directory's contents and the directory itself.
// `removed` counts each file/subdirectory removed (excludes the top-level
// directory). FindFirst/Next is a LOOP — no C-ABI callback boundary, so the
// recursion needs no exception barrier. Every FindFirstFileW is paired with
// FindClose on all paths.
bool remove_recursive(const std::wstring& dir, std::uint64_t& removed) {
    std::wstring pattern = dir;
    if (!pattern.empty() && pattern.back() != L'\\' &&
        pattern.back() != L'/') {
        pattern += L'\\';
    }
    pattern += L'*';

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (std::wcscmp(fd.cFileName, L".") == 0 ||
                std::wcscmp(fd.cFileName, L"..") == 0) {
                continue;
            }
            std::wstring child = dir;
            if (!child.empty() && child.back() != L'\\' &&
                child.back() != L'/') {
                child += L'\\';
            }
            child += fd.cFileName;
            const bool is_dir =
                (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const bool is_reparse =
                (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            if (is_dir && !is_reparse) {
                if (!remove_recursive(child, removed)) {
                    FindClose(h);
                    return false;
                }
            } else {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
                    SetFileAttributesW(
                        child.c_str(),
                        fd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
                }
                // A reparse-point directory: remove the link, not its
                // target (RemoveDirectoryW on the junction itself).
                const bool ok = (is_dir && is_reparse)
                    ? (RemoveDirectoryW(child.c_str()) != 0)
                    : (DeleteFileW(child.c_str()) != 0);
                if (!ok) {
                    FindClose(h);
                    return false;
                }
                ++removed;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    if (!RemoveDirectoryW(dir.c_str())) {
        return false;
    }
    ++removed;
    return true;
}

}  // namespace

void directory_remove(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"path", "recursive"});
    if (args.reject_unknown(conn)) return;

    std::string path;
    if (!resolve_path(conn, args, "directory.delete", path)) return;

    bool recursive = false;
    if (args.present("recursive")) {
        auto r = args.boolean("recursive");
        if (!r) {
            invalid_args(conn,
                         "directory.delete 'recursive' must be a boolean");
            return;
        }
        recursive = *r;
    }

    const std::wstring wpath = text::utf8_to_wide(path);
    const DWORD attrs = GetFileAttributesW(wpath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        write_open_err(conn, path);
        return;
    }
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        conn.writer().write_err(
            ErrorCode::NotADirectory,
            "{\"message\":\"directory.delete 'path' is a file, "
            "not a directory\"}");
        return;
    }

    std::uint64_t entries_removed = 0;
    if (recursive) {
        if (!remove_recursive(wpath, entries_removed)) {
            const DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND ||
                err == ERROR_PATH_NOT_FOUND) {
                conn.writer().write_err(ErrorCode::NotFound);
                return;
            }
            char detail[64];
            std::snprintf(detail, sizeof(detail),
                          "{\"win32_error\":%lu}", err);
            conn.writer().write_err(ErrorCode::PermissionDenied, detail);
            return;
        }
        // remove_recursive counts the top-level dir; the wire's
        // entries_removed reports only the entries INSIDE the target.
        entries_removed = entries_removed > 0 ? entries_removed - 1 : 0;
    } else {
        if (!RemoveDirectoryW(wpath.c_str())) {
            const DWORD err = GetLastError();
            if (err == ERROR_DIR_NOT_EMPTY) {
                conn.writer().write_err(
                    ErrorCode::NotEmpty,
                    "{\"message\":\"directory not empty; pass "
                    "recursive:true\"}");
                return;
            }
            if (err == ERROR_FILE_NOT_FOUND ||
                err == ERROR_PATH_NOT_FOUND) {
                conn.writer().write_err(ErrorCode::NotFound);
                return;
            }
            char detail[64];
            std::snprintf(detail, sizeof(detail),
                          "{\"win32_error\":%lu}", err);
            conn.writer().write_err(ErrorCode::PermissionDenied, detail);
            return;
        }
    }

    char body[64];
    std::snprintf(body, sizeof(body),
        "{\"entries_removed\":%llu}",
        static_cast<unsigned long long>(entries_removed));
    conn.writer().write_ok(body);
}

}  // namespace remote_hands::file_verbs
