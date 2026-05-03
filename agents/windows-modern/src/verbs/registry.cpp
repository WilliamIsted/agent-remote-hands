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

// `registry.*` namespace verb handlers.
//
// Implements PROTOCOL.md §4.8:
//   registry.read    (read)
//   registry.write   (update)
//   registry.delete  (delete)
//   registry.wait    (read)
//
// Paths use the standard `HKLM\Software\...` form. Roots accepted:
// HKLM, HKCU, HKCR, HKU, HKCC (or their long-form equivalents).

#include "../connection.hpp"
#include "../errors.hpp"
#include "../json.hpp"
#include "../log.hpp"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace remote_hands::registry_verbs {

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

// ---------------------------------------------------------------------------
// Type mapping

DWORD parse_reg_type(std::string_view s) {
    if (s == "REG_SZ")        return REG_SZ;
    if (s == "REG_EXPAND_SZ") return REG_EXPAND_SZ;
    if (s == "REG_DWORD")     return REG_DWORD;
    if (s == "REG_QWORD")     return REG_QWORD;
    if (s == "REG_BINARY")    return REG_BINARY;
    if (s == "REG_MULTI_SZ")  return REG_MULTI_SZ;
    return REG_NONE;
}

const char* reg_type_to_string(DWORD t) {
    switch (t) {
        case REG_SZ:        return "REG_SZ";
        case REG_EXPAND_SZ: return "REG_EXPAND_SZ";
        case REG_DWORD:     return "REG_DWORD";
        case REG_QWORD:     return "REG_QWORD";
        case REG_BINARY:    return "REG_BINARY";
        case REG_MULTI_SZ:  return "REG_MULTI_SZ";
        case REG_NONE:      return "REG_NONE";
        default:            return "REG_UNKNOWN";
    }
}

// Builds JSON for a value's data given its raw bytes + type.
void append_value_data(std::string& out, DWORD type,
                       const std::vector<BYTE>& data) {
    json::append_string(out, "data");
    out += ':';

    switch (type) {
        case REG_SZ:
        case REG_EXPAND_SZ: {
            const auto* w = reinterpret_cast<const wchar_t*>(data.data());
            std::size_t wlen = data.size() / sizeof(wchar_t);
            // Trim trailing null.
            while (wlen > 0 && w[wlen - 1] == L'\0') --wlen;
            json::append_string(out, wide_to_utf8(w, wlen));
            break;
        }
        case REG_DWORD: {
            DWORD v = 0;
            if (data.size() >= sizeof(v)) std::memcpy(&v, data.data(), sizeof(v));
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%lu", v);
            out += buf;
            break;
        }
        case REG_QWORD: {
            std::uint64_t v = 0;
            if (data.size() >= sizeof(v)) std::memcpy(&v, data.data(), sizeof(v));
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%llu",
                          static_cast<unsigned long long>(v));
            out += buf;
            break;
        }
        default: {
            // Hex-encode unknown / binary data.
            std::string hex;
            hex.reserve(data.size() * 2);
            static constexpr char kHex[] = "0123456789abcdef";
            for (BYTE b : data) {
                hex.push_back(kHex[b >> 4]);
                hex.push_back(kHex[b & 0x0f]);
            }
            json::append_string(out, hex);
            break;
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// registry.read

void read(Connection& conn, const wire::Request& req) {
    if (req.args.empty()) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"registry.read requires <path> [--value <name>]\"}");
        return;
    }

    // Reconstruct the path by joining all non-flag positional args with
    // spaces. The wire protocol (v2) tokenizes the header on whitespace and
    // has no argument-quoting mechanism, so a path like
    // `HKLM\Software\Microsoft\Windows NT\CurrentVersion` arrives as two
    // separate args. The previous implementation took only `args[0]`, which
    // landed on a real-but-wrong key (`...\Microsoft\Windows`) and either
    // returned the wrong values or ERR not_found. Forward-compatible with
    // the planned protocol quoting in rc.9.
    std::string path_utf8;
    std::wstring value_name;
    bool has_value = false;
    for (std::size_t i = 0; i < req.args.size(); ++i) {
        if (req.args[i] == "--value" && i + 1 < req.args.size()) {
            value_name = utf8_to_wide(req.args[++i]);
            has_value = true;
            continue;
        }
        if (req.args[i].size() >= 2 && req.args[i].compare(0, 2, "--") == 0) {
            std::string detail = "{\"unknown_flag\":\"";
            detail += req.args[i];
            detail += "\"}";
            conn.writer().write_err(ErrorCode::InvalidArgs, detail);
            return;
        }
        if (!path_utf8.empty()) path_utf8 += ' ';
        path_utf8 += req.args[i];
    }

    HKEY root = nullptr;
    std::wstring subkey;
    if (!split_path(path_utf8, root, subkey)) {
        conn.writer().write_err(ErrorCode::InvalidArgs,
                                "{\"message\":\"unrecognised registry path\"}");
        return;
    }

    HKEY hkey = nullptr;
    LSTATUS status = RegOpenKeyExW(root, subkey.c_str(), 0,
                                   KEY_READ | KEY_WOW64_64KEY, &hkey);
    if (status != ERROR_SUCCESS) {
        if (status == ERROR_FILE_NOT_FOUND) {
            conn.writer().write_err(ErrorCode::NotFound);
        } else {
            char detail[64];
            std::snprintf(detail, sizeof(detail),
                          "{\"win32_error\":%ld}", status);
            conn.writer().write_err(ErrorCode::NotSupported, detail);
        }
        return;
    }

    std::string body = "{";

    if (has_value) {
        DWORD type = 0;
        DWORD size = 0;
        status = RegQueryValueExW(hkey, value_name.c_str(), nullptr,
                                  &type, nullptr, &size);
        if (status != ERROR_SUCCESS) {
            RegCloseKey(hkey);
            if (status == ERROR_FILE_NOT_FOUND) {
                conn.writer().write_err(ErrorCode::NotFound);
            } else {
                conn.writer().write_err(ErrorCode::NotSupported);
            }
            return;
        }
        std::vector<BYTE> buf(size);
        RegQueryValueExW(hkey, value_name.c_str(), nullptr,
                         &type, buf.data(), &size);

        json::append_kv_string(body, "type", reg_type_to_string(type));
        body += ',';
        append_value_data(body, type, buf);
    } else {
        // Enumerate values + subkeys.
        body += "\"values\":{";
        bool first = true;
        for (DWORD i = 0; ; ++i) {
            wchar_t namebuf[256];
            DWORD namelen = static_cast<DWORD>(std::size(namebuf));
            DWORD type    = 0;
            DWORD size    = 0;
            status = RegEnumValueW(hkey, i, namebuf, &namelen, nullptr,
                                   &type, nullptr, &size);
            if (status == ERROR_NO_MORE_ITEMS) break;
            if (status != ERROR_SUCCESS && status != ERROR_MORE_DATA) break;

            std::vector<BYTE> data(size);
            namelen = static_cast<DWORD>(std::size(namebuf));
            RegEnumValueW(hkey, i, namebuf, &namelen, nullptr,
                          &type, data.data(), &size);

            if (!first) body += ',';
            first = false;
            json::append_string(body, wide_to_utf8(namebuf, namelen));
            body += ":{";
            json::append_kv_string(body, "type", reg_type_to_string(type));
            body += ',';
            append_value_data(body, type, data);
            body += '}';
        }
        body += "},\"subkeys\":[";
        first = true;
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
        body += ']';
    }

    body += '}';
    RegCloseKey(hkey);
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// registry.write

void write(Connection& conn, const wire::Request& req) {
    if (req.args.size() < 4) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"registry.write requires <path> <name> <type> <data>\"}");
        return;
    }

    HKEY root = nullptr;
    std::wstring subkey;
    if (!split_path(req.args[0], root, subkey)) {
        conn.writer().write_err(ErrorCode::InvalidArgs,
                                "{\"message\":\"unrecognised registry path\"}");
        return;
    }

    const std::wstring value_name = utf8_to_wide(req.args[1]);
    const DWORD type = parse_reg_type(req.args[2]);
    if (type == REG_NONE) {
        conn.writer().write_err(ErrorCode::InvalidArgs,
                                "{\"message\":\"unknown registry type\"}");
        return;
    }
    const std::string& data_str = req.args[3];

    HKEY hkey = nullptr;
    LSTATUS status = RegCreateKeyExW(root, subkey.c_str(), 0, nullptr,
                                     REG_OPTION_NON_VOLATILE,
                                     KEY_WRITE | KEY_WOW64_64KEY,
                                     nullptr, &hkey, nullptr);
    if (status != ERROR_SUCCESS) {
        char detail[64];
        std::snprintf(detail, sizeof(detail),
                      "{\"win32_error\":%ld}", status);
        conn.writer().write_err(ErrorCode::NotSupported, detail);
        return;
    }

    std::vector<BYTE> bytes;
    DWORD bytes_size = 0;
    std::wstring wstr;

    switch (type) {
        case REG_SZ:
        case REG_EXPAND_SZ: {
            wstr = utf8_to_wide(data_str);
            bytes_size = static_cast<DWORD>((wstr.size() + 1) * sizeof(wchar_t));
            bytes.resize(bytes_size);
            std::memcpy(bytes.data(), wstr.c_str(), bytes_size);
            break;
        }
        case REG_DWORD: {
            const DWORD v = static_cast<DWORD>(std::strtoul(data_str.c_str(), nullptr, 0));
            bytes.resize(sizeof(v));
            std::memcpy(bytes.data(), &v, sizeof(v));
            bytes_size = sizeof(v);
            break;
        }
        case REG_QWORD: {
            const std::uint64_t v =
                static_cast<std::uint64_t>(std::strtoull(data_str.c_str(), nullptr, 0));
            bytes.resize(sizeof(v));
            std::memcpy(bytes.data(), &v, sizeof(v));
            bytes_size = sizeof(v);
            break;
        }
        case REG_BINARY: {
            // Hex-encoded.
            for (std::size_t i = 0; i + 1 < data_str.size(); i += 2) {
                const auto hi = std::strtoul(std::string{data_str.substr(i, 2)}.c_str(),
                                             nullptr, 16);
                bytes.push_back(static_cast<BYTE>(hi));
            }
            bytes_size = static_cast<DWORD>(bytes.size());
            break;
        }
        default:
            RegCloseKey(hkey);
            conn.writer().write_err(ErrorCode::InvalidArgs,
                                    "{\"message\":\"unsupported registry type for write\"}");
            return;
    }

    status = RegSetValueExW(hkey, value_name.c_str(), 0, type,
                            bytes.data(), bytes_size);
    RegCloseKey(hkey);

    if (status != ERROR_SUCCESS) {
        char detail[64];
        std::snprintf(detail, sizeof(detail),
                      "{\"win32_error\":%ld}", status);
        conn.writer().write_err(ErrorCode::NotSupported, detail);
        return;
    }
    conn.writer().write_ok();
}

// ---------------------------------------------------------------------------
// registry.delete

void delete_(Connection& conn, const wire::Request& req) {
    if (req.args.empty()) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"registry.delete requires <path> [--value <name>]\"}");
        return;
    }

    // Same path-reconstruction approach as registry.read — see the comment
    // there for the wire-protocol-quoting context.
    std::string path_utf8;
    std::wstring value_name;
    bool has_value = false;
    for (std::size_t i = 0; i < req.args.size(); ++i) {
        if (req.args[i] == "--value" && i + 1 < req.args.size()) {
            value_name = utf8_to_wide(req.args[++i]);
            has_value = true;
            continue;
        }
        if (req.args[i].size() >= 2 && req.args[i].compare(0, 2, "--") == 0) {
            std::string detail = "{\"unknown_flag\":\"";
            detail += req.args[i];
            detail += "\"}";
            conn.writer().write_err(ErrorCode::InvalidArgs, detail);
            return;
        }
        if (!path_utf8.empty()) path_utf8 += ' ';
        path_utf8 += req.args[i];
    }

    HKEY root = nullptr;
    std::wstring subkey;
    if (!split_path(path_utf8, root, subkey)) {
        conn.writer().write_err(ErrorCode::InvalidArgs,
                                "{\"message\":\"unrecognised registry path\"}");
        return;
    }

    LSTATUS status;
    if (has_value) {
        HKEY hkey = nullptr;
        status = RegOpenKeyExW(root, subkey.c_str(), 0,
                               KEY_SET_VALUE | KEY_WOW64_64KEY, &hkey);
        if (status == ERROR_SUCCESS) {
            status = RegDeleteValueW(hkey, value_name.c_str());
            RegCloseKey(hkey);
        }
    } else {
        status = RegDeleteTreeW(root, subkey.c_str());
    }

    if (status != ERROR_SUCCESS) {
        if (status == ERROR_FILE_NOT_FOUND) {
            conn.writer().write_err(ErrorCode::NotFound);
        } else {
            char detail[64];
            std::snprintf(detail, sizeof(detail),
                          "{\"win32_error\":%ld}", status);
            conn.writer().write_err(ErrorCode::NotSupported, detail);
        }
        return;
    }
    conn.writer().write_ok();
}

// ---------------------------------------------------------------------------
// registry.wait

void wait(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 2) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"registry.wait requires <path> <timeout-ms>\"}");
        return;
    }

    HKEY root = nullptr;
    std::wstring subkey;
    if (!split_path(req.args[0], root, subkey)) {
        conn.writer().write_err(ErrorCode::InvalidArgs,
                                "{\"message\":\"unrecognised registry path\"}");
        return;
    }

    unsigned long timeout_ms = 0;
    {
        const auto* end = req.args[1].data() + req.args[1].size();
        const auto [p, ec] = std::from_chars(req.args[1].data(), end, timeout_ms, 10);
        if (ec != std::errc{} || p != end) {
            conn.writer().write_err(
                ErrorCode::InvalidArgs,
                "{\"message\":\"timeout-ms must be a non-negative integer\"}");
            return;
        }
    }

    HKEY hkey = nullptr;
    LSTATUS status = RegOpenKeyExW(root, subkey.c_str(), 0,
                                   KEY_NOTIFY | KEY_WOW64_64KEY, &hkey);
    if (status != ERROR_SUCCESS) {
        conn.writer().write_err(ErrorCode::NotFound);
        return;
    }

    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) {
        RegCloseKey(hkey);
        conn.writer().write_err(ErrorCode::NotSupported);
        return;
    }

    status = RegNotifyChangeKeyValue(
        hkey, TRUE,
        REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET,
        event, TRUE);
    if (status != ERROR_SUCCESS) {
        CloseHandle(event);
        RegCloseKey(hkey);
        conn.writer().write_err(ErrorCode::NotSupported);
        return;
    }

    const DWORD wr = WaitForSingleObject(event, timeout_ms);
    CloseHandle(event);
    RegCloseKey(hkey);

    if (wr == WAIT_TIMEOUT) {
        conn.writer().write_err(ErrorCode::Timeout);
        return;
    }
    if (wr != WAIT_OBJECT_0) {
        conn.writer().write_err(ErrorCode::NotSupported);
        return;
    }

    conn.writer().write_ok("{\"kind\":\"changed\"}");
}

}  // namespace remote_hands::registry_verbs
