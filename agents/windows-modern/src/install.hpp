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

// `--install` / `--uninstall` flag handlers.
//
// Install:
//   - Copies the current binary to %ProgramFiles%\AgentRemoteHands\.
//   - Registers a Task Scheduler logon-task running the installed binary so
//     the agent autostarts in the user's interactive desktop session on the
//     next logon.
//   - Adds an inbound TCP firewall rule on the configured port.
//
// Uninstall: removes all of the above.
//
// Both require Administrator privileges. We shell out to schtasks.exe and
// netsh.exe rather than driving the Task Scheduler / firewall COM APIs
// directly — the resulting code is shorter, easier to audit, and matches
// what a sysadmin would do by hand.

namespace remote_hands {
struct Config;
}

namespace remote_hands::install {

// Returns 0 on success, non-zero on failure (with diagnostic on stderr).
int run_install(const Config& config);
int run_uninstall(const Config& config);

}  // namespace remote_hands::install
