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

/* window.* (PROTOCOL.md 4.3). EnumWindows + classic user32, ANSI APIs. */

#ifndef RH_VERBS_WINDOW_H
#define RH_VERBS_WINDOW_H

#include "../connection.h"

void rh_verb_window_list(RhConn* c, const RhRequest* req);
void rh_verb_window_find(RhConn* c, const RhRequest* req);
void rh_verb_window_focus(RhConn* c, const RhRequest* req);
void rh_verb_window_close(RhConn* c, const RhRequest* req);
void rh_verb_window_move(RhConn* c, const RhRequest* req);
void rh_verb_window_state(RhConn* c, const RhRequest* req);

#endif /* RH_VERBS_WINDOW_H */
