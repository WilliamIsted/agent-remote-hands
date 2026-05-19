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

/* TCP listener + single-threaded accept loop (W8). The per-thread
 * connection model (PROTOCOL.md 1.1) and connection limits land in W9. */

#ifndef RH_SERVER_H
#define RH_SERVER_H

#include "types.h"

/* Generates the elevation token, opens the listener, then serves
 * connections one at a time until a fatal error. Returns 0 on clean
 * shutdown, non-zero on a startup failure.
 * token_ttl_hours: -1 = FOREVER, 0 = per-session, >0 = hours. */
int rh_server_run(u16 port, int token_ttl_hours);

/* Closes the current listen socket and re-binds a new one on the same port.
 * Safe to call from any thread.  No-op if a rebind is already in progress. */
void rh_server_rebind(u16 port);

/* Called on PBT_APMSUSPEND: cleanly closes the listen socket before the OS
 * tears it down during suspend. */
void rh_server_suspend(void);

#endif /* RH_SERVER_H */
