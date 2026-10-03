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

#include <stdio.h>
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

/* Flag-name compare with '-' == '_' (see common.h). */
static int flag_eq(const char* tok, const char* name)
{
    char a;
    char b;

    for (;;) {
        a = *tok++;
        b = *name++;
        if (a == '_') a = '-';
        if (b == '_') b = '-';
        if (a != b) {
            return 0;
        }
        if (a == '\0') {
            return 1;
        }
    }
}

static int is_bool_word(const char* t)
{
    return (t != NULL &&
            (strcmp(t, "true") == 0 || strcmp(t, "false") == 0)) ? 1 : 0;
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
            if (flag_eq(tok, defs[k].name)) {
                matched = 1;
                out->seen[k] = 1;
                if (defs[k].has_val == RH_FLAG_BOOL) {
                    if (i + 1 < req->argc && is_bool_word(req->args[i + 1])) {
                        out->val[k] = req->args[i + 1];
                        i += 2;
                    } else {
                        i += 1;
                    }
                } else if (defs[k].has_val) {
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

int rh_arg_bool(const RhArgs* a, int idx, int dflt)
{
    if (!a->seen[idx]) {
        return dflt;
    }
    if (a->val[idx] != NULL && strcmp(a->val[idx], "false") == 0) {
        return 0;
    }
    return 1;
}

const char* rh_arg_named_or_pos(const RhArgs* a, int idx, int pos)
{
    if (idx >= 0 && a->seen[idx] && a->val[idx] != NULL) {
        return a->val[idx];
    }
    if (pos >= 0 && pos < a->npos) {
        return a->pos[pos];
    }
    return NULL;
}

int rh_parse_long(const char* s, long* out)
{
    char* end;
    long  v;

    if (s == NULL || s[0] == '\0') {
        return 0;
    }
    v = strtol(s, &end, 10);
    if (*end != '\0') {
        return 0;
    }
    *out = v;
    return 1;
}

static int lower_ch(int ch)
{
    return (ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch;
}

int rh_glob_match(const char* p, const char* s)
{
    const char* star_p = NULL;
    const char* star_s = NULL;

    while (*s != '\0') {
        if (*p == '*') {
            star_p = ++p;
            star_s = s;
        } else if (*p == '?' || (*p != '\0' &&
                   lower_ch((unsigned char)*p) ==
                   lower_ch((unsigned char)*s))) {
            ++p;
            ++s;
        } else if (star_p != NULL) {
            p = star_p;
            s = ++star_s;
        } else {
            return 0;
        }
    }
    while (*p == '*') {
        ++p;
    }
    return (*p == '\0') ? 1 : 0;
}

int rh_contains_ci(const char* h, const char* n)
{
    int i;

    if (n == NULL || n[0] == '\0') {
        return 1;
    }
    for (; *h != '\0'; ++h) {
        for (i = 0; n[i] != '\0' && h[i] != '\0' &&
                    lower_ch((unsigned char)h[i]) ==
                    lower_ch((unsigned char)n[i]); ++i) {
        }
        if (n[i] == '\0') {
            return 1;
        }
    }
    return 0;
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

/* --- filesystem helpers ------------------------------------------------ */

#ifndef FILE_ATTRIBUTE_REPARSE_POINT
#define FILE_ATTRIBUTE_REPARSE_POINT      0x00000400
#endif
#ifndef FILE_ATTRIBUTE_SPARSE_FILE
#define FILE_ATTRIBUTE_SPARSE_FILE        0x00000200
#endif
#ifndef FILE_ATTRIBUTE_ENCRYPTED
#define FILE_ATTRIBUTE_ENCRYPTED          0x00004000
#endif
#ifndef FILE_ATTRIBUTE_NOT_CONTENT_INDEXED
#define FILE_ATTRIBUTE_NOT_CONTENT_INDEXED 0x00002000
#endif
#ifndef FILE_ATTRIBUTE_OFFLINE
#define FILE_ATTRIBUTE_OFFLINE            0x00001000
#endif

const char* rh_attr_type(DWORD attr)
{
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT) {
        return "link";
    }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        return "directory";
    }
    return "file";
}

void rh_json_attr_flags(RhJson* j, DWORD attr)
{
    static const struct { DWORD bit; const char* name; } kFlags[] = {
        { FILE_ATTRIBUTE_READONLY,            "readonly"      },
        { FILE_ATTRIBUTE_HIDDEN,              "hidden"        },
        { FILE_ATTRIBUTE_SYSTEM,              "system"        },
        { FILE_ATTRIBUTE_ARCHIVE,             "archive"       },
        { FILE_ATTRIBUTE_TEMPORARY,           "temporary"     },
        { FILE_ATTRIBUTE_COMPRESSED,          "compressed"    },
        { FILE_ATTRIBUTE_ENCRYPTED,           "encrypted"     },
        { FILE_ATTRIBUTE_SPARSE_FILE,         "sparse_file"   },
        { FILE_ATTRIBUTE_REPARSE_POINT,       "reparse_point" },
        { FILE_ATTRIBUTE_OFFLINE,             "offline"       },
        { FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, "not_indexed"   }
    };
    int i;

    rh_json_begin_arr(j);
    for (i = 0; i < (int)(sizeof(kFlags) / sizeof(kFlags[0])); ++i) {
        if (attr & kFlags[i].bit) {
            rh_json_arr_str(j, kFlags[i].name);
        }
    }
    rh_json_end_arr(j);
}

void rh_json_find_stat(RhJson* j, const WIN32_FIND_DATAA* fd)
{
    char  num[32];
    int   is_dir = (fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
    double size;

    /* Byte size can exceed i32; emit it as a raw JSON number. Directories
     * report 0 per the spec. */
    size = is_dir ? 0.0
                  : (double)fd->nFileSizeHigh * 4294967296.0 +
                    (double)fd->nFileSizeLow;
    _snprintf(num, sizeof(num), "%.0f", size);
    num[sizeof(num) - 1] = '\0';

    rh_json_key(j, "type");  rh_json_str(j, rh_attr_type(fd->dwFileAttributes));
    rh_json_key(j, "size");  rh_json_raw(j, num);
    rh_json_key(j, "mtime_unix_s");
    rh_json_int(j, (i32)rh_filetime_unix(&fd->ftLastWriteTime));
    rh_json_key(j, "ctime_unix_s");
    rh_json_int(j, (i32)rh_filetime_unix(&fd->ftCreationTime));
    rh_json_key(j, "atime_unix_s");
    rh_json_int(j, (i32)rh_filetime_unix(&fd->ftLastAccessTime));
    rh_json_key(j, "flags");
    rh_json_attr_flags(j, fd->dwFileAttributes);
}

void rh_path_join(const char* dir, const char* name, char* out, int cap)
{
    int dl = (int)strlen(dir);
    if (dl > 0 && (dir[dl - 1] == '\\' || dir[dl - 1] == '/')) {
        _snprintf(out, (size_t)cap, "%s%s", dir, name);
    } else {
        _snprintf(out, (size_t)cap, "%s\\%s", dir, name);
    }
    out[cap - 1] = '\0';
}

int rh_is_dot(const char* n)
{
    return (n[0] == '.' && (n[1] == '\0' ||
            (n[1] == '.' && n[2] == '\0'))) ? 1 : 0;
}
