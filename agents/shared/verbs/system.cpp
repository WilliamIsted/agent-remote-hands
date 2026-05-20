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
// Implements the verbs whose contracts are the spec JSON under
// protocol/spec/verbs/common/system.*.json and
// protocol/spec/verbs/windows/system.power.*.json (the single source of
// truth):
//   system.info             (R)  -> v2.2 discovery schema (family, agent,
//                                    agent_protocol, os_name, os_version,
//                                    cpu_arch, integrity, uiaccess, hostname,
//                                    screens[], capabilities{}, current_tier,
//                                    framings[])
//   system.capabilities     (R)  -> verb->{tier} map
//   system.health           (R)  -> empty body (literal OK 0)
//   system.verbs            (R)  -> {verbs:{<name>:<strict-tool-def>}}
//   system.power.lock       (X)  -> empty body
//   system.power.blockers   (R)  -> {blockers:[{handle,reason}]}
//   system.power.reboot     (X)  {delay_seconds,force_close_apps,reason}
//   system.power.shutdown   (X)  {delay_seconds,force_close_apps,reason}
//   system.power.logoff     (X)  {delay_seconds,force_close_apps,reason}
//   system.power.hibernate  (X)  {delay_seconds,wake_at,bypass_vm_check,reason}
//   system.power.sleep      (X)  {delay_seconds,wake_at,bypass_vm_check,reason}
//   system.power.cancel     (U)  -> {cancelled_until_ms}
//
// PHASE 2.1 — NAMED-ARG MIGRATION. These handlers no longer index
// `req.args` positionally with ad-hoc `--flag` scanning. Each handler
// declares its input_schema property list (IN SCHEMA ORDER) and reads each
// value by NAME through the shared SchemaArgs resolver (schema_args.hpp),
// with the schema's property ORDER used as the positional fallback when the
// caller invoked the verb positionally (the v2.2 reference client packs
// positional calls as `{"_args":[...]}`). The `window.*` namespace was the
// pattern-setter; this file follows it exactly. Validation emits the same
// ErrorCode::InvalidArgs + {"message":...} ergonomics as before.

#include "../capabilities.hpp"
#include "../connection.hpp"
#include "../errors.hpp"
#include "../json.hpp"
#include "../log.hpp"
#include "../platform.hpp"
#include "../sysinfo.hpp"
#include "args.hpp"
#include "schema_args.hpp"

#ifdef RH_MCP
#include "../verbs_blob.hpp"
#endif

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <powrprof.h>
#include <reason.h>

#pragma comment(lib, "powrprof.lib")

namespace remote_hands::system_verbs {

// The Phase-2.1 named-argument resolver and its invalid_args helper live in
// the shared header (schema_args.hpp) so every namespace reads through one
// definition. Pull them into this TU's unqualified name lookup; behaviour is
// identical to window.cpp (the pattern-setter).
using wire::SchemaArgs;
using wire::invalid_args;

namespace {

// Compile-time family marker. RH_MODERN is defined only for the
// windows-modern build; windows-legacy compiles the same shared TU without
// it. This is the only family discriminator available to a shared verb TU
// (sysinfo::kOsName is a single shared constant; there is no runtime family
// accessor exposed to verb handlers — see report).
#ifdef RH_MODERN
constexpr const char* kFamily = "windows-modern";
#else
constexpr const char* kFamily = "windows-legacy";
#endif

// ISO 8601 UTC timestamp ("YYYY-MM-DDTHH:MM:SSZ") for a unix time. Used for
// the power verbs' `scheduled_at` (x-output-schema: format date-time).
std::string iso8601_utc(long long unix_seconds) {
    std::time_t t = static_cast<std::time_t>(unix_seconds);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf),
                  "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

}  // namespace

// ---------------------------------------------------------------------------
// system.info — input_schema {} (no properties). x-output-schema (v2.2,
// post-rc.2): {family, agent, agent_protocol, os_name, os_version, cpu_arch,
// integrity, uiaccess, hostname, screens[], capabilities{}, current_tier,
// framings[]}. strict:false — `capabilities` is the open-ended per-family
// sub-cap map.

namespace {

struct ScreenCollector {
    std::string out;
    bool        first = true;
    int         index = 0;
};

BOOL CALLBACK enum_screen(HMONITOR mon, HDC, LPRECT, LPARAM lparam) {
    // C-ABI callback boundary: keep it noexcept-in-practice. Nothing here
    // throws (POD MONITORINFO + json string appends), but the established
    // pattern is "no C++ exception crosses an Enum* frame" — there is no
    // throwing call in this body, so no try/catch is required.
    auto* col = reinterpret_cast<ScreenCollector*>(lparam);

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) {
        // Skip a monitor we cannot describe rather than emitting a
        // schema-invalid partial entry.
        return TRUE;
    }

    if (!col->first) col->out += ',';
    col->first = false;

    const LONG x = mi.rcMonitor.left;
    const LONG y = mi.rcMonitor.top;
    const LONG w = mi.rcMonitor.right - mi.rcMonitor.left;
    const LONG h = mi.rcMonitor.bottom - mi.rcMonitor.top;

    col->out += '{';
    json::append_kv_int(col->out, "index", col->index);  col->out += ',';
    json::append_string(col->out, "bounds");
    col->out += ":{";
    json::append_kv_int(col->out, "x", x);  col->out += ',';
    json::append_kv_int(col->out, "y", y);  col->out += ',';
    json::append_kv_int(col->out, "w", w);  col->out += ',';
    json::append_kv_int(col->out, "h", h);
    col->out += "},";
    json::append_kv_bool(col->out, "primary",
                         (mi.dwFlags & MONITORINFOF_PRIMARY) != 0);
    col->out += '}';

    ++col->index;
    return TRUE;
}

std::string build_screens_array() {
    ScreenCollector col;
    col.out = "[";
    EnumDisplayMonitors(nullptr, nullptr, enum_screen,
                        reinterpret_cast<LPARAM>(&col));
    col.out += ']';
    return col.out;
}

}  // namespace

void info(Connection& conn, const wire::Request& req) {
    // No input properties (input_schema.properties == {}); still routed
    // through SchemaArgs for consistency with the migrated namespaces.
    SchemaArgs args(req, {});
    if (args.reject_unknown(conn)) return;

    std::string j;
    j += '{';

    json::append_kv_string(j, "family", kFamily);                          j += ',';
    json::append_kv_string(j, "agent", "agent-remote-hands");              j += ',';
    json::append_kv_string(j, "agent_protocol", "2.2");                    j += ',';
    json::append_kv_string(j, "os_name", sysinfo::os_name());              j += ',';
    json::append_kv_string(j, "os_version", sysinfo::os_version());        j += ',';
    json::append_kv_string(j, "cpu_arch", sysinfo::arch());                j += ',';

    // `integrity` enum: low|medium|high|system|none. sysinfo returns
    // "untrusted"/"low"/"medium"/"high"/"system" or empty; map empty (ILs
    // unavailable on this OS) to the schema's "none".
    {
        std::string integrity = sysinfo::integrity_level();
        if (integrity.empty() || integrity == "untrusted") {
            integrity = integrity.empty() ? "none" : "low";
        }
        json::append_kv_string(j, "integrity", integrity);
    }
    j += ',';

    json::append_kv_bool(j, "uiaccess", sysinfo::uiaccess_enabled());      j += ',';
    json::append_kv_string(j, "hostname", sysinfo::hostname());            j += ',';

    json::append_string(j, "screens");
    j += ':';
    j += build_screens_array();
    j += ',';

    // Open-ended per-family sub-capability map (strict:false carve-out).
    json::append_string(j, "capabilities");
    j += ":{";
    json::append_kv_string(j, "capture", "gdi");          j += ',';
    json::append_kv_string(j, "ui_automation", "uia");    j += ',';
    json::append_string(j, "image_formats");
    j += ":[\"png\",\"bmp\"],";
    const auto& ocr = platform::ocr_capabilities();
    json::append_string_array(j, "ocr_languages", ocr.languages);   j += ',';
    json::append_kv_int(j, "ocr_max_dimension", ocr.max_dimension); j += ',';
    json::append_string_array(j, "ocr_input_formats", ocr.formats); j += ',';

    // wake_timer_supported (issue #82): false on detected VM guests, where a
    // SetWaitableTimer bResume wake timer is armed successfully but the
    // hypervisor never resumes the guest. Cached after first computation.
    json::append_kv_bool(j, "wake_timer_supported",
                         sysinfo::wake_timer_supported());
    j += ',';

    // input_settings (issue #86): the host's configured pointer/keyboard
    // timings, so callers can pace synthetic input to match the OS cadence.
    {
        const sysinfo::InputSettings in = sysinfo::input_settings();
        json::append_string(j, "input_settings");
        j += ":{";
        json::append_kv_int(j, "double_click_time_ms",
                            in.double_click_time_ms);
        j += ',';
        json::append_string(j, "double_click_rect");
        j += ":{";
        json::append_kv_int(j, "w", in.double_click_w);  j += ',';
        json::append_kv_int(j, "h", in.double_click_h);
        j += "},";
        json::append_kv_int(j, "keyboard_repeat_delay_ms",
                            in.keyboard_repeat_delay_ms);
        j += ',';
        json::append_kv_int(j, "keyboard_repeat_rate_cps",
                            in.keyboard_repeat_rate_cps);
        if (in.has_wheel_scroll_lines) {
            j += ',';
            json::append_kv_int(j, "wheel_scroll_lines",
                                in.wheel_scroll_lines);
        }
        j += '}';
    }
    j += "},";

    json::append_kv_string(j, "current_tier", to_wire(conn.tier()));       j += ',';

    // Wire-framing modes this agent honours in connection.hello. Both
    // families speak MCP framing (RH_MCP); RFC 6455 binary framing ("ws")
    // is not advertised by the current build.
    json::append_string(j, "framings");
    j += ":[\"mcp\"]";

    j += '}';
    conn.writer().write_ok(j);
}

// ---------------------------------------------------------------------------
// system.capabilities — input_schema {} (no properties). x-output-schema:
// open-ended map of verb name -> {tier}.

void capabilities(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {});
    if (args.reject_unknown(conn)) return;
    conn.writer().write_ok(build_capabilities_json());
}

// ---------------------------------------------------------------------------
// system.health — input_schema {} (no properties). x-output-schema: null
// (empty body — wire response is the literal OK 0).

void health(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {});
    if (args.reject_unknown(conn)) return;
    conn.writer().write_ok();
}

// ---------------------------------------------------------------------------
// system.ping — input_schema {} (no properties). x-output-schema: null
// (empty body — wire response is the literal OK 0).
//
// Companion to system.health intended specifically for liveness probes that
// want a small, named, log-suppressible identity. The on-VM watchdog uses
// the loopback PING\n -> PONG\n sub-MCP fast-path in server.cpp's accept
// loop; that fast-path is behaviourally equivalent to invoking this verb
// (empty OK) but bypasses framing and logging. External clients that
// don't speak the fast-path use this verb via the standard wire.

void ping(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {});
    if (args.reject_unknown(conn)) return;
    conn.writer().write_ok();
}

// ---------------------------------------------------------------------------
// system.power.lock — input_schema {} (no properties). x-output-schema: null
// (empty body). x-errors: ["not_supported"].

void lock(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {});
    if (args.reject_unknown(conn)) return;

    if (!LockWorkStation()) {
        const DWORD err = GetLastError();
        log::warning(L"LockWorkStation failed (%lu)", err);
        // Only declared code in system.power.lock.json x-errors.
        conn.writer().write_err(ErrorCode::NotSupported);
        return;
    }
    conn.writer().write_ok();
}

// ---------------------------------------------------------------------------
// system.power.blockers — input_schema {} (no properties). x-output-schema:
// {blockers:[{handle,reason}]} (required). x-errors: []. Enumerates
// top-level windows that registered a ShutdownBlockReason.

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
                  static_cast<unsigned long long>(
                      reinterpret_cast<uintptr_t>(hwnd)));

    col->out += '{';
    // x-output-schema requires the key `handle` (win:0x<hex> form), matching
    // window.list / window.focus. (Pre-Phase-2.1 emitted `hwnd`, which was
    // outside the declared schema; the spec is authoritative so corrected.)
    json::append_kv_string(col->out, "handle", hbuf);
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

void shutdown_blockers(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {});
    if (args.reject_unknown(conn)) return;

    BlockerCollector col;
    col.out = "{\"blockers\":[";
    EnumWindows(enum_blocker, reinterpret_cast<LPARAM>(&col));
    col.out += "]}";
    conn.writer().write_ok(col.out);
}

// ---------------------------------------------------------------------------
// Power verbs (reboot / shutdown / logoff / hibernate / sleep)
//
// Shared input_schema for reboot/shutdown/logoff:
//   {delay_seconds (int>=0), force_close_apps (bool), reason (string)}
// hibernate/sleep add {wake_at (int>=0), bypass_vm_check (bool)} and drop
// force_close_apps.
//
// x-output-schema (reboot/shutdown/logoff/hibernate/sleep): an object with
// an optional `scheduled_at` (ISO 8601), present ONLY when delay_seconds>0;
// additionalProperties:false. delay_seconds==0 => empty object body.

namespace {

bool ensure_shutdown_privilege() {
    return sysinfo::enable_privilege(SE_SHUTDOWN_NAME);
}

struct PowerArgs {
    long long   delay_seconds = 0;
    bool        force         = false;
    std::string reason        = "planned";
};

// Resolve the shared {delay_seconds, force_close_apps, reason} input_schema
// via SchemaArgs. Returns false (and writes ERR invalid_args) on a malformed
// value; otherwise populates `out` and returns true. Property order matches
// the spec input_schema (delay_seconds, force_close_apps, reason) so the
// positional fallback slots line up.
bool resolve_power_args(Connection& conn, const wire::Request& req,
                        std::string_view verb, PowerArgs& out) {
    SchemaArgs args(req, {"delay_seconds", "force_close_apps", "reason"});
    if (args.reject_unknown(conn)) return false;

    if (args.present("delay_seconds")) {
        auto d = args.integer("delay_seconds");
        if (!d || *d < 0) {
            invalid_args(conn, std::string(verb) +
                         " 'delay_seconds' must be a non-negative integer");
            return false;
        }
        out.delay_seconds = *d;
    }

    if (args.present("force_close_apps")) {
        auto f = args.boolean("force_close_apps");
        if (!f) {
            invalid_args(conn, std::string(verb) +
                         " 'force_close_apps' must be a boolean");
            return false;
        }
        out.force = *f;
    }

    if (args.present("reason")) {
        auto r = args.str("reason");
        if (!r) {
            invalid_args(conn, std::string(verb) +
                         " 'reason' must be a string");
            return false;
        }
        out.reason = *r;
    }

    return true;
}

// Single-process pending-shutdown state. A `delay_seconds > 0` request takes
// the slot; subsequent overlapping calls reject until the timer fires or
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
    // wake_at carry-through for a delayed suspend (issue #82): the detached
    // fire thread arms the wake timer just before SetSuspendState.
    bool                                    wake_at_set = false;
    long long                               wake_at_unix = 0;
};

PendingShutdown& pending_shutdown() {
    static PendingShutdown p;
    return p;
}

// ---------------------------------------------------------------------------
// Wake-timer arming (issue #82).
//
// `wake_at` schedules an OS resume after sleep/hibernate. The mechanism is a
// Win32 waitable timer created with SetWaitableTimer(..., bResume=TRUE): the
// power manager treats the timer as a wake source and resumes the machine
// from S3/S4 when it elapses. The timer must remain valid (handle not closed,
// owning thread alive) until it fires, so we hand it to a detached
// process-lifetime thread that blocks on it. There can be at most one armed
// wake timer at a time (a second `wake_at` re-arms it).
//
// FILETIME due-time is the absolute UTC instant (positive value) computed
// from the unix `wake_at` seconds; SetWaitableTimer accepts an absolute time
// directly, avoiding negative-relative arithmetic and clock-skew between the
// request and the actual suspend.

void arm_wake_timer(long long wake_at_unix) {
    // 100ns ticks between the Windows epoch (1601-01-01) and the Unix epoch
    // (1970-01-01): 11644473600 seconds.
    constexpr long long kUnixToFileTime = 116444736000000000LL;
    const long long ft_ticks =
        kUnixToFileTime + wake_at_unix * 10000000LL;

    HANDLE timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    if (!timer) {
        log::warning(L"system.power: CreateWaitableTimer failed (%lu); "
                     L"wake_at will not resume the machine",
                     GetLastError());
        return;
    }

    LARGE_INTEGER due;
    due.QuadPart = ft_ticks;   // absolute time (positive => UTC FILETIME)

    if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, TRUE)) {
        log::warning(L"system.power: SetWaitableTimer(bResume=TRUE) failed "
                     L"(%lu); wake_at will not resume the machine",
                     GetLastError());
        CloseHandle(timer);
        return;
    }

    // Process-lifetime owner: keep the handle alive and the thread parked on
    // it so the OS retains the wake source through suspend. Detached — the
    // agent never joins it; the timer self-disarms after a single fire.
    std::thread([timer]() {
        WaitForSingleObject(timer, INFINITE);
        CloseHandle(timer);
        log::info(L"system.power: wake timer elapsed");
    }).detach();
}

void do_power(Connection& conn, UINT exit_flags, std::string_view verb,
              const wire::Request& req) {
    PowerArgs args;
    if (!resolve_power_args(conn, req, verb, args)) return;

    if (!ensure_shutdown_privilege()) {
        // Declared in reboot/shutdown/logoff x-errors.
        conn.writer().write_err(
            ErrorCode::InsufficientPrivilege,
            "{\"missing\":\"SeShutdownPrivilege\"}");
        return;
    }

    if (args.force) exit_flags |= EWX_FORCE;

    const DWORD reason =
        SHTDN_REASON_MAJOR_OPERATINGSYSTEM | SHTDN_REASON_FLAG_PLANNED;

    if (args.delay_seconds > 0) {
        // Honour delay_seconds by waiting in a detached thread, then calling
        // ExitWindowsEx. ExitWindowsEx covers reboot / shutdown / logoff
        // uniformly; the timer is bound to the agent process. See issue #59.
        auto& p = pending_shutdown();
        {
            std::lock_guard<std::mutex> lk(p.mu);
            if (p.active) {
                // NOTE: `conflict` is the faithful behaviour for an
                // overlapping delayed shutdown (the canonical conformance
                // suite asserts r.code == "conflict"), but `conflict` is NOT
                // listed in this verb's x-errors. Reported as a spec
                // inconsistency — NOT weakened away here.
                char detail[80];
                std::snprintf(detail, sizeof(detail),
                              "{\"pending_until_ms\":%lld}",
                              p.unix_deadline_ms);
                conn.writer().write_err(ErrorCode::Conflict, detail);
                return;
            }
            const auto now_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
            p.active           = true;
            p.steady_deadline  = std::chrono::steady_clock::now() +
                                 std::chrono::seconds(args.delay_seconds);
            p.unix_deadline_ms = now_ms + args.delay_seconds * 1000LL;
            p.exit_flags       = exit_flags;
            p.reason           = reason;
            // Not a suspend: clear any wake_at carry-over from a prior
            // delayed sleep/hibernate that reused this static slot.
            p.wake_at_set      = false;
            p.wake_at_unix     = 0;
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
                p.cv.wait_until(lk, deadline,
                                [&p]() { return !p.active; });
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
            // Post-privilege ExitWindowsEx failure. reboot/shutdown/logoff
            // x-errors = ["insufficient_privilege","policy_blocked"]; the
            // common Win32 failure here is ERROR_ACCESS_DENIED, which maps
            // to insufficient_privilege (the agent lacks the rights to
            // complete the call). `policy_blocked` has no ErrorCode and is
            // not emitted by this build (spec permits agents to require an
            // elevated tier instead).
            char detail[64];
            std::snprintf(detail, sizeof(detail),
                          "{\"win32_error\":%lu}", GetLastError());
            conn.writer().write_err(ErrorCode::InsufficientPrivilege, detail);
            return;
        }
    }

    // x-output-schema: empty object when immediate; {scheduled_at} when
    // delay_seconds > 0. additionalProperties:false — emit ONLY scheduled_at.
    std::string body = "{";
    if (args.delay_seconds > 0) {
        const auto now_unix =
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        json::append_kv_string(
            body, "scheduled_at",
            iso8601_utc(now_unix + args.delay_seconds));
    }
    body += '}';
    conn.writer().write_ok(body);
}

// hibernate / sleep share input_schema
// {delay_seconds, wake_at, bypass_vm_check, reason} and output schema
// {scheduled_at?}. `hibernate` selects SetSuspendState(Hibernate=TRUE).
void do_suspend(Connection& conn, BOOLEAN hibernate_flag,
                std::string_view verb, const wire::Request& req) {
    SchemaArgs args(req, {"delay_seconds", "wake_at",
                          "bypass_vm_check", "reason"});
    if (args.reject_unknown(conn)) return;

    long long delay_seconds = 0;
    if (args.present("delay_seconds")) {
        auto d = args.integer("delay_seconds");
        if (!d || *d < 0) {
            invalid_args(conn, std::string(verb) +
                         " 'delay_seconds' must be a non-negative integer");
            return;
        }
        delay_seconds = *d;
    }

    bool      have_wake_at = false;
    long long wake_at      = 0;
    bool      bypass_vm    = false;

    if (args.present("wake_at")) {
        auto w = args.integer("wake_at");
        if (!w || *w < 0) {
            invalid_args(conn, std::string(verb) +
                         " 'wake_at' must be a non-negative integer "
                         "(unix epoch seconds)");
            return;
        }
        have_wake_at = true;
        wake_at      = *w;
    }
    if (args.present("bypass_vm_check")) {
        auto b = args.boolean("bypass_vm_check");
        if (!b) {
            invalid_args(conn, std::string(verb) +
                         " 'bypass_vm_check' must be a boolean");
            return;
        }
        bypass_vm = *b;
    }
    if (args.present("reason")) {
        auto r = args.str("reason");
        if (!r) {
            invalid_args(conn, std::string(verb) +
                         " 'reason' must be a string");
            return;
        }
    }

    // VM-environment gate for wake_at (issue #82). On a detected VM guest the
    // waitable timer arms successfully but the hypervisor does not resume the
    // guest, so the machine would sleep and never wake. Reject up front
    // unless the caller explicitly opts out with bypass_vm_check. The gate
    // has no effect when wake_at is absent (spec input_schema description).
    if (have_wake_at && !bypass_vm && !sysinfo::wake_timer_supported()) {
        // system.power.sleep/hibernate x-errors include "not_supported"; the
        // spec wake_at description mandates `{"reason":"vm_environment"}`.
        conn.writer().write_err(ErrorCode::NotSupported,
                                "{\"reason\":\"vm_environment\"}");
        return;
    }

    if (!ensure_shutdown_privilege()) {
        // Declared in hibernate/sleep x-errors.
        conn.writer().write_err(
            ErrorCode::InsufficientPrivilege,
            "{\"missing\":\"SeShutdownPrivilege\"}");
        return;
    }

    auto fire = [&]() -> bool {
        // Arm the OS wake timer (bResume=TRUE) immediately before entering
        // suspend so the power manager registers it as a wake source. The
        // absolute FILETIME means a fixed wall-clock wake instant regardless
        // of how long the suspend takes to engage.
        if (have_wake_at) arm_wake_timer(wake_at);
        // SetSuspendState(Hibernate, ForceCritical=FALSE,
        // DisableWakeEvent=FALSE).
        return SetSuspendState(hibernate_flag, FALSE, FALSE) != FALSE;
    };

    if (delay_seconds > 0) {
        // Reuse the in-process pending slot so system.power.cancel can abort
        // a delayed suspend the same way it aborts a delayed shutdown.
        auto& p = pending_shutdown();
        {
            std::lock_guard<std::mutex> lk(p.mu);
            if (p.active) {
                char detail[80];
                std::snprintf(detail, sizeof(detail),
                              "{\"pending_until_ms\":%lld}",
                              p.unix_deadline_ms);
                // See do_power: `conflict` is faithful behaviour but absent
                // from this verb's x-errors. Reported, not weakened.
                conn.writer().write_err(ErrorCode::Conflict, detail);
                return;
            }
            const auto now_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
            p.active           = true;
            p.steady_deadline  = std::chrono::steady_clock::now() +
                                 std::chrono::seconds(delay_seconds);
            p.unix_deadline_ms = now_ms + delay_seconds * 1000LL;
            p.exit_flags       = 0;
            p.reason           = hibernate_flag ? 1u : 0u;
            p.wake_at_set      = have_wake_at;
            p.wake_at_unix     = wake_at;
        }
        std::thread([]() {
            auto& p = pending_shutdown();
            bool      fired;
            BOOLEAN   hib;
            bool      wake_set;
            long long wake_unix;
            {
                std::unique_lock<std::mutex> lk(p.mu);
                const auto deadline = p.steady_deadline;
                hib       = p.reason ? TRUE : FALSE;
                wake_set  = p.wake_at_set;
                wake_unix = p.wake_at_unix;
                p.cv.wait_until(lk, deadline,
                                [&p]() { return !p.active; });
                fired = p.active;
                p.active = false;
            }
            if (fired) {
                // Arm the wake timer immediately before suspend so the power
                // manager registers it as a wake source (issue #82).
                if (wake_set) arm_wake_timer(wake_unix);
                if (!SetSuspendState(hib, FALSE, FALSE)) {
                    log::warning(L"system.power: SetSuspendState failed "
                                 L"(%lu) after delayed fire",
                                 GetLastError());
                }
            } else {
                log::info(L"system.power: pending suspend cancelled");
            }
        }).detach();
    } else {
        if (!fire()) {
            // hibernate/sleep x-errors = ["insufficient_privilege",
            // "not_supported"]. A SetSuspendState failure post-privilege
            // means the OS/hardware does not support the requested state
            // (hibernation disabled, no S3, etc.) -> not_supported.
            conn.writer().write_err(ErrorCode::NotSupported);
            return;
        }
    }

    std::string body = "{";
    if (delay_seconds > 0) {
        const auto now_unix =
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        json::append_kv_string(
            body, "scheduled_at",
            iso8601_utc(now_unix + delay_seconds));
    }
    body += '}';
    conn.writer().write_ok(body);
}

}  // namespace

void reboot(Connection& conn, const wire::Request& req) {
    do_power(conn, EWX_REBOOT, "system.power.reboot", req);
}

void shutdown(Connection& conn, const wire::Request& req) {
    do_power(conn, EWX_SHUTDOWN, "system.power.shutdown", req);
}

void logoff(Connection& conn, const wire::Request& req) {
    do_power(conn, EWX_LOGOFF, "system.power.logoff", req);
}

void hibernate(Connection& conn, const wire::Request& req) {
    do_suspend(conn, TRUE, "system.power.hibernate", req);
}

void sleep(Connection& conn, const wire::Request& req) {
    do_suspend(conn, FALSE, "system.power.sleep", req);
}

// ---------------------------------------------------------------------------
// system.power.cancel — input_schema {} (no properties). x-output-schema:
// {cancelled_until_ms} (required, integer). x-errors:
// ["not_found","not_supported","insufficient_privilege"]. Aborts a pending
// in-process delayed shutdown / reboot / logoff / suspend.

void power_cancel(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {});
    if (args.reject_unknown(conn)) return;

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
        // x-output-schema: cancelled_until_ms = ms remaining on the
        // cancelled timer (0 if it was about to fire). Clamp negatives to 0.
        const auto now_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        long long remaining = unix_deadline_ms - now_ms;
        if (remaining < 0) remaining = 0;
        char body[64];
        std::snprintf(body, sizeof(body),
                      "{\"cancelled_until_ms\":%lld}", remaining);
        conn.writer().write_ok(body);
    } else {
        // Declared in system.power.cancel.json x-errors.
        conn.writer().write_err(ErrorCode::NotFound,
                                "{\"message\":\"no pending shutdown\"}");
    }
}

// ---------------------------------------------------------------------------
// system.verbs — input_schema {} (no properties). x-output-schema:
// {verbs:{<name>:<strict-tool-def>}}. Returns the full spec corpus for every
// implemented verb; backs the MCP tools/list catalogue.
//
// Only compiled with content when verbs_blob.cpp is linked (RH_MCP builds:
// windows-modern and, as of Phase 2.0, windows-legacy). For other families
// (windows-classic) the stub satisfies the linker; verb_enabled() ensures it
// is never dispatched.
//
// NOTE: system.verbs itself stays gated to the Modern family at runtime via
// verb_enabled() (capabilities.cpp) — legacy compiles the blob so tools/list
// has verb_tool_meta(), but does not surface system.verbs as a verb. This
// tools/list-backing path is preserved verbatim; only the no-op SchemaArgs
// consistency line is added.

#ifdef RH_MCP
void verbs(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {});
    if (args.reject_unknown(conn)) return;

    const auto& specs = verb_specs();

    std::string j;
    j.reserve(256 * 1024);   // typical payload ~220 KB
    j += "{\"verbs\":{";

    bool first = true;
    // Iterate the spec map and include only verbs this agent actually
    // implements (find_verb checks verb_enabled + dispatch table).
    for (const auto& [name, blob] : specs) {
        if (!find_verb(name)) continue;
        if (!first) j += ',';
        first = false;
        j += '"';
        j += name;
        j += "\":";
        j += blob;
    }

    j += "}}";
    conn.writer().write_ok(j);
}
#else
// Stub: system.verbs is gated off for non-modern families via verb_enabled().
// This definition exists only to satisfy the linker.
void verbs(Connection& conn, const wire::Request&) {
    conn.writer().write_err(ErrorCode::NotSupported, "{}");
}
#endif

}  // namespace remote_hands::system_verbs
