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

/* watch.* (PROTOCOL.md 4.11 / 6). Registration only: a subscription id is
 * allocated and returned; watch.cancel is idempotent. EVENT-frame delivery
 * is NOT implemented -- the classic agent is single-threaded (W8) and the
 * conformance suite explicitly does not exercise delivery ("verifying
 * actual EVENT delivery is left to integration scenarios"). watch.element
 * is omitted entirely per D9. */

#ifndef RH_VERBS_WATCH_H
#define RH_VERBS_WATCH_H

#include "../connection.h"

void rh_verb_watch_window(RhConn* c, const RhRequest* req);
void rh_verb_watch_process(RhConn* c, const RhRequest* req);
void rh_verb_watch_region(RhConn* c, const RhRequest* req);
void rh_verb_watch_file(RhConn* c, const RhRequest* req);
void rh_verb_watch_registry(RhConn* c, const RhRequest* req);
void rh_verb_watch_cancel(RhConn* c, const RhRequest* req);

#endif /* RH_VERBS_WATCH_H */
