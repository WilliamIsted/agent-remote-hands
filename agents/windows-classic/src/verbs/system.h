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

/* `system.*` read-tier verbs (PROTOCOL.md 3.1, 3.2, 4.1). W9 widened the
 * handler signature to (RhConn*, const RhRequest*) for the OS verbs; these
 * three ignore `req` but must match the table's function-pointer type. */

#ifndef RH_VERBS_SYSTEM_H
#define RH_VERBS_SYSTEM_H

#include "../connection.h"

void rh_verb_system_info(RhConn* c, const RhRequest* req);
void rh_verb_system_capabilities(RhConn* c, const RhRequest* req);
void rh_verb_system_health(RhConn* c, const RhRequest* req);

#endif /* RH_VERBS_SYSTEM_H */
