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

/* Per-connection state machine + verb dispatch, C89 port of
 * agents/shared/connection.cpp.
 *
 * NOTE (deviation from the W8 prompt sketch): the prompt folded dispatch
 * into server.c and listed only an rh_do_hello() helper. The real contract
 * (tests/conformance/test_connection.py + PROTOCOL.md 2.1) requires the full
 * pre-hello state machine, connection.tier_raise/tier_drop/reset/close, and
 * the protocol_mismatch / invalid_state / auth_invalid behaviours. That is
 * wire-core, not an OS verb, so it lives here and server.c stays a thin
 * listen/accept loop. */

#ifndef RH_CONNECTION_H
#define RH_CONNECTION_H

#include "types.h"
#include "protocol.h"

/* Tier ladder (PROTOCOL.md 7). */
#define RH_TIER_READ         0
#define RH_TIER_CREATE       1
#define RH_TIER_UPDATE       2
#define RH_TIER_DELETE       3
#define RH_TIER_EXTRA_RISKY  4

/* Connection state (PROTOCOL.md 2.1). */
#define RH_ST_PRE_HELLO  0
#define RH_ST_CONNECTED  1
#define RH_ST_CLOSED     2

#define RH_MAX_CONNECTIONS 4

typedef struct {
    SOCKET   sock;
    RhReader reader;
    int      state;
    int      tier;
} RhConn;

const char* rh_tier_name(int tier);            /* NULL if out of range  */
int         rh_tier_from_name(const char* s);  /* -1 if unknown / v2.0  */

/* Verb-table introspection so verbs/system.c can build the
 * system.capabilities map from the single source of truth (the dispatch
 * table) instead of a parallel hand-maintained list. */
int  rh_verb_table_count(void);
void rh_verb_table_get(int i, const char** verb_out,
                       const char** tier_name_out);

/* Owns the socket: runs the pre-hello -> connected -> closed lifecycle,
 * then closes it. Single connection at a time (W8); threading is W9. */
void rh_connection_run(SOCKET s);

#endif /* RH_CONNECTION_H */
