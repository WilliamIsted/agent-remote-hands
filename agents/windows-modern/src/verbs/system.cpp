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
//   system.info                  (observe)
//   system.capabilities          (observe)
//   system.health                (observe)
//   system.shutdown_blockers     (observe)
//   system.lock                  (observe)
//   system.reboot                (power)
//   system.shutdown              (power)
//   system.logoff                (power)
//   system.hibernate             (power)
//   system.sleep                 (power)

#include "../capabilities.hpp"
#include "../connection.hpp"
#include "../errors.hpp"
#include "../json.hpp"
#include "../log.hpp"
#include "../sysinfo.hpp"

#include <cstdio>
#include <string>
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
    json::append_string_array(j, "privileges", sysinfo::enabled_privileges()); j += ',';

    json::append_string(j, "tiers");
    j += ":[\"observe\",\"drive\",\"power\"],";

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
    // Capture engine: BitBlt baseline; WGC detection lands in build phase 6.
    json::append_kv_string(j, "capture", "gdi"); j += ',';
    // UIA: detection lands in build phase 9. Default to "uia" on this target.
    json::append_kv_string(j, "ui_automation", "uia"); j += ',';
    // Image formats: BMP baseline. PNG / WebP are wired alongside the WGC
    // capture path in build phase 6.
    json::append_string(j, "image_formats");
    j += ":[\"bmp\"]";
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

PowerArgs parse_power_args(const wire::Request& req) {
    PowerArgs p;
    for (std::size_t i = 0; i < req.args.size(); ++i) {
        if (req.args[i] == "--delay" && i + 1 < req.args.size()) {
            p.delay_seconds = static_cast<DWORD>(std::strtoul(
                req.args[++i].c_str(), nullptr, 10));
        } else if (req.args[i] == "--force") {
            p.force = true;
        } else if (req.args[i] == "--reason" && i + 1 < req.args.size()) {
            p.reason_code = req.args[++i];
        }
    }
    return p;
}

void do_power(Connection& conn, UINT exit_flags, const wire::Request& req) {
    const auto args = parse_power_args(req);

    if (!ensure_shutdown_privilege()) {
        conn.writer().write_err(
            ErrorCode::InsufficientPrivilege,
            "{\"missing\":\"SeShutdownPrivilege\"}");
        return;
    }

    if (args.force) exit_flags |= EWX_FORCE;

    DWORD reason = SHTDN_REASON_MAJOR_OPERATINGSYSTEM | SHTDN_REASON_FLAG_PLANNED;

    if (args.delay_seconds > 0) {
        const DWORD r = InitiateShutdownW(
            nullptr,
            nullptr,
            args.delay_seconds,
            exit_flags | SHUTDOWN_GRACE_PERIOD,
            reason);
        if (r != ERROR_SUCCESS) {
            char detail[64];
            std::snprintf(detail, sizeof(detail),
                          "{\"win32_error\":%lu}", r);
            conn.writer().write_err(ErrorCode::NotSupported, detail);
            return;
        }
    } else {
        if (!ExitWindowsEx(exit_flags, reason)) {
            char detail[64];
            std::snprintf(detail, sizeof(detail),
                          "{\"win32_error\":%lu}", GetLastError());
            conn.writer().write_err(ErrorCode::NotSupported, detail);
            return;
        }
    }

    char body[128];
    std::snprintf(body, sizeof(body),
                  "{\"phase\":\"requested\",\"grace_ms\":%lu}",
                  args.delay_seconds * 1000UL);
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

}  // namespace remote_hands::system_verbs
