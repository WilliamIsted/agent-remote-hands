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

/* registry.* (PROTOCOL.md 4.9). ANSI advapi32 Reg* APIs.
 *
 * v2.1 split the single registry.read / .write / .delete trio into a
 * resource-first CRUDX surface:
 *
 *   registry.key.read     -- enumerate one level (subkeys + value meta)
 *   registry.key.delete   -- delete a key (--recursive for non-empty)
 *   registry.value.read   -- fetch one value's type + data
 *   registry.value.create -- new value (refuse-if-exists)
 *   registry.value.update -- overwrite existing value (require-exists)
 *   registry.value.delete -- delete one value
 *
 * The pre-v2.1 verbs (registry.read / .write / .delete) remain advertised
 * by classic as back-compat aliases so existing clients keep working until
 * they migrate. registry.wait is unchanged.
 *
 * The dispatch table (connection.c) is updated by the main agent task; this
 * header just declares the entry points so the table can name them. */

#ifndef RH_VERBS_REGISTRY_H
#define RH_VERBS_REGISTRY_H

#include "../connection.h"

/* v2.0 (back-compat aliases). */
void rh_verb_registry_read(RhConn* c, const RhRequest* req);
void rh_verb_registry_write(RhConn* c, const RhRequest* req);
void rh_verb_registry_delete(RhConn* c, const RhRequest* req);

/* registry.wait -- unchanged across the split. */
void rh_verb_registry_wait(RhConn* c, const RhRequest* req);

/* v2.1 namespace. */
void rh_verb_registry_key_read(RhConn* c, const RhRequest* req);
void rh_verb_registry_key_delete(RhConn* c, const RhRequest* req);
void rh_verb_registry_value_read(RhConn* c, const RhRequest* req);
void rh_verb_registry_value_create(RhConn* c, const RhRequest* req);
void rh_verb_registry_value_update(RhConn* c, const RhRequest* req);
void rh_verb_registry_value_delete(RhConn* c, const RhRequest* req);

#endif /* RH_VERBS_REGISTRY_H */
