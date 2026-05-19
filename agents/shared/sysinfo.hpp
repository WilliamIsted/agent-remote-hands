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

}  // namespace remote_hands::sysinfo
