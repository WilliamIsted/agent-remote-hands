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

/* Elevation-token store (PROTOCOL.md 2.6), C89 port of agents/shared/token.cpp.
 *
 * ttl_hours semantics (mirrors shared/token.cpp):
 *   -1  FOREVER: load existing file if valid, else generate + write.
 *    0  Per-session: always generate fresh; delete token file on exit via
 *       rh_token_cleanup() (call before ExitProcess / normal return).
 *   >0  Hours: reload existing file if within TTL, else delete + write new.
 *
 * Default (720 h = 30 days) matches agents/shared/config.hpp. */

#ifndef RH_TOKEN_H
#define RH_TOKEN_H

/* Initialise the token with the given TTL policy.  Generates or loads the
 * in-memory token and writes/updates the token file accordingly. */
void rh_token_init(int ttl_hours);

/* Delete the token file when ttl_hours == 0 (per-session mode).
 * No-op in all other modes.  Call before ExitProcess() on clean shutdown. */
void rh_token_cleanup(void);

/* Constant-time compare of a presented token against the live token. */
int  rh_token_verify(const char* presented);

#endif /* RH_TOKEN_H */
