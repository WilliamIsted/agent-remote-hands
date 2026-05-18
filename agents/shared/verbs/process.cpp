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

// `process.*` namespace verb handlers.
//
// Implements PROTOCOL.md §4.7:
//   process.list   (read)
//   process.start  (create)  -- CreateProcess, optional --stdin payload
//   process.shell  (create)  -- ShellExecuteEx for paths with spaces / unicode
//   process.kill   (delete)
//   process.wait   (read)

#include "../connection.hpp"
#include "../errors.hpp"
#include "../json.hpp"
#include "../log.hpp"
#include "../text_util.hpp"

#include <charconv>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

namespace remote_hands::process_verbs {

namespace {

// Track HANDLEs of agent-spawned processes so process.wait can query the
// exit code even after the OS has reaped the process. Without this, the
// caller gets `ERR target_gone` from `OpenProcess` rather than the cached
// exit code.
//
// Bookkeeping is best-effort: a handle is leaked if process.start is called
// without matching process.wait. In practice the map size is bounded by
// per-session orphan-spawn count and each entry is small.
std::mutex& spawned_mutex() {
    static std::mutex m;
    return m;
}
std::unordered_map<DWORD, HANDLE>& spawned_processes() {
    static std::unordered_map<DWORD, HANDLE> m;
    return m;
}
void track_spawned(DWORD pid, HANDLE handle) {
    std::lock_guard<std::mutex> lk(spawned_mutex());
    spawned_processes()[pid] = handle;
}
// Returns the stored HANDLE for `pid` (caller now owns) or NULL if untracked.
HANDLE take_spawned(DWORD pid) {
    std::lock_guard<std::mutex> lk(spawned_mutex());
    auto it = spawned_processes().find(pid);
    if (it == spawned_processes().end()) return nullptr;
    HANDLE h = it->second;
    spawned_processes().erase(it);
    return h;
}

bool contains_ci(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    if (haystack.size() < needle.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (std::tolower(static_cast<unsigned char>(haystack[i + j])) !=
                std::tolower(static_cast<unsigned char>(needle[j]))) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

bool parse_uint(std::string_view s, unsigned long long& out) {
    const auto* end = s.data() + s.size();
    const auto [p, ec] = std::from_chars(s.data(), end, out, 10);
    return ec == std::errc{} && p == end;
}

}  // namespace

// ---------------------------------------------------------------------------
// process.list

void list(Connection& conn, const wire::Request& req) {
    std::string filter;
    for (std::size_t i = 0; i < req.args.size(); ++i) {
        if (req.args[i] == "--filter" && i + 1 < req.args.size()) {
            filter = req.args[++i];
        } else if (req.args[i].size() >= 2 &&
                   req.args[i].compare(0, 2, "--") == 0) {
            std::string detail = "{\"unknown_flag\":\"";
            detail += req.args[i];
            detail += "\"}";
            conn.writer().write_err(ErrorCode::InvalidArgs, detail);
            return;
        }
    }

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        conn.writer().write_err(ErrorCode::NotSupported);
        return;
    }

    std::string body = "{\"processes\":[";
    bool first = true;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            const auto image = text::wide_to_utf8(
                pe.szExeFile, std::wcslen(pe.szExeFile));
            if (!filter.empty() && !contains_ci(image, filter)) continue;

            if (!first) body += ',';
            first = false;
            body += '{';
            json::append_kv_uint(body, "pid",  pe.th32ProcessID);        body += ',';
            json::append_kv_uint(body, "ppid", pe.th32ParentProcessID);  body += ',';
            json::append_kv_string(body, "image", image);
            body += '}';
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    body += "]}";
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// process.start
//
// `process.start <command-line> [--stdin <length>]`. Single command-line
// string (passed verbatim to CreateProcessW); optional --stdin reads the
// stated number of bytes from the wire and pipes them into the child's stdin.

void start(Connection& conn, const wire::Request& req) {
    if (req.args.empty()) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"process.start requires <command-line> [--stdin <length>]\"}");
        return;
    }

    // Reconstruct the command line by joining all positional args with spaces.
    // The wire protocol (v2) tokenizes the header on whitespace and has no
    // argument-quoting mechanism, so a caller's `cmd.exe /c exit 7` arrives as
    // four separate args. The previous implementation took only `args[0]` and
    // silently dropped the rest, producing a bare interactive cmd.exe.
    // Forward-compatible with the planned protocol quoting in rc.9: a single
    // quoted arg arrives as one element and joining a one-element list is a
    // no-op.
    std::string cmdline_utf8;
    bool                   has_stdin = false;
    std::size_t            stdin_bytes = 0;
    for (std::size_t i = 0; i < req.args.size(); ++i) {
        if (req.args[i] == "--stdin" && i + 1 < req.args.size()) {
            unsigned long long v = 0;
            if (!parse_uint(req.args[++i], v)) {
                conn.writer().write_err(
                    ErrorCode::InvalidArgs,
                    "{\"message\":\"--stdin length must be a non-negative integer\"}");
                return;
            }
            stdin_bytes = static_cast<std::size_t>(v);
            has_stdin = true;
            continue;
        }
        if (!cmdline_utf8.empty()) cmdline_utf8 += ' ';
        cmdline_utf8 += req.args[i];
    }

    if (cmdline_utf8.empty()) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"process.start requires <command-line> [--stdin <length>]\"}");
        return;
    }

    std::vector<std::byte> stdin_payload;
    if (has_stdin) {
        stdin_payload = conn.reader().read_payload(stdin_bytes);
    }

    std::wstring cmdline = text::utf8_to_wide(cmdline_utf8);
    // CreateProcessW may write to lpCommandLine; ensure it's writable.
    cmdline.push_back(L'\0');

    HANDLE pipe_read  = nullptr;
    HANDLE pipe_write = nullptr;
    if (has_stdin) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength        = sizeof(sa);
        sa.bInheritHandle = TRUE;
        if (!CreatePipe(&pipe_read, &pipe_write, &sa, 0)) {
            conn.writer().write_err(ErrorCode::NotSupported);
            return;
        }
        // Don't let the child inherit the write end.
        SetHandleInformation(pipe_write, HANDLE_FLAG_INHERIT, 0);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (has_stdin) {
        si.dwFlags    = STARTF_USESTDHANDLES;
        si.hStdInput  = pipe_read;
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    }

    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(
        nullptr,
        cmdline.data(),
        nullptr, nullptr,
        has_stdin ? TRUE : FALSE,
        0,
        nullptr, nullptr,
        &si, &pi);

    if (pipe_read) CloseHandle(pipe_read);

    if (!ok) {
        if (pipe_write) CloseHandle(pipe_write);
        char detail[64];
        std::snprintf(detail, sizeof(detail),
                      "{\"win32_error\":%lu}", GetLastError());
        conn.writer().write_err(ErrorCode::NotSupported, detail);
        return;
    }

    if (has_stdin && pipe_write) {
        std::size_t written_total = 0;
        while (written_total < stdin_payload.size()) {
            DWORD chunk = static_cast<DWORD>(
                std::min<std::size_t>(stdin_payload.size() - written_total,
                                      static_cast<std::size_t>(0x10000)));
            DWORD written = 0;
            if (!WriteFile(pipe_write, stdin_payload.data() + written_total,
                           chunk, &written, nullptr)) {
                break;
            }
            written_total += written;
        }
        CloseHandle(pipe_write);
    }

    const DWORD pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    // Retain pi.hProcess for later process.wait — see track_spawned. The
    // handle is closed by take_spawned + the eventual wait response.
    track_spawned(pid, pi.hProcess);

    char body[64];
    std::snprintf(body, sizeof(body), "{\"pid\":%lu}", pid);
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// process.shell
//
// ShellExecuteEx with the default verb. Suited to "open this file/URL in the
// associated app" — paths with spaces / unicode are handled by the shell
// without escape hazards.

void shell(Connection& conn, const wire::Request& req) {
    if (req.args.empty()) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"process.shell requires <path> [--args <s>] [--verb <v>]\"}");
        return;
    }

    std::string verb_arg;
    std::string args_arg;
    for (std::size_t i = 1; i < req.args.size(); ++i) {
        if (req.args[i] == "--verb" && i + 1 < req.args.size()) {
            verb_arg = req.args[++i];
        } else if (req.args[i] == "--args" && i + 1 < req.args.size()) {
            args_arg = req.args[++i];
        }
    }

    std::wstring target     = text::utf8_to_wide(req.args[0]);
    std::wstring wide_args   = args_arg.empty() ? std::wstring{} : text::utf8_to_wide(args_arg);
    std::wstring wide_verb   = verb_arg.empty() ? std::wstring{} : text::utf8_to_wide(verb_arg);

    SHELLEXECUTEINFOW sei{};
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb       = wide_verb.empty() ? nullptr : wide_verb.c_str();
    sei.lpFile       = target.c_str();
    sei.lpParameters = wide_args.empty() ? nullptr : wide_args.c_str();
    sei.nShow        = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) {
        char detail[64];
        std::snprintf(detail, sizeof(detail),
                      "{\"win32_error\":%lu}", GetLastError());
        conn.writer().write_err(ErrorCode::NotSupported, detail);
        return;
    }

    DWORD pid = 0;
    if (sei.hProcess) {
        pid = GetProcessId(sei.hProcess);
        CloseHandle(sei.hProcess);
    }

    char body[64];
    std::snprintf(body, sizeof(body), "{\"pid\":%lu}", pid);
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// process.kill

void kill(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 1) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"process.kill requires <pid>\"}");
        return;
    }

    unsigned long long pid_v = 0;
    if (!parse_uint(req.args[0], pid_v)) {
        conn.writer().write_err(ErrorCode::InvalidArgs,
                                "{\"message\":\"pid must be a non-negative integer\"}");
        return;
    }

    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid_v));
    if (!h) {
        std::string detail = "{";
        json::append_kv_uint(detail, "pid", pid_v);
        detail += '}';
        conn.writer().write_err(ErrorCode::TargetGone, detail);
        return;
    }

    if (!TerminateProcess(h, 1)) {
        CloseHandle(h);
        char detail[64];
        std::snprintf(detail, sizeof(detail),
                      "{\"win32_error\":%lu}", GetLastError());
        conn.writer().write_err(ErrorCode::InsufficientPrivilege, detail);
        return;
    }
    CloseHandle(h);
    conn.writer().write_ok();
}

// ---------------------------------------------------------------------------
// process.wait

void wait(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 2) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"process.wait requires <pid> <timeout-ms>\"}");
        return;
    }

    unsigned long long pid_v = 0;
    unsigned long long timeout_v = 0;
    if (!parse_uint(req.args[0], pid_v) || !parse_uint(req.args[1], timeout_v)) {
        conn.writer().write_err(ErrorCode::InvalidArgs,
                                "{\"message\":\"pid and timeout-ms must be integers\"}");
        return;
    }

    // Prefer the tracked handle from process.start: GetExitCodeProcess on a
    // retained handle works whether the process is alive or already reaped,
    // so callers get the cached exit code rather than ERR target_gone.
    HANDLE h = take_spawned(static_cast<DWORD>(pid_v));
    if (!h) {
        h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                        FALSE, static_cast<DWORD>(pid_v));
        if (!h) {
            std::string detail = "{";
            json::append_kv_uint(detail, "pid", pid_v);
            detail += '}';
            conn.writer().write_err(ErrorCode::TargetGone, detail);
            return;
        }
    }

    const DWORD wr = WaitForSingleObject(h, static_cast<DWORD>(timeout_v));
    if (wr == WAIT_TIMEOUT) {
        // Re-track the handle so a subsequent wait can reuse it; otherwise
        // the next call falls back to OpenProcess and we lose the cache.
        track_spawned(static_cast<DWORD>(pid_v), h);
        conn.writer().write_err(ErrorCode::Timeout);
        return;
    }
    if (wr != WAIT_OBJECT_0) {
        CloseHandle(h);
        conn.writer().write_err(ErrorCode::NotSupported);
        return;
    }

    DWORD exit_code = 0;
    GetExitCodeProcess(h, &exit_code);
    CloseHandle(h);

    char body[64];
    std::snprintf(body, sizeof(body), "{\"exit_code\":%lu}", exit_code);
    conn.writer().write_ok(body);
}

}  // namespace remote_hands::process_verbs
