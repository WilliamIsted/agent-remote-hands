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

/* process.* (PROTOCOL.md 4.8). Toolhelp32 (Win9x/2000) with a PSAPI
 * fallback for NT4, both bound at runtime via GetProcAddress so the binary
 * loads on hosts lacking either. process.start caches the process HANDLE so
 * process.wait works after the OS has reaped the child (regression #16). */

#ifndef RH_VERBS_PROCESS_H
#define RH_VERBS_PROCESS_H

#include "../connection.h"

void rh_verb_process_list(RhConn* c, const RhRequest* req);
void rh_verb_process_start(RhConn* c, const RhRequest* req);
void rh_verb_process_shell(RhConn* c, const RhRequest* req);
void rh_verb_process_kill(RhConn* c, const RhRequest* req);
void rh_verb_process_wait(RhConn* c, const RhRequest* req);

#endif /* RH_VERBS_PROCESS_H */
