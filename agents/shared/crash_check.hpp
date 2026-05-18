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

// App-crash per-call validation (Approach A from the crash-detection design).
//
// Closes #46. Concretely:
//
//   1. After a successful `window.focus`, the Connection records the target
//      hwnd + owning pid via `note_focus_target`.
//   2. Before each high-cost input verb (input.click, input.type, …) the
//      handler calls `check_focus_or_fail`, which compares the current
//      foreground hwnd / pid against the tracked target. If the target has
//      vanished or the foreground has switched away, the verb writes
//      `ERR target_gone` and returns false — the verb does not proceed.
//
// Before any window.focus has happened on the connection, the tracked pid is
// zero and check_focus_or_fail is a no-op (returns true), so verbs run as
// before — we only enforce the contract once the caller has explicitly told
// us what they're targeting.

namespace remote_hands {
class Connection;
}

namespace remote_hands::crash_check {

// Returns true if the connection's tracked focus target is still alive and
// in the foreground. Returns false after writing `ERR target_gone` if not.
//
// Called by every update-tier input verb before injecting.
bool check_focus_or_fail(Connection& conn);

}  // namespace remote_hands::crash_check
