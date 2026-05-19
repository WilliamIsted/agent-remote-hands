/*   Copyright 2026 William Isted and contributors
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

/* Power-broadcast watcher for the windows-classic agent.
 *
 * A background thread creates an invisible window and pumps its message
 * queue.  On WM_POWERBROADCAST / PBT_APMSUSPEND the listen socket is closed
 * cleanly before the OS tears it down.  On any resume event rh_server_rebind()
 * is called to open a new listen socket.
 *
 * Uses a real (non-message-only) window so this works on NT4/9x where
 * HWND_MESSAGE does not exist. */

#ifndef RH_POWER_WATCHER_H
#define RH_POWER_WATCHER_H

#include "types.h"

/* Start the hidden-window power-broadcast thread.  Call once after the
 * listen socket is first opened. */
void rh_power_watcher_start(u16 port);

/* Signal the pump thread to exit.  Called on agent shutdown (optional). */
void rh_power_watcher_stop(void);

#endif /* RH_POWER_WATCHER_H */
