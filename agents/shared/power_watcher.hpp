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

// Power-broadcast watcher for windows-modern and windows-legacy.
//
// A background thread creates a message-only hidden window (HWND_MESSAGE,
// safe on Win2000+ / XP SP3+ / Win10) and pumps its message queue.
//
// on_suspend is called on PBT_APMSUSPEND -- close the listen socket cleanly
//            before the OS tears it down.
// on_resume  is called on any resume event -- trigger a socket rebind.

#include <functional>

namespace remote_hands {

// Start the power-broadcast watcher thread.
void power_watcher_start(std::function<void()> on_suspend,
                         std::function<void()> on_resume);

// Signal the pump thread to stop. Call from the server destructor.
void power_watcher_stop();

}  // namespace remote_hands
