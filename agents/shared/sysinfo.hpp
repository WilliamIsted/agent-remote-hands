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

#pragma once

// OS introspection helpers used by `system.info` and tier-policy decisions.
// Centralised so multiple verbs (and subscription threads) share one source
// of truth for integrity-level / privilege state.

#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace remote_hands::sysinfo {

// Target identifier for `system.info.os`. Constant for this build.
inline constexpr const char* kOsName = "windows-modern";

// Build version surfaced in `system.info.version`.
inline constexpr const char* kAgentVersion = "0.3.0";

// CPU architecture identifier.
std::string arch();

// Computer name.
std::string hostname();

// Human-readable OS product name, e.g. "Windows 11 Pro" / "Windows 10 Pro".
// Source: HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProductName.
// Returns "Windows" if the value can't be read. Used by connection.hello's
// `os_name` field (and the v2.2 system.info `os_name`).
std::string os_name();

// OS version designation, e.g. "22H2" / "23H2" or, when the marketing
// release id is absent, the "10.0.<build>" fallback. Source:
// CurrentVersion\DisplayVersion (Win10 2009+) → ReleaseId → CurrentBuild.
// Used by connection.hello's `os_version` field.
std::string os_version();

// Account the agent is running as (e.g. "DOMAIN\\username" or "username").
std::string current_user();

// Integrity-level string for an arbitrary process token handle.
// Returns one of "untrusted"/"low"/"medium"/"high"/"system", or empty on
// failure. Exposed so uipi.cpp can reuse the same decode without duplicating
// the buffer-query + RID-decode logic.
std::string integrity_level_from_token(HANDLE token);

// Token integrity level: "low" / "medium" / "high" / "system".
// Returns empty string if integrity levels can't be queried.
std::string integrity_level();

// True if the agent's token has the UIAccess flag set.
bool uiaccess_enabled();

// Names of privileges currently *enabled* in the agent's token (e.g.
// "SeShutdownPrivilege"). Disabled privileges that exist in the token are
// not returned.
std::vector<std::string> enabled_privileges();

// Attempts to enable a privilege by name in the current token. Returns true
// on success. Idempotent.
bool enable_privilege(const wchar_t* privilege_name);

// True when this host can be relied on to wake from a SetWaitableTimer
// `bResume=TRUE` wake timer (i.e. `system.power.sleep` / `system.power.hibernate`
// `wake_at` will actually resume the machine). Returns false on virtual-machine
// guests, where the timer API succeeds but the hypervisor does not honour the
// resume (issue #82). Determined by CPUID leaf 1 ECX bit 31 (hypervisor-present)
// AND a WMI `Win32_ComputerSystem.Manufacturer` vendor-string match (VMware,
// VirtualBox, QEMU, Microsoft/Hyper-V, Xen, Parallels). Computed once on first
// call and cached for the process lifetime. Surfaced as
// `system.info.capabilities.wake_timer_supported`.
bool wake_timer_supported();

// OS pointer/keyboard input timings, sourced from the Win32 user settings.
// Surfaced as `system.info.capabilities.input_settings` (issue #86) so callers
// can pace synthetic input to match the host's configured cadence.
struct InputSettings {
    // GetDoubleClickTime() — max ms between two clicks counted as a double.
    unsigned int double_click_time_ms = 0;
    // GetSystemMetrics(SM_CXDOUBLECLK / SM_CYDOUBLECLK) — the double-click
    // rectangle the second click must fall within, in pixels.
    int double_click_w = 0;
    int double_click_h = 0;
    // SystemParametersInfo(SPI_GETKEYBOARDDELAY) — 0..3 setting mapped to the
    // documented 250/500/750/1000 ms repeat-delay scale.
    unsigned int keyboard_repeat_delay_ms = 0;
    // SystemParametersInfo(SPI_GETKEYBOARDSPEED) — 0..31 setting mapped to the
    // documented ~2.5..30 characters-per-second repeat-rate scale.
    unsigned int keyboard_repeat_rate_cps = 0;
    // SystemParametersInfo(SPI_GETWHEELSCROLLLINES). Optional: WHEEL_PAGESCROLL
    // (lines == UINT_MAX, "scroll one page") and pre-wheel hosts are reported
    // as absent. `has_wheel_scroll_lines` gates emission of the JSON key.
    bool has_wheel_scroll_lines = false;
    unsigned int wheel_scroll_lines = 0;
};

// Query the live OS input timings. Cheap Win32 calls; not cached (the values
// can change at runtime via Control Panel / SystemParametersInfo broadcasts).
InputSettings input_settings();

}  // namespace remote_hands::sysinfo
