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
 * A fresh 256-bit token is generated each start, hex-encoded, held in memory
 * for connection.tier_raise verification, and written best-effort to
 * %ProgramData%\AgentRemoteHands\token. ACL hardening is the installer's job
 * (see the note in shared/token.cpp); W8 only needs the in-memory value so
 * the auth_invalid path is exercised correctly. */

#ifndef RH_TOKEN_H
#define RH_TOKEN_H

/* Generate the token and attempt to write the token file. Always succeeds
 * for the in-memory token (random source has a documented weak fallback);
 * file-write failure is non-fatal. */
void rh_token_init(void);

/* Constant-time compare of a presented token against the live token. */
int  rh_token_verify(const char* presented);

#endif /* RH_TOKEN_H */
