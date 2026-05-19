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
// Implements the verbs whose contracts are the spec JSON under
// protocol/spec/verbs/common/process.*.json and
// protocol/spec/verbs/windows/process.shell.json (the single source of
// truth):
//   process.list   (R)  input_schema {pattern,include_counters,limit,
//                        include_system} -> {processes:[{pid,image,ppid,...}]}
//   process.start  (C)  input_schema {argv (required, array),stdin,cwd}
//                        -> {pid}
//   process.shell  (C)  input_schema {path (required),args,verb,cwd}
//                        -> {pid|null}
//   process.kill   (D)  input_schema {pid (required),exit_code} -> null
//   process.wait   (R)  input_schema {pid (required),timeout_ms} ->
//                        {exit_code}
//
// PHASE 2.1 — NAMED-ARG MIGRATION. These handlers no longer index
// `req.args` positionally with ad-hoc `--flag` scanning. Each handler
// declares its input_schema property list (IN SCHEMA ORDER) and reads each
// value by NAME through the shared SchemaArgs resolver (schema_args.hpp),
// with the schema's property ORDER used as the positional fallback when the
// caller invoked the verb positionally (the v2.2 reference client packs
// positional calls as `{"_args":[...]}`). The `window.*` namespace was the
// pattern-setter; this file follows it (and system.cpp) exactly. Validation
// emits the same ErrorCode::InvalidArgs + {"message":...} ergonomics as
// before.
//
// v2.2 fold-ins (per the spec JSON, not the old §4.7 wording):
//   * process.start `argv` is an ARRAY of strings (argv[0] = executable,
//     argv[1..] = arguments). The agent assembles a correctly-quoted Win32
//     lpCommandLine. `stdin` is now an inline UTF-8 STRING arg (no separate
//     length-prefixed wire payload), and `cwd` sets the child's working
//     directory.
//   * process.kill takes an optional `exit_code` (default 1).
//   * process.wait `timeout_ms` is OPTIONAL — absent => wait indefinitely
//     (INFINITE); present => the millisecond bound.
//   * process.shell gains an optional `cwd`; `verb` is enum-validated.

#include "../connection.hpp"
#include "../errors.hpp"
#include "../json.hpp"
#include "../log.hpp"
#include "../text_util.hpp"
#include "args.hpp"
#include "schema_args.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
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

// The Phase-2.1 named-argument resolver and its invalid_args helper live in
// the shared header (schema_args.hpp) so every namespace reads through one
// definition. Pull them into this TU's unqualified name lookup; behaviour is
// identical to window.cpp / system.cpp.
using wire::SchemaArgs;
using wire::invalid_args;

namespace {

// Track HANDLEs of agent-spawned processes so process.wait can query the
// exit code even after the OS has reaped the process. Without this, the
// caller gets `ERR not_found` from `OpenProcess` rather than the cached
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

// Quote one argv element for a Win32 lpCommandLine per the CommandLineToArgvW
// rules (the inverse of what CreateProcessW's CRT uses to split argv). An
// element is wrapped in double quotes when it is empty or contains a space /
// tab / quote; embedded backslashes are doubled only when they precede the
// closing quote, and embedded quotes are backslash-escaped. The agent owning
// this assembly is what makes process.start's argv[] free of shell-escape
// hazards (process.start.json: "The agent quotes each element correctly when
// assembling the Win32 lpCommandLine").
void append_quoted_arg(std::wstring& cmdline, const std::wstring& arg) {
    const bool needs_quote =
        arg.empty() ||
        arg.find_first_of(L" \t\"") != std::wstring::npos;

    if (!needs_quote) {
        cmdline += arg;
        return;
    }

    cmdline += L'"';
    for (std::size_t i = 0; i < arg.size(); ++i) {
        std::size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++backslashes;
            ++i;
        }
        if (i == arg.size()) {
            // Escape all backslashes, but don't add a closing quote yet —
            // the trailing `"` is appended after the loop.
            cmdline.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            // Escape every backslash AND the quote itself.
            cmdline.append(backslashes * 2 + 1, L'\\');
            cmdline += L'"';
        } else {
            // Backslashes not followed by a quote are literal.
            cmdline.append(backslashes, L'\\');
            cmdline += arg[i];
        }
    }
    cmdline += L'"';
}

}  // namespace

// ---------------------------------------------------------------------------
// process.list — input_schema {pattern, include_counters, limit,
// include_system}; x-output-schema {processes:[{pid,image,ppid,...}]}.
// x-errors: ["permission_denied","invalid_args"].
//
// `include_counters` is validated for shape here; the per-pid counter walk
// (OpenProcess + GetProcessTimes + GetProcessMemoryInfo +
// GetProcessHandleCount) is a separate, opt-in cost not part of this
// arg-input refactor — a malformed value still surfaces as invalid_args
// rather than being silently ignored. The required {pid,image,ppid} fields
// are always emitted; the optional counter fields are added by the dedicated
// counters task.

void list(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"pattern", "include_counters", "limit",
                          "include_system"});

    std::string filter;
    if (args.present("pattern")) {
        auto p = args.str("pattern");
        if (!p) {
            invalid_args(conn, "process.list 'pattern' must be a string");
            return;
        }
        filter = *p;
    }

    if (args.present("include_counters")) {
        auto ic = args.boolean("include_counters");
        if (!ic) {
            invalid_args(conn,
                         "process.list 'include_counters' must be a boolean");
            return;
        }
        // Shape-validated; the counter walk is the dedicated counters task.
    }

    int limit = 100;   // schema default
    if (args.present("limit")) {
        // integer32 (not static_cast<int>(integer())): an out-of-int-range
        // limit is treated identically to a non-integer — invalid_args, no
        // silent narrowing wrap.
        auto l = args.integer32("limit");
        if (!l || *l < 1) {
            invalid_args(conn,
                         "process.list 'limit' must be a positive integer");
            return;
        }
        limit = *l;
    }

    bool include_system = false;   // schema default
    if (args.present("include_system")) {
        auto is = args.boolean("include_system");
        if (!is) {
            invalid_args(conn,
                         "process.list 'include_system' must be a boolean");
            return;
        }
        include_system = *is;
    }

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        // process.list.json x-errors = ["permission_denied","invalid_args"].
        // A snapshot failure is the agent lacking the rights to enumerate ->
        // permission_denied (the spec-declared code; the pre-Phase-2.1
        // handler emitted not_supported, which is outside this verb's
        // declared error set — the spec is authoritative so corrected).
        conn.writer().write_err(ErrorCode::PermissionDenied);
        return;
    }

    struct Entry {
        DWORD       pid;
        DWORD       ppid;
        std::string image;
    };
    std::vector<Entry> entries;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            const auto image = text::wide_to_utf8(
                pe.szExeFile, std::wcslen(pe.szExeFile));
            if (!filter.empty() && !contains_ci(image, filter)) continue;

            if (!include_system) {
                DWORD session_id = 0;
                // If the call fails (process already gone), include the entry
                // rather than silently dropping it.
                if (ProcessIdToSessionId(pe.th32ProcessID, &session_id) &&
                    session_id == 0) continue;
            }

            entries.push_back({pe.th32ProcessID, pe.th32ParentProcessID, image});
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    // Sort alphabetically by image name (case-insensitive).
    std::sort(entries.begin(), entries.end(),
        [](const Entry& a, const Entry& b) {
            for (std::size_t i = 0; i < a.image.size() && i < b.image.size(); ++i) {
                const auto ca = static_cast<unsigned char>(
                    std::tolower(static_cast<unsigned char>(a.image[i])));
                const auto cb = static_cast<unsigned char>(
                    std::tolower(static_cast<unsigned char>(b.image[i])));
                if (ca != cb) return ca < cb;
            }
            return a.image.size() < b.image.size();
        });

    std::string body = "{\"processes\":[";
    bool first   = true;
    int  emitted = 0;
    for (const auto& e : entries) {
        if (emitted >= limit) break;
        if (!first) body += ',';
        first = false;
        body += '{';
        json::append_kv_uint(body, "pid",   e.pid);   body += ',';
        json::append_kv_uint(body, "ppid",  e.ppid);  body += ',';
        json::append_kv_string(body, "image", e.image);
        body += '}';
        ++emitted;
    }
    body += "]}";
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// process.start — input_schema {argv (required, array of strings, minItems
// 1), stdin (string), cwd (string)}; x-output-schema {pid (>=1)}. x-errors:
// ["not_found","permission_denied","invalid_args"].
//
// `argv` is an ARRAY: argv[0] is the executable (resolved via PATH), argv[1..]
// are arguments. The agent assembles a correctly-quoted Win32 lpCommandLine
// from the elements (see append_quoted_arg) so the caller never has to
// shell-escape. `stdin` is now an inline UTF-8 STRING arg (v2.2) — the
// pre-Phase-2.1 `--stdin <length>` separate wire payload is gone. `cwd` sets
// the child's working directory.

void start(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"argv", "stdin", "cwd"});

    const mcp::JsonValue* argv_node = args.node("argv");
    if (argv_node == nullptr || !argv_node->is_array()) {
        invalid_args(conn,
                     "process.start requires 'argv' (non-empty array of "
                     "strings)");
        return;
    }
    const auto& argv = argv_node->as_array();
    if (argv.empty()) {
        // input_schema: minItems 1.
        invalid_args(conn,
                     "process.start 'argv' must contain at least one "
                     "element (the executable)");
        return;
    }

    std::wstring cmdline;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        const mcp::JsonValue& el = argv[i];
        if (!el.is_string()) {
            invalid_args(conn,
                         "process.start 'argv' elements must all be strings");
            return;
        }
        if (i != 0) cmdline += L' ';
        append_quoted_arg(cmdline, text::utf8_to_wide(el.as_string()));
    }

    // stdin (optional, inline UTF-8 string per the v2.2 input_schema).
    bool        has_stdin = false;
    std::string stdin_utf8;
    if (args.present("stdin")) {
        auto s = args.str("stdin");
        if (!s) {
            invalid_args(conn, "process.start 'stdin' must be a string");
            return;
        }
        stdin_utf8 = std::move(*s);
        has_stdin  = true;
    }

    // cwd (optional). Empty string is treated as "not supplied" (use the
    // agent's cwd, per the spec default).
    std::wstring cwd_w;
    bool         have_cwd = false;
    if (args.present("cwd")) {
        auto c = args.str("cwd");
        if (!c) {
            invalid_args(conn, "process.start 'cwd' must be a string");
            return;
        }
        if (!c->empty()) {
            cwd_w    = text::utf8_to_wide(*c);
            have_cwd = true;
        }
    }

    // CreateProcessW may write to lpCommandLine; ensure it's writable.
    cmdline.push_back(L'\0');

    HANDLE pipe_read  = nullptr;
    HANDLE pipe_write = nullptr;
    if (has_stdin) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength        = sizeof(sa);
        sa.bInheritHandle = TRUE;
        if (!CreatePipe(&pipe_read, &pipe_write, &sa, 0)) {
            // Resource failure setting up the stdin pipe — the agent could
            // not complete the spawn on the caller's behalf. permission_denied
            // is the closest spec-declared code (process.start.json x-errors =
            // ["not_found","permission_denied","invalid_args"]; not_supported
            // is NOT declared so the pre-Phase-2.1 not_supported here is
            // corrected).
            char detail[64];
            std::snprintf(detail, sizeof(detail),
                          "{\"win32_error\":%lu}", GetLastError());
            conn.writer().write_err(ErrorCode::PermissionDenied, detail);
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
        nullptr,
        have_cwd ? cwd_w.c_str() : nullptr,
        &si, &pi);

    if (pipe_read) CloseHandle(pipe_read);

    if (!ok) {
        if (pipe_write) CloseHandle(pipe_write);
        const DWORD gle = GetLastError();
        char detail[64];
        std::snprintf(detail, sizeof(detail),
                      "{\"win32_error\":%lu}", gle);
        // process.start.json x-errors = ["not_found","permission_denied",
        // "invalid_args"]. ERROR_FILE_NOT_FOUND / ERROR_PATH_NOT_FOUND map to
        // not_found (argv[0] did not resolve); ERROR_ACCESS_DENIED /
        // ERROR_ELEVATION_REQUIRED map to permission_denied; anything else is
        // an unresolvable executable from the caller's point of view ->
        // not_found (the pre-Phase-2.1 blanket not_supported is outside this
        // verb's declared error set — corrected).
        ErrorCode code;
        if (gle == ERROR_FILE_NOT_FOUND || gle == ERROR_PATH_NOT_FOUND ||
            gle == ERROR_INVALID_NAME) {
            code = ErrorCode::NotFound;
        } else if (gle == ERROR_ACCESS_DENIED ||
                   gle == ERROR_ELEVATION_REQUIRED) {
            code = ErrorCode::PermissionDenied;
        } else {
            code = ErrorCode::NotFound;
        }
        conn.writer().write_err(code, detail);
        return;
    }

    if (has_stdin && pipe_write) {
        std::size_t written_total = 0;
        while (written_total < stdin_utf8.size()) {
            DWORD chunk = static_cast<DWORD>(
                std::min<std::size_t>(stdin_utf8.size() - written_total,
                                      static_cast<std::size_t>(0x10000)));
            DWORD written = 0;
            if (!WriteFile(pipe_write, stdin_utf8.data() + written_total,
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
// process.shell — input_schema {path (required), args, verb (enum), cwd};
// x-output-schema {pid (integer|null)}. x-errors:
// ["not_found","permission_denied","user_cancelled","no_handler",
// "invalid_args"].
//
// ShellExecuteEx with the chosen Win32 verb. Suited to "open this file/URL in
// the associated app" — paths with spaces / unicode are handled by the shell
// without escape hazards. `pid` is null when no process spawns (e.g. `print`
// to an existing handler instance).

namespace {

bool is_valid_shell_verb(std::string_view v) {
    return v == "open" || v == "runas" || v == "print" ||
           v == "edit" || v == "explore" || v == "find";
}

}  // namespace

void shell(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"path", "args", "verb", "cwd"});

    std::optional<std::string> path = args.str("path");
    if (!path || path->empty()) {
        invalid_args(conn,
                     "process.shell requires 'path' (non-empty string)");
        return;
    }

    std::string args_arg;
    if (args.present("args")) {
        auto a = args.str("args");
        if (!a) {
            invalid_args(conn, "process.shell 'args' must be a string");
            return;
        }
        args_arg = std::move(*a);
    }

    std::string verb_arg;   // schema default "open"
    if (args.present("verb")) {
        auto v = args.str("verb");
        if (!v) {
            invalid_args(conn, "process.shell 'verb' must be a string");
            return;
        }
        if (!is_valid_shell_verb(*v)) {
            std::string detail = "{";
            json::append_kv_string(
                detail, "message",
                "process.shell 'verb' must be one of "
                "open|runas|print|edit|explore|find");
            detail += ',';
            json::append_kv_string(detail, "verb", *v);
            detail += '}';
            conn.writer().write_err(ErrorCode::InvalidArgs, detail);
            return;
        }
        // "open" is the ShellExecuteEx default (nullptr lpVerb); only set a
        // non-default verb explicitly.
        if (*v != "open") verb_arg = *v;
    }

    std::string cwd_arg;
    if (args.present("cwd")) {
        auto c = args.str("cwd");
        if (!c) {
            invalid_args(conn, "process.shell 'cwd' must be a string");
            return;
        }
        cwd_arg = std::move(*c);
    }

    std::wstring target    = text::utf8_to_wide(*path);
    std::wstring wide_args = args_arg.empty() ? std::wstring{}
                                              : text::utf8_to_wide(args_arg);
    std::wstring wide_verb = verb_arg.empty() ? std::wstring{}
                                              : text::utf8_to_wide(verb_arg);
    std::wstring wide_cwd  = cwd_arg.empty()  ? std::wstring{}
                                              : text::utf8_to_wide(cwd_arg);

    SHELLEXECUTEINFOW sei{};
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb       = wide_verb.empty() ? nullptr : wide_verb.c_str();
    sei.lpFile       = target.c_str();
    sei.lpParameters = wide_args.empty() ? nullptr : wide_args.c_str();
    sei.lpDirectory  = wide_cwd.empty()  ? nullptr : wide_cwd.c_str();
    sei.nShow        = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) {
        const DWORD gle = GetLastError();
        char detail[64];
        std::snprintf(detail, sizeof(detail),
                      "{\"win32_error\":%lu}", gle);
        // process.shell.json x-errors = ["not_found","permission_denied",
        // "user_cancelled","no_handler","invalid_args"]. Every branch below
        // emits a spec-declared code (errors.hpp gained UserCancelled /
        // NoHandler so the faithful codes are now representable):
        //   FILE/PATH_NOT_FOUND        -> not_found
        //   ACCESS_DENIED              -> permission_denied
        //   CANCELLED (UAC denied)     -> user_cancelled
        //   NO_ASSOCIATION/DDE_FAIL    -> no_handler
        //   anything else              -> not_found (unresolvable target)
        ErrorCode code;
        if (gle == ERROR_FILE_NOT_FOUND || gle == ERROR_PATH_NOT_FOUND) {
            code = ErrorCode::NotFound;
        } else if (gle == ERROR_ACCESS_DENIED) {
            code = ErrorCode::PermissionDenied;
        } else if (gle == ERROR_CANCELLED) {
            // `runas` UAC consent denied by the user.
            code = ErrorCode::UserCancelled;
        } else if (gle == ERROR_NO_ASSOCIATION ||
                   gle == ERROR_DDE_FAIL ||
                   gle == ERROR_NO_PROC_SLOTS) {
            // Nothing resolved to handle the target.
            code = ErrorCode::NoHandler;
        } else {
            code = ErrorCode::NotFound;
        }
        conn.writer().write_err(code, detail);
        return;
    }

    // x-output-schema: pid is integer|null. null when no process spawned
    // (e.g. `print` to an already-running handler instance).
    std::string body = "{";
    if (sei.hProcess) {
        const DWORD pid = GetProcessId(sei.hProcess);
        CloseHandle(sei.hProcess);
        // GetProcessId returns 0 on failure. Spec: pid is integer|null and a
        // real pid is always >= 1 (callers null-check before kill/wait which
        // require pid >= 1). Treat 0 as "no pid available" -> null.
        if (pid != 0) {
            json::append_kv_uint(body, "pid", pid);
        } else {
            json::append_kv_null(body, "pid");
        }
    } else {
        json::append_kv_null(body, "pid");
    }
    body += '}';
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// process.kill — input_schema {pid (required, >=1), exit_code (default 1)};
// x-output-schema null (empty body — wire response is the literal OK 0).
// x-errors: ["not_found","permission_denied","invalid_args"].

void kill(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"pid", "exit_code"});

    auto pid = args.integer32("pid");
    if (!pid || *pid < 1) {
        invalid_args(conn,
                     "process.kill requires 'pid' (a positive integer)");
        return;
    }

    UINT exit_code = 1;   // schema default
    if (args.present("exit_code")) {
        auto ec = args.integer("exit_code");
        if (!ec) {
            invalid_args(conn,
                         "process.kill 'exit_code' must be an integer");
            return;
        }
        // TerminateProcess takes a UINT exit code; reduce modulo 2^32 the
        // same way the OS would round-trip it through the process's wait
        // value. Out-of-range is not a caller error here (any integer is a
        // legal exit code on the wire).
        exit_code = static_cast<UINT>(static_cast<unsigned long long>(*ec));
    }

    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE,
                           static_cast<DWORD>(*pid));
    if (!h) {
        // OpenProcess failure: process exited, or pid was reused for an
        // inaccessible process. process.kill.json: "ERR not_found when
        // OpenProcess fails". (Pre-Phase-2.1 emitted target_gone, which is
        // outside this verb's declared error set — corrected.)
        std::string detail = "{";
        json::append_kv_int(detail, "pid", *pid);
        detail += '}';
        conn.writer().write_err(ErrorCode::NotFound, detail);
        return;
    }

    if (!TerminateProcess(h, exit_code)) {
        CloseHandle(h);
        // process.kill.json: "ERR permission_denied when TerminateProcess is
        // denied (protected process, cross-account without SeDebugPrivilege)".
        // (Pre-Phase-2.1 emitted insufficient_privilege, which is outside this
        // verb's declared error set — corrected.)
        char detail[64];
        std::snprintf(detail, sizeof(detail),
                      "{\"win32_error\":%lu}", GetLastError());
        conn.writer().write_err(ErrorCode::PermissionDenied, detail);
        return;
    }
    CloseHandle(h);
    conn.writer().write_ok();
}

// ---------------------------------------------------------------------------
// process.wait — input_schema {pid (required, >=1), timeout_ms (optional,
// >=0)}; x-output-schema {exit_code}. x-errors:
// ["not_found","timeout","permission_denied","invalid_args"].
//
// timeout_ms ABSENT => wait indefinitely (INFINITE). PRESENT => the
// millisecond bound; ERR timeout when it expires before exit. (The
// connection-cancel event the spec describes for the indefinite case is a
// framing-layer concern outside this arg-input refactor; a dropped
// connection still tears the thread down via the existing socket path.)

void wait(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"pid", "timeout_ms"});

    auto pid = args.integer32("pid");
    if (!pid || *pid < 1) {
        invalid_args(conn,
                     "process.wait requires 'pid' (a positive integer)");
        return;
    }

    DWORD timeout = INFINITE;   // absent => wait indefinitely
    if (args.present("timeout_ms")) {
        auto t = args.integer("timeout_ms");
        if (!t || *t < 0) {
            invalid_args(conn,
                         "process.wait 'timeout_ms' must be a non-negative "
                         "integer");
            return;
        }
        // Clamp at the WaitForSingleObject ceiling (INFINITE == 0xFFFFFFFF is
        // the "no timeout" sentinel, so a finite caller value of exactly that
        // would otherwise mean "forever"). Anything >= INFINITE-1 is treated
        // as the maximum finite wait.
        const unsigned long long ms = static_cast<unsigned long long>(*t);
        timeout = (ms >= 0xFFFFFFFEull)
                      ? 0xFFFFFFFEu
                      : static_cast<DWORD>(ms);
    }

    // Prefer the tracked handle from process.start: GetExitCodeProcess on a
    // retained handle works whether the process is alive or already reaped,
    // so callers get the cached exit code rather than ERR not_found.
    HANDLE h = take_spawned(static_cast<DWORD>(*pid));
    if (!h) {
        h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                        FALSE, static_cast<DWORD>(*pid));
        if (!h) {
            // process.wait.json: "ERR not_found when OpenProcess fails
            // (process exited and pid was recycled)". (Pre-Phase-2.1 emitted
            // target_gone, outside this verb's declared error set —
            // corrected.)
            std::string detail = "{";
            json::append_kv_int(detail, "pid", *pid);
            detail += '}';
            conn.writer().write_err(ErrorCode::NotFound, detail);
            return;
        }
    }

    const DWORD wr = WaitForSingleObject(h, timeout);
    if (wr == WAIT_TIMEOUT) {
        // Re-track the handle so a subsequent wait can reuse it; otherwise
        // the next call falls back to OpenProcess and we lose the cache.
        track_spawned(static_cast<DWORD>(*pid), h);
        // Declared in process.wait.json x-errors.
        conn.writer().write_err(ErrorCode::Timeout);
        return;
    }
    if (wr != WAIT_OBJECT_0) {
        // WAIT_FAILED (e.g. the handle lost its rights): the process is no
        // longer waitable from the agent's vantage point -> not_found (the
        // only "object gone" code in this verb's declared error set; the
        // pre-Phase-2.1 not_supported is outside it — corrected).
        CloseHandle(h);
        std::string detail = "{";
        json::append_kv_int(detail, "pid", *pid);
        detail += '}';
        conn.writer().write_err(ErrorCode::NotFound, detail);
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
