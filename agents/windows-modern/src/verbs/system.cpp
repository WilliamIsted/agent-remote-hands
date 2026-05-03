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

// `system.*` namespace verb handlers.
//
// Implements the verbs in PROTOCOL.md §4.1:
//   system.info                  (read)
//   system.capabilities          (read)
//   system.health                (read)
//   system.shutdown_blockers     (read)
//   system.lock                  (read)
//   system.reboot                (extra_risky)
//   system.shutdown              (extra_risky)
//   system.logoff                (extra_risky)
//   system.hibernate             (extra_risky)
//   system.sleep                 (extra_risky)

#include "../capabilities.hpp"
#include "../connection.hpp"
#include "../errors.hpp"
#include "../json.hpp"
#include "../log.hpp"
#include "../sysinfo.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <powrprof.h>
#include <reason.h>

#pragma comment(lib, "powrprof.lib")

namespace remote_hands::system_verbs {

// ---------------------------------------------------------------------------
// system.info

void info(Connection& conn, const wire::Request&) {
    std::string j;
    j += '{';

    json::append_kv_string(j, "name", "agent-remote-hands");                 j += ',';
    json::append_kv_string(j, "version", sysinfo::kAgentVersion);            j += ',';
    json::append_kv_string(j, "protocol", "2.0");                            j += ',';
    json::append_kv_string(j, "os", sysinfo::kOsName);                       j += ',';
    json::append_kv_string(j, "arch", sysinfo::arch());                      j += ',';
    json::append_kv_string(j, "hostname", sysinfo::hostname());              j += ',';
    json::append_kv_string(j, "user", sysinfo::current_user());              j += ',';

    auto integrity = sysinfo::integrity_level();
    if (integrity.empty()) {
        json::append_kv_null(j, "integrity");
    } else {
        json::append_kv_string(j, "integrity", integrity);
    }
    j += ',';

    json::append_kv_bool(j, "uiaccess", sysinfo::uiaccess_enabled());        j += ',';
    json::append_kv_int(j, "monitors", GetSystemMetrics(SM_CMONITORS));      j += ',';
    json::append_string_array(j, "privileges", sysinfo::enabled_privileges()); j += ',';

    json::append_string(j, "tiers");
    j += ":[\"read\",\"create\",\"update\",\"delete\",\"extra_risky\"],";

    json::append_kv_string(j, "current_tier", to_wire(conn.tier()));         j += ',';

    json::append_string(j, "auth");
    j += ":[\"token\"],";

    json::append_kv_int(j, "max_connections", conn.max_connections());       j += ',';

    json::append_string(j, "namespaces");
    j += ':';
    j += build_namespaces_json_array();
    j += ',';

    // Capabilities sub-object.
    json::append_string(j, "capabilities");
    j += ":{";
    // Capture engine: BitBlt today; WGC fast path is a future runtime probe.
    json::append_kv_string(j, "capture", "gdi"); j += ',';
    // UIA available on this target.
    json::append_kv_string(j, "ui_automation", "uia"); j += ',';
    // Image formats: BMP (raw) and PNG (via WIC). WebP arrives with libwebp.
    json::append_string(j, "image_formats");
    j += ":[\"png\",\"bmp\"]";
    j += '}';

    j += '}';
    conn.writer().write_ok(j);
}

// ---------------------------------------------------------------------------
// system.capabilities

void capabilities(Connection& conn, const wire::Request&) {
    conn.writer().write_ok(build_capabilities_json());
}

// ---------------------------------------------------------------------------
// system.health

void health(Connection& conn, const wire::Request&) {
    conn.writer().write_ok();
}

// ---------------------------------------------------------------------------
// system.lock

void lock(Connection& conn, const wire::Request&) {
    if (!LockWorkStation()) {
        const DWORD err = GetLastError();
        log::warning(L"LockWorkStation failed (%lu)", err);
        conn.writer().write_err(ErrorCode::NotSupported);
        return;
    }
    conn.writer().write_ok();
}

// ---------------------------------------------------------------------------
// system.shutdown_blockers
//
// Enumerates top-level windows that have called ShutdownBlockReasonCreate,
// returning the hwnd + reason text for each.

namespace {

struct BlockerCollector {
    std::string out;
    bool        first = true;
};

BOOL CALLBACK enum_blocker(HWND hwnd, LPARAM lparam) {
    auto* col = reinterpret_cast<BlockerCollector*>(lparam);

    wchar_t reason[1024] = {};
    DWORD   reason_len = static_cast<DWORD>(std::size(reason));
    if (!ShutdownBlockReasonQuery(hwnd, reason, &reason_len)) {
        return TRUE;
    }
    if (reason_len == 0) {
        return TRUE;
    }

    if (!col->first) col->out += ',';
    col->first = false;

    char hbuf[32];
    std::snprintf(hbuf, sizeof(hbuf), "win:0x%llx",
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hwnd)));

    col->out += '{';
    json::append_kv_string(col->out, "hwnd", hbuf);
    col->out += ',';

    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, reason, -1, nullptr, 0, nullptr, nullptr);
    std::string reason_utf8;
    if (needed > 0) {
        reason_utf8.resize(static_cast<std::size_t>(needed - 1));
        WideCharToMultiByte(CP_UTF8, 0, reason, -1,
                            reason_utf8.data(), needed, nullptr, nullptr);
    }
    json::append_kv_string(col->out, "reason", reason_utf8);
    col->out += '}';

    return TRUE;
}

}  // namespace

void shutdown_blockers(Connection& conn, const wire::Request&) {
    BlockerCollector col;
    col.out = "{\"blockers\":[";
    EnumWindows(enum_blocker, reinterpret_cast<LPARAM>(&col));
    col.out += "]}";
    conn.writer().write_ok(col.out);
}

// ---------------------------------------------------------------------------
// Power verbs (reboot / shutdown / logoff / hibernate / sleep)

namespace {

bool ensure_shutdown_privilege() {
    return sysinfo::enable_privilege(SE_SHUTDOWN_NAME);
}

struct PowerArgs {
    DWORD       delay_seconds = 0;
    bool        force         = false;
    std::string reason_code   = "planned";
};

// Returns false (and writes ERR invalid_args to the wire) if an unknown
// --flag is present. Otherwise populates `out` and returns true.
bool parse_power_args(Connection& conn, const wire::Request& req, PowerArgs& out) {
    for (std::size_t i = 0; i < req.args.size(); ++i) {
        if (req.args[i] == "--delay" && i + 1 < req.args.size()) {
            out.delay_seconds = static_cast<DWORD>(std::strtoul(
                req.args[++i].c_str(), nullptr, 10));
        } else if (req.args[i] == "--force") {
            out.force = true;
        } else if (req.args[i] == "--reason" && i + 1 < req.args.size()) {
            out.reason_code = req.args[++i];
        } else if (req.args[i].size() >= 2 &&
                   req.args[i].compare(0, 2, "--") == 0) {
            std::string detail = "{\"unknown_flag\":\"";
            detail += req.args[i];
            detail += "\"}";
            conn.writer().write_err(ErrorCode::InvalidArgs, detail);
            return false;
        }
    }
    return true;
}

// Single-process pending-shutdown state. A `--delay > 0` request takes the
// slot; subsequent calls reject with ERR conflict until the timer fires or
// system.power.cancel clears it. The detached thread waits on the CV so a
// cancel notification can interrupt the sleep.
struct PendingShutdown {
    std::mutex                              mu;
    std::condition_variable                 cv;
    bool                                    active = false;
    std::chrono::steady_clock::time_point   steady_deadline;
    long long                               unix_deadline_ms = 0;
    UINT                                    exit_flags = 0;
    DWORD                                   reason = 0;
};

PendingShutdown& pending_shutdown() {
    static PendingShutdown p;
    return p;
}

void do_power(Connection& conn, UINT exit_flags, const wire::Request& req) {
    PowerArgs args;
    if (!parse_power_args(conn, req, args)) return;

    if (!ensure_shutdown_privilege()) {
        conn.writer().write_err(
            ErrorCode::InsufficientPrivilege,
            "{\"missing\":\"SeShutdownPrivilege\"}");
        return;
    }

    if (args.force) exit_flags |= EWX_FORCE;

    const DWORD reason =
        SHTDN_REASON_MAJOR_OPERATINGSYSTEM | SHTDN_REASON_FLAG_PLANNED;

    if (args.delay_seconds > 0) {
        // Honour --delay by waiting in a detached thread, then calling
        // ExitWindowsEx. We don't use InitiateShutdownW here because its
        // SHUTDOWN_* flag set is incompatible with the EWX_* flags the
        // caller has chosen, and ExitWindowsEx covers reboot / shutdown /
        // logoff uniformly. Trade-offs: no "Windows is shutting down" toast,
        // and the timer is bound to the agent process — if the agent exits
        // before it fires the OS-level call never happens. See issue #59.
        auto& p = pending_shutdown();
        {
            std::lock_guard<std::mutex> lk(p.mu);
            if (p.active) {
                char detail[80];
                std::snprintf(detail, sizeof(detail),
                              "{\"pending_until_ms\":%lld}",
                              p.unix_deadline_ms);
                conn.writer().write_err(ErrorCode::Conflict, detail);
                return;
            }
            const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            p.active           = true;
            p.steady_deadline  = std::chrono::steady_clock::now() +
                                 std::chrono::seconds(args.delay_seconds);
            p.unix_deadline_ms = now_ms + static_cast<long long>(args.delay_seconds) * 1000LL;
            p.exit_flags       = exit_flags;
            p.reason           = reason;
        }
        std::thread([]() {
            auto& p = pending_shutdown();
            UINT  flags;
            DWORD r;
            bool  fired;
            {
                std::unique_lock<std::mutex> lk(p.mu);
                const auto deadline = p.steady_deadline;
                flags = p.exit_flags;
                r     = p.reason;
                // Wake on either the deadline or a cancel that clears `active`.
                p.cv.wait_until(lk, deadline, [&p]() { return !p.active; });
                fired = p.active;     // still active => deadline expired
                p.active = false;
            }
            if (fired) {
                if (!ExitWindowsEx(flags, r)) {
                    log::warning(L"system.power: ExitWindowsEx failed (%lu) "
                                 L"after delayed fire",
                                 GetLastError());
                }
            } else {
                log::info(L"system.power: pending shutdown cancelled");
            }
        }).detach();
    } else {
        if (!ExitWindowsEx(exit_flags, reason)) {
            char detail[64];
            std::snprintf(detail, sizeof(detail),
                          "{\"win32_error\":%lu}", GetLastError());
            conn.writer().write_err(ErrorCode::NotSupported, detail);
            return;
        }
    }

    const auto now_unix = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    char body[160];
    std::snprintf(body, sizeof(body),
                  "{\"phase\":\"requested\",\"grace_ms\":%lu,"
                  "\"deadline_unix\":%lld}",
                  args.delay_seconds * 1000UL,
                  static_cast<long long>(now_unix + args.delay_seconds));
    conn.writer().write_ok(body);
}

}  // namespace

void reboot(Connection& conn, const wire::Request& req) {
    do_power(conn, EWX_REBOOT, req);
}

void shutdown(Connection& conn, const wire::Request& req) {
    do_power(conn, EWX_SHUTDOWN, req);
}

void logoff(Connection& conn, const wire::Request& req) {
    do_power(conn, EWX_LOGOFF, req);
}

void hibernate(Connection& conn, const wire::Request&) {
    if (!ensure_shutdown_privilege()) {
        conn.writer().write_err(
            ErrorCode::InsufficientPrivilege,
            "{\"missing\":\"SeShutdownPrivilege\"}");
        return;
    }
    if (!SetSuspendState(TRUE, FALSE, FALSE)) {
        conn.writer().write_err(ErrorCode::NotSupported);
        return;
    }
    conn.writer().write_ok();
}

void sleep(Connection& conn, const wire::Request&) {
    if (!ensure_shutdown_privilege()) {
        conn.writer().write_err(
            ErrorCode::InsufficientPrivilege,
            "{\"missing\":\"SeShutdownPrivilege\"}");
        return;
    }
    if (!SetSuspendState(FALSE, FALSE, FALSE)) {
        conn.writer().write_err(ErrorCode::NotSupported);
        return;
    }
    conn.writer().write_ok();
}

// ---------------------------------------------------------------------------
// system.power.cancel — abort a pending in-process delayed shutdown.

void power_cancel(Connection& conn, const wire::Request&) {
    auto& p = pending_shutdown();
    bool      was_pending = false;
    long long unix_deadline_ms = 0;
    {
        std::lock_guard<std::mutex> lk(p.mu);
        was_pending = p.active;
        unix_deadline_ms = p.unix_deadline_ms;
        if (was_pending) p.active = false;
    }
    if (was_pending) {
        p.cv.notify_all();
        char body[80];
        std::snprintf(body, sizeof(body),
                      "{\"cancelled_until_ms\":%lld}", unix_deadline_ms);
        conn.writer().write_ok(body);
    } else {
        conn.writer().write_err(ErrorCode::NotFound,
                                "{\"message\":\"no pending shutdown\"}");
    }
}

}  // namespace remote_hands::system_verbs
