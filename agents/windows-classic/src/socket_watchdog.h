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

/* Socket-state watchdog for the windows-classic agent.
 *
 * Periodically checks whether the listen socket is still alive.  If it is
 * dead (e.g. after a suspend/resume cycle where the power-broadcast path
 * was not delivered), calls rh_server_rebind() to open a new one.
 *
 * Probe interval: 30 seconds.  Silent on success; logs on dead + rebind. */

#ifndef RH_SOCKET_WATCHDOG_H
#define RH_SOCKET_WATCHDOG_H

#include "types.h"
#include <winsock.h>

/* Start the socket watchdog thread.  Call once after the listen socket is
 * first opened.  The port is passed so the watchdog can supply it to
 * rh_server_rebind() without extra shared state. */
void rh_watchdog_start(u16 port);

/* Called by server.c whenever the listen socket is replaced (on each
 * successful rebind).  The watchdog must be told the new socket so it
 * monitors the right descriptor. Thread-safe. */
void rh_watchdog_update_socket(SOCKET s);

#endif /* RH_SOCKET_WATCHDOG_H */
