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

#include "common.h"
#include "../json.h"
#include "../protocol.h"

#include <stdlib.h>
#include <string.h>

/* RhJson is RH_MAX_JSON_LEN (64 KB) wide; heap-allocate it for the tiny
 * error/detail documents so a trivial ERR never costs a 64 KB stack frame. */
static RhJson* jnew(void)
{
    RhJson* j = (RhJson*)malloc(sizeof(RhJson));
    if (j != NULL) {
        rh_json_init(j);
    }
    return j;
}

/* --- response emitters ------------------------------------------------- */

void rh_ok(RhConn* c)
{
    rh_send_ok(c->sock);
}

void rh_ok_json(RhConn* c, const char* json)
{
    rh_send_ok_json(c->sock, json);
}

void rh_ok_bytes(RhConn* c, const char* data, int len)
{
    rh_send_ok_bytes(c->sock, data, len);
}

void rh_err(RhConn* c, const char* code)
{
    rh_send_err(c->sock, code);
}

void rh_err_kv(RhConn* c, const char* code,
               const char* key, const char* value)
{
    RhJson*     j;
    const char* r;

    j = jnew();
    if (j == NULL) {
        rh_send_err(c->sock, code);
        return;
    }
    rh_json_begin_obj(j);
    rh_json_key(j, key);
    rh_json_str(j, value);
    rh_json_end_obj(j);
    r = rh_json_finish(j);
    if (r != NULL) {
        rh_send_err_json(c->sock, code, r);
    } else {
        rh_send_err(c->sock, code);
    }
    free(j);
}

void rh_err_msg(RhConn* c, const char* code, const char* message)
{
    rh_err_kv(c, code, "message", message);
}

void rh_err_unknown_flag(RhConn* c, const char* flag)
{
    rh_err_kv(c, "invalid_args", "unknown_flag", flag);
}

/* --- argument / flag parser ------------------------------------------- */

static int is_double_dash(const char* t)
{
    return (t != NULL && t[0] == '-' && t[1] == '-') ? 1 : 0;
}

void rh_args_parse(const RhRequest* req, const RhFlagDef* defs,
                   int ndefs, RhArgs* out)
{
    int i;
    int k;

    out->npos        = 0;
    out->unknown     = NULL;
    out->missing_val = 0;
    for (k = 0; k < RH_MAX_FLAGS; ++k) {
        out->val[k]  = NULL;
        out->seen[k] = 0;
    }

    i = 0;
    while (i < req->argc) {
        const char* tok = req->args[i];
        int matched = 0;

        for (k = 0; k < ndefs && k < RH_MAX_FLAGS; ++k) {
            if (strcmp(tok, defs[k].name) == 0) {
                matched = 1;
                out->seen[k] = 1;
                if (defs[k].has_val) {
                    if (i + 1 < req->argc) {
                        out->val[k] = req->args[i + 1];
                        i += 2;
                    } else {
                        out->missing_val = 1;
                        i += 1;
                    }
                } else {
                    i += 1;
                }
                break;
            }
        }
        if (matched) {
            continue;
        }

        if (is_double_dash(tok)) {
            if (out->unknown == NULL) {
                out->unknown = tok;
            }
            i += 1;
            continue;
        }

        if (out->npos < RH_MAX_ARGS) {
            out->pos[out->npos] = tok;
            out->npos += 1;
        }
        i += 1;
    }
}

/* --- win: window-handle helpers --------------------------------------- */

int rh_hwnd_parse(const char* s, HWND* out)
{
    const char*   p;
    unsigned long v;
    char*         end;

    if (s == NULL) {
        return 0;
    }
    if (strncmp(s, "win:", 4) != 0) {
        return 0;
    }
    p = s + 4;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }
    if (p[0] == '\0') {
        return 0;
    }
    v = strtoul(p, &end, 16);
    if (end == p || *end != '\0') {
        return 0;
    }
    *out = (HWND)(ULONG_PTR)v;
    return 1;
}

void rh_hwnd_format(HWND h, char* buf, int cap)
{
    _snprintf(buf, (size_t)cap, "win:0x%lX",
              (unsigned long)(ULONG_PTR)h);
    buf[cap - 1] = '\0';
}

/* --- error mapping ----------------------------------------------------- */

const char* rh_win32_code(DWORD err)
{
    switch (err) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_NAME:
        case ERROR_BAD_NETPATH:
            return "not_found";
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
            return "permission_denied";
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS:
            return "already_exists";
        case ERROR_DIR_NOT_EMPTY:
            return "not_empty";
        case ERROR_NOT_SAME_DEVICE:
            return "cross_device";
        case ERROR_DIRECTORY:
            return "not_a_directory";
        default:
            return "permission_denied";
    }
}

unsigned long rh_filetime_unix(const FILETIME* ft)
{
    ULARGE_INTEGER u;
    unsigned __int64 secs;

    u.LowPart  = ft->dwLowDateTime;
    u.HighPart = ft->dwHighDateTime;
    /* 100-ns ticks since 1601-01-01; 11644473600 s from 1601 to 1970. */
    if (u.QuadPart < 116444736000000000ui64) {
        return 0UL;
    }
    secs = (u.QuadPart - 116444736000000000ui64) / 10000000ui64;
    return (unsigned long)secs;
}
