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

// User Interface Privilege Isolation (UIPI) helpers.
//
// Windows blocks synthetic input from a process to a window owned by a
// process at a higher integrity level. Pre-v2 the input verbs returned `OK`
// regardless, which made UIPI silent and confusing. v2 surfaces it as
// `ERR uipi_blocked` with diagnostic detail.
//
// The agent's own integrity is queried once at startup; the foreground or
// target-window integrity is queried per call (it can change between calls).

#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace remote_hands {
class Connection;
}

namespace remote_hands::uipi {

// Returns the agent's own integrity level (memoised).
// One of "untrusted" / "low" / "medium" / "high" / "system" or empty if
// the OS doesn't expose integrity levels.
const std::string& agent_integrity();

// Returns the integrity of the process owning `hwnd`. Empty on failure.
std::string window_integrity(HWND hwnd);

// Returns true if the agent should be allowed to inject input at `target_il`.
// Empty strings on either side return true (no block known).
bool input_allowed(const std::string& agent_il, const std::string& target_il);

// Convenience: checks the foreground window. If a UIPI block is detected,
// writes `ERR uipi_blocked` and returns false. Otherwise returns true.
bool check_foreground_or_fail(Connection& conn);

// Convenience: checks a specific window. Same return contract.
bool check_window_or_fail(Connection& conn, HWND target);

}  // namespace remote_hands::uipi
