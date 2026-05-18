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

/* directory.* (PROTOCOL.md 4.7). New v2.1 namespace; ANSI FindFirstFileA. */

#ifndef RH_VERBS_DIRECTORY_H
#define RH_VERBS_DIRECTORY_H

#include "../connection.h"

void rh_verb_directory_list(RhConn* c, const RhRequest* req);
void rh_verb_directory_stat(RhConn* c, const RhRequest* req);
void rh_verb_directory_exists(RhConn* c, const RhRequest* req);
void rh_verb_directory_create(RhConn* c, const RhRequest* req);
void rh_verb_directory_rename(RhConn* c, const RhRequest* req);
void rh_verb_directory_remove(RhConn* c, const RhRequest* req);

#endif /* RH_VERBS_DIRECTORY_H */
