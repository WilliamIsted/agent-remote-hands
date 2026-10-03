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
#include "registry.h"
#include "../json.h"

#include <windows.h>
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
    static const RhFlagDef defs[] = {
        { "--title-prefix", RH_FLAG_VALUE }
    };
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
    static const RhFlagDef defs[] = { { "--pid", RH_FLAG_VALUE } };
    RhArgs      a;
    const char* s;
    long        pid;
    HANDLE      h;

    rh_args_parse(req, defs, 1, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    s = rh_arg_named_or_pos(&a, 0, 0);
    if (s == NULL || !rh_parse_long(s, &pid) || pid < 0) {
        rh_err_msg(c, "invalid_args", "watch.process requires <pid>");
        return;
    }
    /* The System pseudo-processes (0, 4) refuse OpenProcess but exist. */
    if (pid > 4) {
        h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
        if (h == NULL && GetLastError() != ERROR_ACCESS_DENIED) {
            rh_err_kv(c, "not_found", "pid", s);
            return;
        }
        if (h != NULL) {
            CloseHandle(h);
        }
    }
    emit_subscription(c);
}

void rh_verb_watch_region(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = {
        { "--region",       RH_FLAG_VALUE },
        { "--interval-ms",  RH_FLAG_VALUE },
        { "--until-change", RH_FLAG_BOOL  },
        { "--encoding",     RH_FLAG_VALUE }
    };
    RhArgs      a;
    const char* region;
    long        x;
    long        y;
    long        w;
    long        h;
    long        interval;

    rh_args_parse(req, defs, 4, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    region = rh_arg_named_or_pos(&a, 0, 0);
    if (region == NULL ||
        sscanf(region, "%ld,%ld,%ld,%ld", &x, &y, &w, &h) != 4 ||
        w <= 0 || h <= 0) {
        rh_err_msg(c, "invalid_args",
                   "watch.region requires <x>,<y>,<w>,<h>");
        return;
    }
    if (a.val[1] != NULL && (!rh_parse_long(a.val[1], &interval) ||
                             interval <= 0)) {
        rh_err_msg(c, "invalid_args", "interval_ms must be > 0");
        return;
    }
    if (a.val[3] != NULL && strcmp(a.val[3], "binary") != 0 &&
        strcmp(a.val[3], "base64") != 0) {
        rh_err_msg(c, "invalid_args", "encoding must be binary|base64");
        return;
    }
    if (a.missing_val) {
        rh_err_msg(c, "invalid_args", "flag requires a value");
        return;
    }
    emit_subscription(c);
}

void rh_verb_watch_file(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = {
        { "--recursive", RH_FLAG_BOOL  },
        { "--glob",      RH_FLAG_VALUE }
    };
    RhArgs a;

    rh_args_parse(req, defs, 2, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (rh_arg_named_or_pos(&a, 1, 0) == NULL) {
        rh_err_msg(c, "invalid_args", "watch.file requires <glob>");
        return;
    }
    emit_subscription(c);
}

void rh_verb_watch_registry(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = {
        { "--watch-subtree", RH_FLAG_BOOL  },
        { "--until-change",  RH_FLAG_BOOL  },
        { "--timeout-ms",    RH_FLAG_VALUE },
        { "--path",          RH_FLAG_VALUE }
    };
    RhArgs      a;
    const char* path;
    long        timeout_ms;

    rh_args_parse(req, defs, 4, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    path = rh_arg_named_or_pos(&a, 3, 0);
    if (path == NULL || a.missing_val) {
        rh_err_msg(c, "invalid_args", "watch.registry requires <path>");
        return;
    }
    timeout_ms = 30000;
    if (a.val[2] != NULL && (!rh_parse_long(a.val[2], &timeout_ms) ||
                             timeout_ms < 0)) {
        rh_err_msg(c, "invalid_args", "timeout_ms must be >= 0");
        return;
    }
    if (rh_arg_bool(&a, 1, 0)) {
        /* Synchronous one-shot: OK {path} on change, else ERR timeout. */
        rh_registry_wait_change(c, path, rh_arg_bool(&a, 0, 0), timeout_ms);
        return;
    }
    emit_subscription(c);
}

void rh_verb_watch_cancel(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = {
        { "--subscription-id", RH_FLAG_VALUE }
    };
    RhArgs a;

    rh_args_parse(req, defs, 1, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (rh_arg_named_or_pos(&a, 0, 0) == NULL) {
        rh_err_msg(c, "invalid_args", "watch.cancel requires <subscription_id>");
        return;
    }
    /* Idempotent on any id, known or not (PROTOCOL.md 10.2). No
     * subscription bookkeeping exists because no EVENT thread exists. */
    rh_ok(c);
}
