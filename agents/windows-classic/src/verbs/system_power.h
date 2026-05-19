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

/* system.power.* verb handlers for windows-classic.
 *
 * Call rh_power_init() once from server.c before accepting any connections.
 * All other functions are verb handlers following the standard (RhConn*,
 * const RhRequest*) signature. */

#ifndef VERBS_SYSTEM_POWER_H
#define VERBS_SYSTEM_POWER_H

#include "../connection.h"
#include "../protocol.h"

void rh_power_init(void);

void rh_verb_power_blockers(RhConn* c, const RhRequest* req);
void rh_verb_power_lock(RhConn* c, const RhRequest* req);
void rh_verb_power_reboot(RhConn* c, const RhRequest* req);
void rh_verb_power_shutdown(RhConn* c, const RhRequest* req);
void rh_verb_power_logoff(RhConn* c, const RhRequest* req);
void rh_verb_power_hibernate(RhConn* c, const RhRequest* req);
void rh_verb_power_sleep(RhConn* c, const RhRequest* req);
void rh_verb_power_cancel(RhConn* c, const RhRequest* req);

#endif /* VERBS_SYSTEM_POWER_H */
