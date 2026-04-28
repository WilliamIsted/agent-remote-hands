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

// `file.*` namespace verb handlers.
//
// Implements PROTOCOL.md §4.6:
//   file.read    (observe)
//   file.write   (drive)
//   file.list    (observe)
//   file.stat    (observe)
//   file.delete  (power)
//   file.exists  (observe)
//   file.wait    (observe)
//   file.mkdir   (drive)
//   file.rename  (drive)
//
// Paths are UTF-8 on the wire; converted to wide-char internally.
// FILETIME values are converted to Unix epoch seconds for the JSON `mtime_unix`.

#include "../connection.hpp"
#include "../errors.hpp"
#include "../json.hpp"
#include "../log.hpp"
#include "../text_util.hpp"

#include <charconv>
#include <chrono>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace remote_hands::file_verbs {

namespace {

constexpr long long kFiletimeUnixDelta = 116444736000000000LL;  // 100ns intervals

long long filetime_to_unix(const FILETIME& ft) {
    ULARGE_INTEGER u{};
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (static_cast<long long>(u.QuadPart) - kFiletimeUnixDelta) / 10000000LL;
}

std::string_view attribute_type(DWORD attrs) {
    if (attrs == INVALID_FILE_ATTRIBUTES)              return "unknown";
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT)          return "link";
    if (attrs & FILE_ATTRIBUTE_DIRECTORY)              return "dir";
    return "file";
}

bool parse_uint(std::string_view s, unsigned long long& out) {
    const auto* end = s.data() + s.size();
    const auto [p, ec] = std::from_chars(s.data(), end, out, 10);
    return ec == std::errc{} && p == end;
}

void write_target_gone_or_not_supported(Connection& conn, std::string_view path) {
    const DWORD err = GetLastError();
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
        std::string detail = "{";
        json::append_kv_string(detail, "path", path);
        detail += '}';
        conn.writer().write_err(ErrorCode::NotFound, detail);
    } else {
        char detail[96];
        std::snprintf(detail, sizeof(detail),
                      "{\"win32_error\":%lu}", err);
        conn.writer().write_err(ErrorCode::NotSupported, detail);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// file.read

void read(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 1) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"file.read requires <path>\"}");
        return;
    }

    const std::wstring wpath = text::utf8_to_wide(req.args[0]);
    HANDLE h = CreateFileW(
        wpath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        write_target_gone_or_not_supported(conn, req.args[0]);
        return;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0) {
        CloseHandle(h);
        conn.writer().write_err(ErrorCode::NotSupported);
        return;
    }
    if (size.QuadPart > static_cast<long long>(SIZE_MAX)) {
        CloseHandle(h);
        conn.writer().write_err(
            ErrorCode::NotSupported,
            "{\"message\":\"file too large for whole-file read\"}");
        return;
    }

    std::vector<std::byte> buf(static_cast<std::size_t>(size.QuadPart));
    std::size_t total = 0;
    while (total < buf.size()) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(buf.size() - total, 0x10000000u));  // 256 MiB
        DWORD got = 0;
        if (!ReadFile(h, buf.data() + total, chunk, &got, nullptr) || got == 0) {
            break;
        }
        total += got;
    }
    CloseHandle(h);

    buf.resize(total);
    conn.writer().write_ok(std::span<const std::byte>(buf));
}

// ---------------------------------------------------------------------------
// file.write

void write(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 2) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"file.write requires <path> <length>\"}");
        return;
    }

    unsigned long long length = 0;
    if (!parse_uint(req.args[1], length)) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"length must be a non-negative integer\"}");
        return;
    }

    auto payload = conn.reader().read_payload(static_cast<std::size_t>(length));

    const std::wstring wpath = text::utf8_to_wide(req.args[0]);
    HANDLE h = CreateFileW(
        wpath.c_str(), GENERIC_WRITE, 0,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        write_target_gone_or_not_supported(conn, req.args[0]);
        return;
    }

    std::size_t total = 0;
    while (total < payload.size()) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(payload.size() - total, 0x10000000u));
        DWORD written = 0;
        if (!WriteFile(h, payload.data() + total, chunk, &written, nullptr) ||
            written == 0) {
            CloseHandle(h);
            char detail[64];
            std::snprintf(detail, sizeof(detail),
                          "{\"win32_error\":%lu}", GetLastError());
            conn.writer().write_err(ErrorCode::NotSupported, detail);
            return;
        }
        total += written;
    }
    CloseHandle(h);
    conn.writer().write_ok();
}

// ---------------------------------------------------------------------------
// file.list

void list(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 1) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"file.list requires <path>\"}");
        return;
    }

    std::wstring pattern = text::utf8_to_wide(req.args[0]);

    // If the pattern has no wildcard, treat it as a directory and append \\*.
    if (pattern.find_first_of(L"*?") == std::wstring::npos) {
        if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/') {
            pattern += L'\\';
        }
        pattern += L'*';
    }

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        conn.writer().write_ok("{\"entries\":[]}");
        return;
    }

    std::string body = "{\"entries\":[";
    bool first = true;
    do {
        if (std::wcscmp(fd.cFileName, L".") == 0 ||
            std::wcscmp(fd.cFileName, L"..") == 0) {
            continue;
        }

        if (!first) body += ',';
        first = false;

        ULARGE_INTEGER sz{};
        sz.LowPart  = fd.nFileSizeLow;
        sz.HighPart = fd.nFileSizeHigh;

        body += '{';
        json::append_kv_string(body, "name",
            text::wide_to_utf8(fd.cFileName, std::wcslen(fd.cFileName)));
        body += ',';
        json::append_kv_string(body, "type", attribute_type(fd.dwFileAttributes));
        body += ',';
        json::append_kv_uint(body, "size", sz.QuadPart);
        body += ',';
        json::append_kv_int(body, "mtime_unix",
                            filetime_to_unix(fd.ftLastWriteTime));
        body += '}';
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    body += "]}";
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// file.stat

void stat(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 1) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"file.stat requires <path>\"}");
        return;
    }

    const std::wstring wpath = text::utf8_to_wide(req.args[0]);
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &info)) {
        write_target_gone_or_not_supported(conn, req.args[0]);
        return;
    }

    ULARGE_INTEGER sz{};
    sz.LowPart  = info.nFileSizeLow;
    sz.HighPart = info.nFileSizeHigh;

    std::string body = "{";
    json::append_kv_string(body, "type", attribute_type(info.dwFileAttributes));
    body += ',';
    json::append_kv_uint(body, "size", sz.QuadPart);
    body += ',';
    json::append_kv_int(body, "mtime_unix", filetime_to_unix(info.ftLastWriteTime));
    body += '}';
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// file.delete

void delete_(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 1) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"file.delete requires <path>\"}");
        return;
    }

    const std::wstring wpath = text::utf8_to_wide(req.args[0]);
    const DWORD attrs = GetFileAttributesW(wpath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        write_target_gone_or_not_supported(conn, req.args[0]);
        return;
    }

    const BOOL ok = (attrs & FILE_ATTRIBUTE_DIRECTORY)
        ? RemoveDirectoryW(wpath.c_str())
        : DeleteFileW(wpath.c_str());

    if (!ok) {
        char detail[64];
        std::snprintf(detail, sizeof(detail),
                      "{\"win32_error\":%lu}", GetLastError());
        conn.writer().write_err(ErrorCode::NotSupported, detail);
        return;
    }
    conn.writer().write_ok();
}

// ---------------------------------------------------------------------------
// file.exists

void exists(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 1) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"file.exists requires <path>\"}");
        return;
    }

    const std::wstring wpath = text::utf8_to_wide(req.args[0]);
    const DWORD attrs = GetFileAttributesW(wpath.c_str());

    std::string body = "{";
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        json::append_kv_bool(body, "exists", false);
    } else {
        json::append_kv_bool(body, "exists", true);
        body += ',';
        json::append_kv_string(body, "type", attribute_type(attrs));
    }
    body += '}';
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// file.wait
//
// Polls FindFirstFileW at 100 ms cadence until a match appears or the
// timeout expires. Future iteration replaces this with ReadDirectoryChangesW
// for lower overhead on long waits.

void wait(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 2) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"file.wait requires <pattern> <timeout-ms>\"}");
        return;
    }

    unsigned long long timeout = 0;
    if (!parse_uint(req.args[1], timeout)) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"timeout-ms must be a non-negative integer\"}");
        return;
    }

    const std::wstring pattern = text::utf8_to_wide(req.args[0]);

    // Determine the directory portion to prefix to FindFirstFile results.
    std::wstring dir_prefix;
    if (const auto sep = pattern.find_last_of(L"\\/");
        sep != std::wstring::npos) {
        dir_prefix = pattern.substr(0, sep + 1);
    }

    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(timeout);

    while (true) {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            // Found at least one match.
            std::wstring full = dir_prefix;
            full.append(fd.cFileName);
            FindClose(h);

            std::string body = "{";
            json::append_kv_string(body, "kind", "appeared");
            body += ',';
            json::append_kv_string(body, "path", text::wide_to_utf8(full));
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
// file.mkdir

void mkdir(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 1) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"file.mkdir requires <path>\"}");
        return;
    }

    const std::wstring wpath = text::utf8_to_wide(req.args[0]);
    if (!CreateDirectoryW(wpath.c_str(), nullptr)) {
        char detail[64];
        std::snprintf(detail, sizeof(detail),
                      "{\"win32_error\":%lu}", GetLastError());
        conn.writer().write_err(ErrorCode::NotSupported, detail);
        return;
    }
    conn.writer().write_ok();
}

// ---------------------------------------------------------------------------
// file.rename

void rename(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 2) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"file.rename requires <src> <dst>\"}");
        return;
    }

    const std::wstring src = text::utf8_to_wide(req.args[0]);
    const std::wstring dst = text::utf8_to_wide(req.args[1]);

    if (!MoveFileExW(src.c_str(), dst.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        char detail[64];
        std::snprintf(detail, sizeof(detail),
                      "{\"win32_error\":%lu}", GetLastError());
        conn.writer().write_err(ErrorCode::NotSupported, detail);
        return;
    }
    conn.writer().write_ok();
}

}  // namespace remote_hands::file_verbs
