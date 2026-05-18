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

#include "window.h"
#include "common.h"
#include "../json.h"

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Case-insensitive substring test (C89; no strcasestr on VS6). */
static int ci_contains(const char* hay, const char* needle)
{
    size_t nl;
    size_t i;

    if (needle == NULL || needle[0] == '\0') {
        return 1;
    }
    if (hay == NULL) {
        return 0;
    }
    nl = strlen(needle);
    for (i = 0; hay[i] != '\0'; ++i) {
        size_t k = 0;
        while (k < nl &&
               (char)tolower((unsigned char)hay[i + k]) ==
               (char)tolower((unsigned char)needle[k])) {
            ++k;
        }
        if (k == nl) {
            return 1;
        }
    }
    return 0;
}

typedef struct {
    RhJson*     j;
    const char* filter;
    int         include_invisible;
} ListCtx;

static void emit_window_obj(RhJson* j, HWND h)
{
    char  hbuf[32];
    char  title[256];
    RECT  rc;
    DWORD pid;

    rh_hwnd_format(h, hbuf, (int)sizeof(hbuf));
    title[0] = '\0';
    GetWindowTextA(h, title, (int)sizeof(title));
    if (!GetWindowRect(h, &rc)) {
        rc.left = 0; rc.top = 0; rc.right = 0; rc.bottom = 0;
    }
    pid = 0;
    GetWindowThreadProcessId(h, &pid);

    rh_json_begin_obj(j);
    rh_json_key(j, "hwnd");  rh_json_str(j, hbuf);
    rh_json_key(j, "x");     rh_json_int(j, (i32)rc.left);
    rh_json_key(j, "y");     rh_json_int(j, (i32)rc.top);
    rh_json_key(j, "w");     rh_json_int(j, (i32)(rc.right - rc.left));
    rh_json_key(j, "h");     rh_json_int(j, (i32)(rc.bottom - rc.top));
    rh_json_key(j, "title"); rh_json_str(j, title);
    rh_json_key(j, "pid");   rh_json_int(j, (i32)pid);
    rh_json_end_obj(j);
}

static BOOL CALLBACK list_proc(HWND h, LPARAM lp)
{
    ListCtx* ctx = (ListCtx*)lp;
    char     title[256];

    if (!ctx->include_invisible && !IsWindowVisible(h)) {
        return TRUE;
    }
    title[0] = '\0';
    GetWindowTextA(h, title, (int)sizeof(title));
    if (ctx->filter != NULL && !ci_contains(title, ctx->filter)) {
        return TRUE;
    }
    rh_json_arr_elem(ctx->j);
    emit_window_obj(ctx->j, h);
    return TRUE;
}

void rh_verb_window_list(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = {
        { "--filter", 1 },
        { "--all",    0 }
    };
    RhArgs      a;
    RhJson*     j;
    ListCtx     ctx;
    const char* r;

    rh_args_parse(req, defs, 2, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }

    j = (RhJson*)malloc(sizeof(RhJson));
    if (j == NULL) {
        rh_err(c, "wire_desync");
        return;
    }
    rh_json_init(j);
    rh_json_begin_obj(j);
    rh_json_key(j, "windows");
    rh_json_begin_arr(j);

    ctx.j                 = j;
    ctx.filter            = a.val[0];
    ctx.include_invisible = a.seen[1];
    EnumWindows(list_proc, (LPARAM)&ctx);

    rh_json_end_arr(j);
    rh_json_end_obj(j);

    r = rh_json_finish(j);
    if (r == NULL) {
        rh_err(c, "wire_desync");
    } else {
        rh_ok_json(c, r);
    }
    free(j);
}

typedef struct {
    const char* pattern;
    HWND        found;
} FindCtx;

static BOOL CALLBACK find_proc(HWND h, LPARAM lp)
{
    FindCtx* ctx = (FindCtx*)lp;
    char     title[256];

    if (!IsWindowVisible(h)) {
        return TRUE;
    }
    title[0] = '\0';
    GetWindowTextA(h, title, (int)sizeof(title));
    if (ci_contains(title, ctx->pattern)) {
        ctx->found = h;
        return FALSE;   /* stop enumeration */
    }
    return TRUE;
}

void rh_verb_window_find(RhConn* c, const RhRequest* req)
{
    RhArgs      a;
    FindCtx     ctx;
    RhJson*     j;
    const char* r;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 1) {
        rh_err_msg(c, "invalid_args", "window.find requires <title-pattern>");
        return;
    }

    ctx.pattern = a.pos[0];
    ctx.found   = NULL;
    EnumWindows(find_proc, (LPARAM)&ctx);

    if (ctx.found == NULL) {
        rh_err(c, "not_found");
        return;
    }
    j = (RhJson*)malloc(sizeof(RhJson));
    if (j == NULL) {
        rh_err(c, "wire_desync");
        return;
    }
    rh_json_init(j);
    emit_window_obj(j, ctx.found);
    r = rh_json_finish(j);
    if (r == NULL) {
        rh_err(c, "wire_desync");
    } else {
        rh_ok_json(c, r);
    }
    free(j);
}

void rh_verb_window_focus(RhConn* c, const RhRequest* req)
{
    RhArgs      a;
    HWND        h;
    HWND        prior;
    char        pbuf[32];
    RhJson*     j;
    const char* r;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 1 || !rh_hwnd_parse(a.pos[0], &h)) {
        rh_err_msg(c, "invalid_args", "window.focus requires <hwnd>");
        return;
    }
    if (!IsWindow(h)) {
        rh_err_kv(c, "target_gone", "handle", a.pos[0]);
        return;
    }
    prior = GetForegroundWindow();
    if (!SetForegroundWindow(h)) {
        rh_err_kv(c, "lock_held", "lock_type", "foreground");
        return;
    }
    rh_hwnd_format(prior, pbuf, (int)sizeof(pbuf));

    j = (RhJson*)malloc(sizeof(RhJson));
    if (j == NULL) {
        rh_err(c, "wire_desync");
        return;
    }
    rh_json_init(j);
    rh_json_begin_obj(j);
    rh_json_key(j, "prior_hwnd");
    rh_json_str(j, pbuf);
    rh_json_end_obj(j);
    r = rh_json_finish(j);
    if (r == NULL) {
        rh_err(c, "wire_desync");
    } else {
        rh_ok_json(c, r);
    }
    free(j);
}

void rh_verb_window_close(RhConn* c, const RhRequest* req)
{
    RhArgs a;
    HWND   h;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 1 || !rh_hwnd_parse(a.pos[0], &h)) {
        rh_err_msg(c, "invalid_args", "window.close requires <hwnd>");
        return;
    }
    if (!IsWindow(h)) {
        rh_err_kv(c, "target_gone", "handle", a.pos[0]);
        return;
    }
    PostMessageA(h, WM_CLOSE, 0, 0);
    rh_ok(c);
}

void rh_verb_window_move(RhConn* c, const RhRequest* req)
{
    RhArgs a;
    HWND   h;
    int    x;
    int    y;
    int    w;
    int    hh;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 5 || !rh_hwnd_parse(a.pos[0], &h)) {
        rh_err_msg(c, "invalid_args",
                   "window.move requires <hwnd> <x> <y> <w> <h>");
        return;
    }
    if (!IsWindow(h)) {
        rh_err_kv(c, "target_gone", "handle", a.pos[0]);
        return;
    }
    x  = atoi(a.pos[1]);
    y  = atoi(a.pos[2]);
    w  = atoi(a.pos[3]);
    hh = atoi(a.pos[4]);
    MoveWindow(h, x, y, w, hh, TRUE);
    rh_ok(c);
}

void rh_verb_window_state(RhConn* c, const RhRequest* req)
{
    RhArgs      a;
    HWND        h;
    RhJson*     j;
    const char* st;
    const char* r;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 1 || !rh_hwnd_parse(a.pos[0], &h)) {
        rh_err_msg(c, "invalid_args", "window.state requires <hwnd>");
        return;
    }
    if (!IsWindow(h)) {
        rh_err_kv(c, "target_gone", "handle", a.pos[0]);
        return;
    }
    if (IsIconic(h)) {
        st = "minimised";
    } else if (IsZoomed(h)) {
        st = "maximised";
    } else if (!IsWindowVisible(h)) {
        st = "hidden";
    } else {
        st = "normal";
    }

    j = (RhJson*)malloc(sizeof(RhJson));
    if (j == NULL) {
        rh_err(c, "wire_desync");
        return;
    }
    rh_json_init(j);
    rh_json_begin_obj(j);
    rh_json_key(j, "state");
    rh_json_str(j, st);
    rh_json_end_obj(j);
    r = rh_json_finish(j);
    if (r == NULL) {
        rh_err(c, "wire_desync");
    } else {
        rh_ok_json(c, r);
    }
    free(j);
}
