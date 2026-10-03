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

static RhJson* jopen(RhConn* c)
{
    RhJson* j = (RhJson*)malloc(sizeof(RhJson));
    if (j == NULL) {
        rh_err(c, "wire_desync");
        return NULL;
    }
    rh_json_init(j);
    return j;
}

static void jsend(RhConn* c, RhJson* j)
{
    const char* r = rh_json_finish(j);
    if (r == NULL) {
        rh_err(c, "wire_desync");
    } else {
        rh_ok_json(c, r);
    }
    free(j);
}

/* Resolve the handle arg (positional 0 or --handle). Replies and returns
 * 0 on failure: unparsable -> invalid_args, dead/unknown -> not_found. */
static int get_hwnd(RhConn* c, const RhArgs* a, int handle_idx, HWND* out)
{
    const char* s = rh_arg_named_or_pos(a, handle_idx, 0);

    if (s == NULL || !rh_hwnd_parse(s, out)) {
        rh_err_msg(c, "invalid_args",
                   "requires a window handle (win:0x...)");
        return 0;
    }
    if (!IsWindow(*out)) {
        rh_err_kv(c, "not_found", "handle", s);
        return 0;
    }
    return 1;
}

/* Members of a window entry: handle, title, pid, bounds [, monitor_index].
 * Caller opens/closes the object. */
static void emit_window_members(RhJson* j, HWND h, int include_monitor)
{
    char  hbuf[32];
    char  title[256];
    RECT  rc;
    DWORD pid;
    POINT centre;

    rh_hwnd_format(h, hbuf, (int)sizeof(hbuf));
    title[0] = '\0';
    GetWindowTextA(h, title, (int)sizeof(title));
    if (!GetWindowRect(h, &rc)) {
        rc.left = 0; rc.top = 0; rc.right = 0; rc.bottom = 0;
    }
    pid = 0;
    GetWindowThreadProcessId(h, &pid);

    rh_json_key(j, "handle"); rh_json_str(j, hbuf);
    rh_json_key(j, "title");  rh_json_str(j, title);
    rh_json_key(j, "pid");    rh_json_int(j, (i32)pid);
    rh_json_key(j, "bounds"); rh_json_bounds(j, &rc);
    if (include_monitor) {
        centre.x = rc.left + (rc.right - rc.left) / 2;
        centre.y = rc.top + (rc.bottom - rc.top) / 2;
        rh_json_key(j, "monitor_index");
        rh_json_int(j, (i32)rh_point_monitor_index(centre));
    }
}

/* --- window.list ------------------------------------------------------- */

typedef struct {
    RhJson*     j;
    const char* pattern;
    DWORD       pid;          /* 0 = any */
    int         visible_only;
    int         include_monitor;
} ListCtx;

static BOOL CALLBACK list_proc(HWND h, LPARAM lp)
{
    ListCtx* ctx = (ListCtx*)lp;
    char     title[256];
    DWORD    pid;

    if (ctx->visible_only && !IsWindowVisible(h)) {
        return TRUE;
    }
    if (ctx->pid != 0) {
        pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid != ctx->pid) {
            return TRUE;
        }
    }
    if (ctx->pattern != NULL) {
        title[0] = '\0';
        GetWindowTextA(h, title, (int)sizeof(title));
        if (!rh_contains_ci(title, ctx->pattern)) {
            return TRUE;
        }
    }
    rh_json_arr_elem(ctx->j);
    rh_json_begin_obj(ctx->j);
    emit_window_members(ctx->j, h, ctx->include_monitor);
    rh_json_end_obj(ctx->j);
    return TRUE;
}

void rh_verb_window_list(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = {
        { "--visible-only",    RH_FLAG_BOOL  },
        { "--pid",             RH_FLAG_VALUE },
        { "--pattern",         RH_FLAG_VALUE },
        { "--include-monitor", RH_FLAG_BOOL  }
    };
    RhArgs  a;
    RhJson* j;
    ListCtx ctx;
    long    pid;

    rh_args_parse(req, defs, 4, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    pid = 0;
    if (a.missing_val ||
        (a.val[1] != NULL && (!rh_parse_long(a.val[1], &pid) || pid < 1))) {
        rh_err_msg(c, "invalid_args", "pid must be a positive integer");
        return;
    }

    j = jopen(c);
    if (j == NULL) {
        return;
    }
    rh_json_begin_arr(j);   /* bare array per spec */
    ctx.j               = j;
    ctx.pattern         = a.val[2];
    ctx.pid             = (DWORD)pid;
    ctx.visible_only    = rh_arg_bool(&a, 0, 1);
    ctx.include_monitor = rh_arg_bool(&a, 3, 0);
    EnumWindows(list_proc, (LPARAM)&ctx);
    rh_json_end_arr(j);
    jsend(c, j);
}

/* --- window.find ------------------------------------------------------- */

#define MATCH_SUBSTRING 0
#define MATCH_PREFIX    1
#define MATCH_EXACT     2
#define MATCH_GLOB      3

typedef struct {
    const char* pattern;
    int         mode;
    HWND        found;
} FindCtx;

static int title_matches(const char* title, const char* pat, int mode)
{
    size_t n;

    switch (mode) {
        case MATCH_PREFIX:
            n = strlen(pat);
            return (strlen(title) >= n &&
                    CompareStringA(LOCALE_SYSTEM_DEFAULT, NORM_IGNORECASE,
                                   title, (int)n, pat, (int)n) == 2) ? 1 : 0;
        case MATCH_EXACT:
            return (lstrcmpiA(title, pat) == 0) ? 1 : 0;
        case MATCH_GLOB:
            return rh_glob_match(pat, title);
        default:
            return rh_contains_ci(title, pat);
    }
}

static BOOL CALLBACK find_proc(HWND h, LPARAM lp)
{
    FindCtx* ctx = (FindCtx*)lp;
    char     title[256];

    if (!IsWindowVisible(h)) {
        return TRUE;
    }
    title[0] = '\0';
    GetWindowTextA(h, title, (int)sizeof(title));
    if (title_matches(title, ctx->pattern, ctx->mode)) {
        ctx->found = h;
        return FALSE;   /* stop enumeration */
    }
    return TRUE;
}

void rh_verb_window_find(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = {
        { "--match",   RH_FLAG_VALUE },
        { "--pattern", RH_FLAG_VALUE }
    };
    RhArgs      a;
    FindCtx     ctx;
    RhJson*     j;
    const char* m;

    rh_args_parse(req, defs, 2, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    ctx.pattern = rh_arg_named_or_pos(&a, 1, 0);
    if (ctx.pattern == NULL || ctx.pattern[0] == '\0' || a.missing_val) {
        rh_err_msg(c, "invalid_args", "window.find requires <pattern>");
        return;
    }
    m = a.val[0];
    if (m == NULL || strcmp(m, "substring") == 0) {
        ctx.mode = MATCH_SUBSTRING;
    } else if (strcmp(m, "prefix") == 0) {
        ctx.mode = MATCH_PREFIX;
    } else if (strcmp(m, "exact") == 0) {
        ctx.mode = MATCH_EXACT;
    } else if (strcmp(m, "glob") == 0) {
        ctx.mode = MATCH_GLOB;
    } else if (strcmp(m, "regex") == 0) {
        /* No regex engine in the C89 classic build. */
        rh_err_kv(c, "invalid_args", "reason",
                  "match=regex is not supported on windows-classic");
        return;
    } else {
        rh_err_msg(c, "invalid_args",
                   "match must be substring|prefix|exact|glob|regex");
        return;
    }

    ctx.found = NULL;
    EnumWindows(find_proc, (LPARAM)&ctx);
    if (ctx.found == NULL) {
        rh_err(c, "not_found");
        return;
    }
    j = jopen(c);
    if (j == NULL) {
        return;
    }
    rh_json_begin_obj(j);
    emit_window_members(j, ctx.found, 0);
    rh_json_end_obj(j);
    jsend(c, j);
}

/* --- window.focus / close / move / state ------------------------------- */

void rh_verb_window_focus(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = { { "--handle", RH_FLAG_VALUE } };
    RhArgs  a;
    HWND    h;
    HWND    prior;
    char    pbuf[32];
    int     ok;
    RhJson* j;

    rh_args_parse(req, defs, 1, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (!get_hwnd(c, &a, 0, &h)) {
        return;
    }
    prior = GetForegroundWindow();
    pbuf[0] = '\0';
    if (prior != NULL) {
        rh_hwnd_format(prior, pbuf, (int)sizeof(pbuf));
    }
    /* Foreground-lock refusal is success-with-status, not an ERR. */
    ok = SetForegroundWindow(h) ? 1 : 0;

    j = jopen(c);
    if (j == NULL) {
        return;
    }
    rh_json_begin_obj(j);
    rh_json_key(j, "prior_handle");   rh_json_str(j, pbuf);
    rh_json_key(j, "focused_status"); rh_json_str(j, ok ? "ok" : "lock_held");
    rh_json_end_obj(j);
    jsend(c, j);
}

void rh_verb_window_close(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = { { "--handle", RH_FLAG_VALUE } };
    RhArgs a;
    HWND   h;

    rh_args_parse(req, defs, 1, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (!get_hwnd(c, &a, 0, &h)) {
        return;
    }
    PostMessageA(h, WM_CLOSE, 0, 0);
    rh_ok(c);
}

void rh_verb_window_move(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = {
        { "--handle",     RH_FLAG_VALUE },
        { "--x",          RH_FLAG_VALUE },
        { "--y",          RH_FLAG_VALUE },
        { "--w",          RH_FLAG_VALUE },
        { "--h",          RH_FLAG_VALUE },
        { "--foreground", RH_FLAG_BOOL  }
    };
    RhArgs      a;
    HWND        h;
    RECT        prior;
    RECT        now;
    const char* vals[4];
    long        num[4];
    int         next_pos;
    int         i;
    const char* fg;
    RhJson*     j;

    rh_args_parse(req, defs, 6, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (!get_hwnd(c, &a, 0, &h)) {
        return;
    }
    if (!GetWindowRect(h, &prior)) {
        rh_err(c, rh_win32_code(GetLastError()));
        return;
    }
    /* Positional grammar <handle> <x> <y> [<w> <h>]; each may also come
     * as a --x/--y/--w/--h flag. Omitted w/h keep the current size. */
    next_pos = a.seen[0] ? 0 : 1;
    num[0] = prior.left;
    num[1] = prior.top;
    num[2] = prior.right - prior.left;
    num[3] = prior.bottom - prior.top;
    for (i = 0; i < 4; ++i) {
        vals[i] = a.seen[i + 1] ? a.val[i + 1]
                : (next_pos < a.npos ? a.pos[next_pos++] : NULL);
        if (vals[i] == NULL) {
            if (i < 2) {
                rh_err_msg(c, "invalid_args",
                           "window.move requires <handle> <x> <y>");
                return;
            }
            continue;
        }
        if (!rh_parse_long(vals[i], &num[i]) || (i >= 2 && num[i] < 0)) {
            rh_err_msg(c, "invalid_args", "x/y/w/h must be integers");
            return;
        }
    }
    if (!MoveWindow(h, (int)num[0], (int)num[1], (int)num[2], (int)num[3],
                    TRUE)) {
        rh_err(c, rh_win32_code(GetLastError()));
        return;
    }
    fg = "not_requested";
    if (rh_arg_bool(&a, 5, 0)) {
        fg = SetForegroundWindow(h) ? "ok" : "lock_held";
    }
    if (!GetWindowRect(h, &now)) {
        now = prior;
    }

    j = jopen(c);
    if (j == NULL) {
        return;
    }
    rh_json_begin_obj(j);
    rh_json_key(j, "bounds");            rh_json_bounds(j, &now);
    rh_json_key(j, "prior_bounds");      rh_json_bounds(j, &prior);
    rh_json_key(j, "foreground_status"); rh_json_str(j, fg);
    rh_json_end_obj(j);
    jsend(c, j);
}

void rh_verb_window_state(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = { { "--handle", RH_FLAG_VALUE } };
    RhArgs      a;
    HWND        h;
    RhJson*     j;
    const char* st;

    rh_args_parse(req, defs, 1, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (!get_hwnd(c, &a, 0, &h)) {
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

    j = jopen(c);
    if (j == NULL) {
        return;
    }
    rh_json_begin_obj(j);
    rh_json_key(j, "state"); rh_json_str(j, st);
    rh_json_end_obj(j);
    jsend(c, j);
}
