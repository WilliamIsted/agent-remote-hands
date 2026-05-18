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

#include "watch.h"
#include "common.h"
#include "../json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Connection-scoped-ish monotonic counter. The single-threaded model
 * serves one connection at a time so a process-global counter is
 * sufficient and never collides within a connection. */
static int g_sub_next = 1;

static void emit_subscription(RhConn* c)
{
    char        idbuf[32];
    RhJson*     j;
    const char* r;

    _snprintf(idbuf, sizeof(idbuf), "sub:%d", g_sub_next);
    idbuf[sizeof(idbuf) - 1] = '\0';
    g_sub_next += 1;

    j = (RhJson*)malloc(sizeof(RhJson));
    if (j == NULL) {
        rh_err(c, "wire_desync");
        return;
    }
    rh_json_init(j);
    rh_json_begin_obj(j);
    rh_json_key(j, "subscription_id");
    rh_json_str(j, idbuf);
    rh_json_end_obj(j);
    r = rh_json_finish(j);
    if (r == NULL) {
        rh_err(c, "wire_desync");
    } else {
        rh_ok_json(c, r);
    }
    free(j);
}

void rh_verb_watch_window(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = { { "--title-prefix", 1 } };
    RhArgs a;

    rh_args_parse(req, defs, 1, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.val[0] == NULL) {
        rh_err_msg(c, "invalid_args",
                   "watch.window requires --title-prefix <pattern>");
        return;
    }
    emit_subscription(c);
}

void rh_verb_watch_process(RhConn* c, const RhRequest* req)
{
    RhArgs a;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 1) {
        rh_err_msg(c, "invalid_args", "watch.process requires <pid>");
        return;
    }
    emit_subscription(c);
}

void rh_verb_watch_region(RhConn* c, const RhRequest* req)
{
    RhArgs a;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 1) {
        rh_err_msg(c, "invalid_args",
                   "watch.region requires <x>,<y>,<w>,<h>");
        return;
    }
    emit_subscription(c);
}

void rh_verb_watch_file(RhConn* c, const RhRequest* req)
{
    RhArgs a;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 1) {
        rh_err_msg(c, "invalid_args", "watch.file requires <pattern>");
        return;
    }
    emit_subscription(c);
}

void rh_verb_watch_registry(RhConn* c, const RhRequest* req)
{
    RhArgs a;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 1) {
        rh_err_msg(c, "invalid_args", "watch.registry requires <path>");
        return;
    }
    emit_subscription(c);
}

void rh_verb_watch_cancel(RhConn* c, const RhRequest* req)
{
    RhArgs a;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    /* Idempotent on any id, known or not (PROTOCOL.md 10.2). No
     * subscription bookkeeping exists because no EVENT thread exists. */
    rh_ok(c);
}
