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

#include "file.h"
#include "common.h"
#include "encoding.h"
#include "../json.h"
#include "../protocol.h"

#include <windows.h>
#include <wininet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* These error sentinels arrived in the Win2000-era SDK; define defensively
 * for a barebones VC98 winbase.h. */
#ifndef INVALID_FILE_ATTRIBUTES
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#endif
#ifndef INVALID_FILE_SIZE
#define INVALID_FILE_SIZE ((DWORD)0xFFFFFFFF)
#endif
#ifndef INVALID_SET_FILE_POINTER
#define INVALID_SET_FILE_POINTER ((DWORD)-1)
#endif
#ifndef MOVEFILE_REPLACE_EXISTING
#define MOVEFILE_REPLACE_EXISTING 0x00000001
#endif

#define RH_FILE_MAX       (64 * 1024 * 1024)  /* payload / write cap        */
#define RH_FILE_READ_MAX  (16 * 1024 * 1024)  /* single file.read response  */

/* --- shared helpers ---------------------------------------------------- */

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

static int has_wildcard(const char* p)
{
    return (strchr(p, '*') != NULL || strchr(p, '?') != NULL) ? 1 : 0;
}

/* Stat one path (no wildcards). FindFirstFile fails on drive roots, so
 * fall back to GetFileAttributes there. Returns 0 when absent. */
static int stat_path(const char* path, WIN32_FIND_DATAA* fd)
{
    HANDLE h;
    DWORD  attr;

    h = FindFirstFileA(path, fd);
    if (h != INVALID_HANDLE_VALUE) {
        FindClose(h);
        return 1;
    }
    attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        return 0;
    }
    memset(fd, 0, sizeof(*fd));
    fd->dwFileAttributes = attr;
    return 1;
}

/* Read the length-prefixed payload: 0 bytes -> *out NULL. Returns n, -1 for
 * a bad length, -2 on wire failure. */
static int read_body(RhConn* c, int n, char** out)
{
    char* buf;
    int   rc;

    *out = NULL;
    if (n < 0 || n > RH_FILE_MAX) {
        return -1;
    }
    if (n == 0) {
        return 0;
    }
    buf = (char*)malloc((size_t)n);
    if (buf == NULL) {
        return -2;
    }
    rc = rh_read_payload(&c->reader, buf, n);
    if (rc != RH_PROTO_OK) {
        free(buf);
        return -2;
    }
    *out = buf;
    return n;
}

/* Content for file.write / write_at / create. v2.1 carries it either as a
 * `--content <text>` header arg or as a trailing <length> positional plus
 * payload. The payload is ALWAYS consumed first (when a length is present)
 * so an error reply never leaves body bytes on the wire.
 *
 * On success returns 1 with the on-disk bytes in *bytes (malloc'd or NULL)
 * / *nbytes. On failure it has already replied and returns 0. Payload
 * content under `binary` is taken as raw bytes (the payload is binary-
 * clean); header-arg content under `binary` is base64. */
static int get_content(RhConn* c, const RhArgs* a, int content_idx,
                       int len_pos, int enc, char** bytes, int* nbytes)
{
    const char* text;
    char*       body;
    int         got;
    long        n;

    *bytes  = NULL;
    *nbytes = 0;

    if (len_pos < a->npos) {
        if (!rh_parse_long(a->pos[len_pos], &n)) {
            rh_err_msg(c, "invalid_args", "bad payload length");
            return 0;
        }
        got = read_body(c, (int)n, &body);
        if (got == -1) {
            rh_err_msg(c, "invalid_args", "bad payload length");
            return 0;
        }
        if (got == -2) {
            rh_err(c, "wire_desync");
            return 0;
        }
        if (a->seen[content_idx]) {
            if (body != NULL) free(body);
            rh_err_msg(c, "invalid_args",
                       "give content as --content or a payload, not both");
            return 0;
        }
        if (enc == RH_ENC_BINARY) {
            *bytes  = body;
            *nbytes = got;
            return 1;
        }
        if (!rh_enc_encode(enc, body, got, bytes, nbytes)) {
            if (body != NULL) free(body);
            rh_err_msg(c, "invalid_args",
                       "content not representable in the requested encoding");
            return 0;
        }
        if (body != NULL) free(body);
        return 1;
    }

    text = a->val[content_idx];
    if (text == NULL) {
        rh_err_msg(c, "invalid_args",
                   "content required (--content or <length> + payload)");
        return 0;
    }
    if (!rh_enc_encode(enc, text, (int)strlen(text), bytes, nbytes)) {
        rh_err_msg(c, "invalid_args",
                   (enc == RH_ENC_BINARY)
                   ? "content is not valid base64"
                   : "content not representable in the requested encoding");
        return 0;
    }
    return 1;
}

/* Parse --encoding; replies invalid_args and returns -1 when unknown. */
static int get_encoding(RhConn* c, const char* name)
{
    int enc = rh_enc_parse(name);
    if (enc < 0) {
        rh_err_msg(c, "invalid_args",
                   "encoding must be one of utf-8|utf-16le|utf-16be|ascii"
                   "|latin-1|cp1252|binary");
    }
    return enc;
}

/* Write `len` bytes to a new file (CREATE_NEW). Returns 1 / 0 (GetLastError
 * is preserved on failure). */
static int write_new(const char* path, const char* bytes, int len)
{
    HANDLE h;
    DWORD  written;
    BOOL   ok;
    DWORD  e;

    h = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    ok = TRUE;
    written = 0;
    if (len > 0) {
        ok = WriteFile(h, bytes, (DWORD)len, &written, NULL);
    }
    e = GetLastError();
    CloseHandle(h);
    if (!ok || written != (DWORD)len) {
        DeleteFileA(path);
        SetLastError(e);
        return 0;
    }
    return 1;
}

/* Replace `dst` with `src`. MoveFileEx is resolved at runtime: Win9x
 * kernel32 lacks a working one, so fall back to CopyFile + DeleteFile
 * (not atomic -- documented in the spec's copy_delete_fallback). */
typedef BOOL (WINAPI *MoveFileExA_fn)(LPCSTR, LPCSTR, DWORD);

static int replace_file(const char* src, const char* dst)
{
    static MoveFileExA_fn move_ex = NULL;
    static int            probed  = 0;
    DWORD                 e;

    if (!probed) {
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        if (k32 != NULL) {
            move_ex = (MoveFileExA_fn)GetProcAddress(k32, "MoveFileExA");
        }
        probed = 1;
    }
    if (move_ex != NULL) {
        if (move_ex(src, dst, MOVEFILE_REPLACE_EXISTING)) {
            return 1;
        }
        e = GetLastError();
        if (e != ERROR_CALL_NOT_IMPLEMENTED) {
            return 0;
        }
    }
    if (!CopyFileA(src, dst, FALSE)) {
        return 0;
    }
    DeleteFileA(src);
    return 1;
}

static void temp_sibling(const char* path, char* out, int cap)
{
    _snprintf(out, (size_t)cap, "%s.rh-tmp.%lu", path,
              (unsigned long)GetTickCount());
    out[cap - 1] = '\0';
}

/* Append a JSON-escaped copy of s[0..n) (already valid UTF-8). */
static char* json_escape_into(char* o, const char* s, int n)
{
    static const char kHex[] = "0123456789abcdef";
    int i;

    for (i = 0; i < n; ++i) {
        unsigned char ch = (unsigned char)s[i];
        switch (ch) {
            case '"':  *o++ = '\\'; *o++ = '"';  break;
            case '\\': *o++ = '\\'; *o++ = '\\'; break;
            case '\n': *o++ = '\\'; *o++ = 'n';  break;
            case '\r': *o++ = '\\'; *o++ = 'r';  break;
            case '\t': *o++ = '\\'; *o++ = 't';  break;
            default:
                if (ch < 0x20) {
                    *o++ = '\\'; *o++ = 'u'; *o++ = '0'; *o++ = '0';
                    *o++ = kHex[ch >> 4];
                    *o++ = kHex[ch & 15];
                } else {
                    *o++ = (char)ch;
                }
                break;
        }
    }
    return o;
}

/* --- file.read --------------------------------------------------------- */

void rh_verb_file_read(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = {
        { "--encoding", RH_FLAG_VALUE },
        { "--offset",   RH_FLAG_VALUE },
        { "--length",   RH_FLAG_VALUE },
        { "--path",     RH_FLAG_VALUE }
    };
    RhArgs      a;
    const char* path;
    int         enc;
    long        offset;
    long        length;
    HANDLE      fh;
    DWORD       size;
    DWORD       want;
    DWORD       got;
    int         truncated;
    char*       buf;
    char*       text;
    int         text_len;
    char*       resp;
    char*       o;

    rh_args_parse(req, defs, 4, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    path = rh_arg_named_or_pos(&a, 3, 0);
    if (path == NULL || a.missing_val) {
        rh_err_msg(c, "invalid_args", "file.read requires <path>");
        return;
    }
    enc = get_encoding(c, a.val[0]);
    if (enc < 0) {
        return;
    }
    offset = 0;
    length = -1;
    if ((a.val[1] != NULL && (!rh_parse_long(a.val[1], &offset) ||
                              offset < 0)) ||
        (a.val[2] != NULL && (!rh_parse_long(a.val[2], &length) ||
                              length < 0))) {
        rh_err_msg(c, "invalid_args",
                   "offset / length must be non-negative integers");
        return;
    }

    fh = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                     NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) {
        rh_err(c, rh_win32_code(GetLastError()));
        return;
    }
    size = GetFileSize(fh, NULL);
    if (size == INVALID_FILE_SIZE) {
        CloseHandle(fh);
        rh_err(c, "permission_denied");
        return;
    }
    want = ((DWORD)offset >= size) ? 0 : size - (DWORD)offset;
    truncated = 0;
    if (length >= 0 && (DWORD)length < want) {
        want = (DWORD)length;
        truncated = 1;
    }
    if (want > RH_FILE_READ_MAX) {
        CloseHandle(fh);
        rh_err_msg(c, "invalid_args",
                   "file too large for one read; page with "
                   "--offset / --length");
        return;
    }
    buf = (char*)malloc((size_t)want + 1);
    if (buf == NULL) {
        CloseHandle(fh);
        rh_err(c, "wire_desync");
        return;
    }
    got = 0;
    if (want > 0) {
        if (SetFilePointer(fh, offset, NULL, FILE_BEGIN) ==
                INVALID_SET_FILE_POINTER ||
            !ReadFile(fh, buf, want, &got, NULL)) {
            free(buf);
            CloseHandle(fh);
            rh_err(c, rh_win32_code(GetLastError()));
            return;
        }
    }
    CloseHandle(fh);

    if (!rh_enc_decode(enc, buf, (int)got, &text, &text_len)) {
        free(buf);
        rh_err(c, "wire_desync");
        return;
    }
    free(buf);

    /* Hand-built: the content can exceed RhJson's 64 KB cap. */
    resp = (char*)malloc((size_t)text_len * 6 + 96);
    if (resp == NULL) {
        free(text);
        rh_err(c, "wire_desync");
        return;
    }
    o = resp;
    memcpy(o, "{\"content\":\"", 12);
    o += 12;
    o = json_escape_into(o, text, text_len);
    o += sprintf(o, "\",\"bytes_read\":%lu,\"truncated\":%s}",
                 (unsigned long)((enc == RH_ENC_BINARY) ? (int)got
                                                        : text_len),
                 truncated ? "true" : "false");
    *o = '\0';
    free(text);
    rh_ok_json(c, resp);
    free(resp);
}

/* --- file.write -------------------------------------------------------- */

static void send_write_result(RhConn* c, int written, int enc,
                              int with_size, DWORD new_size)
{
    RhJson* j = jopen(c);
    if (j == NULL) {
        return;
    }
    rh_json_begin_obj(j);
    rh_json_key(j, "bytes_written"); rh_json_int(j, (i32)written);
    rh_json_key(j, "encoding");      rh_json_str(j, rh_enc_name(enc));
    if (with_size) {
        rh_json_key(j, "new_size");  rh_json_int(j, (i32)new_size);
    }
    rh_json_end_obj(j);
    jsend(c, j);
}

void rh_verb_file_write(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = {
        { "--content",  RH_FLAG_VALUE },
        { "--encoding", RH_FLAG_VALUE },
        { "--atomic",   RH_FLAG_BOOL  },
        { "--path",     RH_FLAG_VALUE }
    };
    RhArgs      a;
    const char* path;
    int         enc;
    char*       bytes;
    int         nbytes;
    DWORD       attr;
    HANDLE      fh;
    DWORD       wrote;
    DWORD       e;
    char        tmp[MAX_PATH + 32];

    rh_args_parse(req, defs, 4, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    path = rh_arg_named_or_pos(&a, 3, 0);
    if (path == NULL || a.missing_val) {
        rh_err_msg(c, "invalid_args", "file.write requires <path>");
        return;
    }
    enc = rh_enc_parse(a.val[1]);
    if (!get_content(c, &a, 0, a.seen[3] ? 0 : 1,
                     (enc < 0) ? RH_ENC_UTF8 : enc, &bytes, &nbytes)) {
        return;
    }
    if (enc < 0) {
        if (bytes != NULL) free(bytes);
        get_encoding(c, a.val[1]);
        return;
    }

    /* U-only: the target must already exist (file.create makes new). */
    attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        if (bytes != NULL) free(bytes);
        rh_err_msg(c, "not_found",
                   "file does not exist; use file.create to make it");
        return;
    }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        if (bytes != NULL) free(bytes);
        rh_err_msg(c, "invalid_args", "path is a directory");
        return;
    }

    if (rh_arg_bool(&a, 2, 1)) {
        temp_sibling(path, tmp, (int)sizeof(tmp));
        if (!write_new(tmp, bytes, nbytes) || !replace_file(tmp, path)) {
            e = GetLastError();
            DeleteFileA(tmp);
            if (bytes != NULL) free(bytes);
            rh_err(c, rh_win32_code(e));
            return;
        }
    } else {
        fh = CreateFileA(path, GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (fh == INVALID_HANDLE_VALUE) {
            e = GetLastError();
            if (bytes != NULL) free(bytes);
            rh_err(c, rh_win32_code(e));
            return;
        }
        if (nbytes > 0 &&
            !WriteFile(fh, bytes, (DWORD)nbytes, &wrote, NULL)) {
            e = GetLastError();
            CloseHandle(fh);
            free(bytes);
            rh_err(c, rh_win32_code(e));
            return;
        }
        CloseHandle(fh);
    }
    if (bytes != NULL) free(bytes);
    send_write_result(c, nbytes, enc, 0, 0);
}

/* --- file.write_at ----------------------------------------------------- */

void rh_verb_file_write_at(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = {
        { "--content",  RH_FLAG_VALUE },
        { "--encoding", RH_FLAG_VALUE },
        { "--truncate", RH_FLAG_BOOL  },
        { "--path",     RH_FLAG_VALUE },
        { "--offset",   RH_FLAG_VALUE }
    };
    RhArgs      a;
    const char* path;
    const char* off_s;
    int         next_pos;
    long        offset;
    int         enc;
    int         truncate;
    char*       bytes;
    int         nbytes;
    HANDLE      fh;
    DWORD       wrote;
    DWORD       e;
    DWORD       new_size;

    rh_args_parse(req, defs, 5, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    /* Positional grammar: <path> <offset> [<length>]; either of the first
     * two may instead come as --path / --offset. */
    next_pos = 0;
    path = a.seen[3] ? a.val[3] : (next_pos < a.npos ? a.pos[next_pos++]
                                                     : NULL);
    off_s = a.seen[4] ? a.val[4] : (next_pos < a.npos ? a.pos[next_pos++]
                                                      : NULL);
    if (path == NULL || off_s == NULL || a.missing_val) {
        rh_err_msg(c, "invalid_args",
                   "file.write_at requires <path> <offset>");
        return;
    }
    enc = rh_enc_parse(a.val[1]);
    if (!get_content(c, &a, 0, next_pos,
                     (enc < 0) ? RH_ENC_UTF8 : enc, &bytes, &nbytes)) {
        return;
    }
    if (enc < 0) {
        if (bytes != NULL) free(bytes);
        get_encoding(c, a.val[1]);
        return;
    }
    if (!rh_parse_long(off_s, &offset) || offset < 0) {
        if (bytes != NULL) free(bytes);
        rh_err_msg(c, "invalid_args",
                   "offset must be a non-negative integer (32-bit on "
                   "classic)");
        return;
    }
    truncate = rh_arg_bool(&a, 2, 0);
    if (truncate && offset != 0) {
        if (bytes != NULL) free(bytes);
        rh_err_msg(c, "invalid_args", "truncate requires offset 0");
        return;
    }

    fh = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) {
        e = GetLastError();
        if (bytes != NULL) free(bytes);
        rh_err(c, rh_win32_code(e));
        return;
    }
    if (SetFilePointer(fh, offset, NULL, FILE_BEGIN) ==
            INVALID_SET_FILE_POINTER ||
        (truncate && !SetEndOfFile(fh)) ||
        (nbytes > 0 && !WriteFile(fh, bytes, (DWORD)nbytes, &wrote, NULL))) {
        e = GetLastError();
        CloseHandle(fh);
        if (bytes != NULL) free(bytes);
        rh_err(c, rh_win32_code(e));
        return;
    }
    new_size = GetFileSize(fh, NULL);
    CloseHandle(fh);
    if (bytes != NULL) free(bytes);
    send_write_result(c, nbytes, enc, 1, new_size);
}

/* --- file.stat / exists ------------------------------------------------ */

void rh_verb_file_stat(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = { { "--path", RH_FLAG_VALUE } };
    RhArgs           a;
    const char*      path;
    WIN32_FIND_DATAA fd;
    RhJson*          j;

    rh_args_parse(req, defs, 1, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    path = rh_arg_named_or_pos(&a, 0, 0);
    if (path == NULL) {
        rh_err_msg(c, "invalid_args", "file.stat requires <path>");
        return;
    }
    if (has_wildcard(path)) {
        rh_err_msg(c, "invalid_args", "wildcards not allowed in path");
        return;
    }
    if (!stat_path(path, &fd)) {
        rh_err(c, "not_found");
        return;
    }
    j = jopen(c);
    if (j == NULL) {
        return;
    }
    rh_json_begin_obj(j);
    rh_json_find_stat(j, &fd);
    rh_json_end_obj(j);
    jsend(c, j);
}

void rh_verb_file_exists(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = { { "--path", RH_FLAG_VALUE } };
    RhArgs      a;
    const char* path;
    DWORD       attr;
    RhJson*     j;

    rh_args_parse(req, defs, 1, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    path = rh_arg_named_or_pos(&a, 0, 0);
    if (path == NULL) {
        rh_err_msg(c, "invalid_args", "file.exists requires <path>");
        return;
    }
    attr = has_wildcard(path) ? INVALID_FILE_ATTRIBUTES
                              : GetFileAttributesA(path);
    j = jopen(c);
    if (j == NULL) {
        return;
    }
    rh_json_begin_obj(j);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        rh_json_key(j, "exists"); rh_json_bool(j, RH_FALSE);
        rh_json_key(j, "type");   rh_json_str(j, "absent");
        rh_json_key(j, "flags");  rh_json_attr_flags(j, 0);
    } else {
        rh_json_key(j, "exists"); rh_json_bool(j, RH_TRUE);
        rh_json_key(j, "type");   rh_json_str(j, rh_attr_type(attr));
        rh_json_key(j, "flags");  rh_json_attr_flags(j, attr);
    }
    rh_json_end_obj(j);
    jsend(c, j);
}

/* --- file.delete / rename / wait --------------------------------------- */

void rh_verb_file_delete(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = { { "--path", RH_FLAG_VALUE } };
    RhArgs      a;
    const char* path;
    DWORD       attr;

    rh_args_parse(req, defs, 1, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    path = rh_arg_named_or_pos(&a, 0, 0);
    if (path == NULL) {
        rh_err_msg(c, "invalid_args", "file.delete requires <path>");
        return;
    }
    attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        rh_err(c, "not_found");
        return;
    }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        rh_err_msg(c, "invalid_args",
                   "path is a directory; use directory.delete");
        return;
    }
    if (!DeleteFileA(path)) {
        rh_err(c, rh_win32_code(GetLastError()));
        return;
    }
    rh_ok(c);
}

void rh_verb_file_rename(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = {
        { "--overwrite", RH_FLAG_BOOL  },
        { "--cross-fs",  RH_FLAG_BOOL  },
        { "--src",       RH_FLAG_VALUE },
        { "--dst",       RH_FLAG_VALUE }
    };
    RhArgs      a;
    const char* src;
    const char* dst;
    const char* fallback;
    DWORD       attr;
    DWORD       e;
    RhJson*     j;

    rh_args_parse(req, defs, 4, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    src = rh_arg_named_or_pos(&a, 2, 0);
    dst = rh_arg_named_or_pos(&a, 3, a.seen[2] ? 0 : 1);
    if (src == NULL || dst == NULL) {
        rh_err_msg(c, "invalid_args", "file.rename requires <src> <dst>");
        return;
    }
    attr = GetFileAttributesA(src);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        rh_err(c, "not_found");
        return;
    }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        rh_err_msg(c, "invalid_args",
                   "src is a directory; use directory.rename");
        return;
    }
    if (GetFileAttributesA(dst) != INVALID_FILE_ATTRIBUTES &&
        !rh_arg_bool(&a, 0, 0)) {
        rh_err(c, "already_exists");
        return;
    }

    fallback = "none";
    if (rh_arg_bool(&a, 0, 0) ? !replace_file(src, dst)
                              : !MoveFileA(src, dst)) {
        e = GetLastError();
        if (e != ERROR_NOT_SAME_DEVICE) {
            rh_err(c, rh_win32_code(e));
            return;
        }
        if (!rh_arg_bool(&a, 1, 0)) {
            rh_err_msg(c, "cross_device",
                       "cross-filesystem move needs --cross-fs");
            return;
        }
        if (!CopyFileA(src, dst, rh_arg_bool(&a, 0, 0) ? FALSE : TRUE)) {
            rh_err(c, rh_win32_code(GetLastError()));
            return;
        }
        DeleteFileA(src);
        fallback = "copy_delete";
    }

    j = jopen(c);
    if (j == NULL) {
        return;
    }
    rh_json_begin_obj(j);
    rh_json_key(j, "renamed");       rh_json_bool(j, RH_TRUE);
    rh_json_key(j, "fallback_used"); rh_json_str(j, fallback);
    rh_json_end_obj(j);
    jsend(c, j);
}

void rh_verb_file_wait(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = {
        { "--timeout-ms", RH_FLAG_VALUE },
        { "--glob",       RH_FLAG_VALUE }
    };
    RhArgs           a;
    const char*      glob;
    const char*      to_s;
    long             timeout_ms;
    DWORD            start;
    WIN32_FIND_DATAA fd;
    HANDLE           h;
    char             dir[MAX_PATH];
    char             full[MAX_PATH];
    char*            slash;
    RhJson*          j;

    rh_args_parse(req, defs, 2, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    glob = rh_arg_named_or_pos(&a, 1, 0);
    /* v2.0 grammar took the timeout positionally; still accepted. */
    to_s = rh_arg_named_or_pos(&a, 0, a.seen[1] ? 0 : 1);
    if (glob == NULL || a.missing_val) {
        rh_err_msg(c, "invalid_args", "file.wait requires <glob>");
        return;
    }
    timeout_ms = 30000;
    if (to_s != NULL && (!rh_parse_long(to_s, &timeout_ms) ||
                         timeout_ms < 0)) {
        rh_err_msg(c, "invalid_args", "timeout_ms must be >= 0");
        return;
    }

    /* Directory part of the glob, for building the matched full path. */
    _snprintf(dir, sizeof(dir), "%s", glob);
    dir[sizeof(dir) - 1] = '\0';
    slash = strrchr(dir, '\\');
    if (slash == NULL) {
        slash = strrchr(dir, '/');
    }
    if (slash != NULL) {
        slash[1] = '\0';
    } else {
        dir[0] = '\0';
    }

    start = GetTickCount();
    for (;;) {
        h = FindFirstFileA(glob, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!rh_is_dot(fd.cFileName)) {
                    break;
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
            if (!rh_is_dot(fd.cFileName)) {
                _snprintf(full, sizeof(full), "%s%s", dir, fd.cFileName);
                full[sizeof(full) - 1] = '\0';
                j = jopen(c);
                if (j == NULL) {
                    return;
                }
                rh_json_begin_obj(j);
                rh_json_key(j, "path"); rh_json_str(j, full);
                rh_json_key(j, "type");
                rh_json_str(j, rh_attr_type(fd.dwFileAttributes));
                rh_json_end_obj(j);
                jsend(c, j);
                return;
            }
        }
        if ((long)(GetTickCount() - start) >= timeout_ms) {
            rh_err_kv(c, "timeout", "glob", glob);
            return;
        }
        Sleep(100);
    }
}

/* --- file.create ------------------------------------------------------- */

void rh_verb_file_create(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = {
        { "--content",  RH_FLAG_VALUE },
        { "--encoding", RH_FLAG_VALUE },
        { "--atomic",   RH_FLAG_BOOL  },
        { "--path",     RH_FLAG_VALUE }
    };
    RhArgs      a;
    const char* path;
    int         enc;
    char*       bytes;
    int         nbytes;
    char        tmp[MAX_PATH + 32];
    DWORD       err;

    rh_args_parse(req, defs, 4, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    path = rh_arg_named_or_pos(&a, 3, 0);
    if (path == NULL || a.missing_val) {
        rh_err_msg(c, "invalid_args", "file.create requires <path>");
        return;
    }
    enc = rh_enc_parse(a.val[1]);
    if (!get_content(c, &a, 0, a.seen[3] ? 0 : 1,
                     (enc < 0) ? RH_ENC_UTF8 : enc, &bytes, &nbytes)) {
        return;
    }
    if (enc < 0) {
        if (bytes != NULL) free(bytes);
        get_encoding(c, a.val[1]);
        return;
    }

    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
        if (bytes != NULL) free(bytes);
        rh_err_msg(c, "already_exists",
                   "file already exists; use file.write to overwrite");
        return;
    }

    if (rh_arg_bool(&a, 2, 1)) {
        /* Temp + MoveFile (no replace): the rename fails if the target
         * appeared meanwhile, giving already_exists. */
        temp_sibling(path, tmp, (int)sizeof(tmp));
        if (!write_new(tmp, bytes, nbytes) || !MoveFileA(tmp, path)) {
            err = GetLastError();
            DeleteFileA(tmp);
            if (bytes != NULL) free(bytes);
            rh_err(c, rh_win32_code(err));
            return;
        }
    } else if (!write_new(path, bytes, nbytes)) {
        err = GetLastError();
        if (bytes != NULL) free(bytes);
        rh_err(c, rh_win32_code(err));
        return;
    }
    if (bytes != NULL) free(bytes);
    send_write_result(c, nbytes, enc, 0, 0);
}

/* --- file.download ----------------------------------------------------- */

/* WinINet streaming download. Maps HTTP 4xx/5xx to permission_denied with
 * a {"http_status":N} detail (the spec only declares permission_denied for
 * upstream failures; the status code lives in the detail). */
void rh_verb_file_download(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = {
        { "--timeout-ms",    1 },
        { "--no-verify-tls", 0 }
    };
    RhArgs        a;
    int           timeout_ms;
    int           verify_tls;
    HINTERNET     hSession;
    HINTERNET     hReq;
    DWORD         flags;
    DWORD         http_status;
    DWORD         sz;
    char          content_type[256];
    HANDLE        hFile;
    DWORD         err;
    unsigned char buf[8192];
    DWORD         bytes_read;
    unsigned long total;
    DWORD         wrote;
    BOOL          read_ok;
    RhJson*       j;
    const char*   r;

    rh_args_parse(req, defs, 2, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 2) {
        rh_err_msg(c, "invalid_args",
                   "file.download requires <url> <path>");
        return;
    }
    timeout_ms = (a.val[0] != NULL) ? atoi(a.val[0]) : 30000;
    if (timeout_ms <= 0) {
        timeout_ms = 30000;
    }
    verify_tls = a.seen[1] ? 0 : 1;

    hSession = InternetOpenA("ARH-Classic/0.3",
                             INTERNET_OPEN_TYPE_PRECONFIG,
                             NULL, NULL, 0);
    if (hSession == NULL) {
        rh_err_msg(c, "permission_denied", "InternetOpen failed");
        return;
    }
    InternetSetOptionA(hSession, INTERNET_OPTION_CONNECT_TIMEOUT,
                       &timeout_ms, sizeof(timeout_ms));
    InternetSetOptionA(hSession, INTERNET_OPTION_RECEIVE_TIMEOUT,
                       &timeout_ms, sizeof(timeout_ms));
    InternetSetOptionA(hSession, INTERNET_OPTION_SEND_TIMEOUT,
                       &timeout_ms, sizeof(timeout_ms));

    flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE;
    if (!verify_tls) {
        flags |= INTERNET_FLAG_IGNORE_CERT_CN_INVALID |
                 INTERNET_FLAG_IGNORE_CERT_DATE_INVALID;
    }
    hReq = InternetOpenUrlA(hSession, a.pos[0], NULL, 0, flags, 0);
    if (hReq == NULL) {
        err = GetLastError();
        InternetCloseHandle(hSession);
        if (err == ERROR_INTERNET_TIMEOUT) {
            rh_err_kv(c, "timeout", "deadline",
                      (a.val[0] != NULL) ? a.val[0] : "30000");
        } else {
            char buf2[32];
            _snprintf(buf2, sizeof(buf2), "%lu", (unsigned long)err);
            buf2[sizeof(buf2) - 1] = '\0';
            rh_err_kv(c, "permission_denied", "wininet_error", buf2);
        }
        return;
    }

    http_status = 0;
    sz = sizeof(http_status);
    HttpQueryInfoA(hReq,
                   HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                   &http_status, &sz, NULL);

    content_type[0] = '\0';
    sz = sizeof(content_type) - 1;
    if (HttpQueryInfoA(hReq, HTTP_QUERY_CONTENT_TYPE,
                       content_type, &sz, NULL)) {
        content_type[sz] = '\0';
    } else {
        content_type[0] = '\0';
    }

    if (http_status >= 400) {
        char buf2[16];
        InternetCloseHandle(hReq);
        InternetCloseHandle(hSession);
        _snprintf(buf2, sizeof(buf2), "%lu", (unsigned long)http_status);
        buf2[sizeof(buf2) - 1] = '\0';
        rh_err_kv(c, "permission_denied", "http_status", buf2);
        return;
    }

    /* CREATE_ALWAYS so a partial prior download is overwritten; spec
     * doesn't require refuse-if-exists for download. */
    hFile = CreateFileA(a.pos[1], GENERIC_WRITE, 0, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        err = GetLastError();
        InternetCloseHandle(hReq);
        InternetCloseHandle(hSession);
        if (err == ERROR_PATH_NOT_FOUND) {
            rh_err_msg(c, "not_found",
                       "parent directory does not exist");
        } else {
            rh_err(c, rh_win32_code(err));
        }
        return;
    }

    total = 0;
    for (;;) {
        bytes_read = 0;
        read_ok = InternetReadFile(hReq, buf, sizeof(buf), &bytes_read);
        if (!read_ok) {
            err = GetLastError();
            CloseHandle(hFile);
            DeleteFileA(a.pos[1]);
            InternetCloseHandle(hReq);
            InternetCloseHandle(hSession);
            if (err == ERROR_INTERNET_TIMEOUT) {
                rh_err_kv(c, "timeout", "deadline",
                          (a.val[0] != NULL) ? a.val[0] : "30000");
            } else {
                rh_err_kv(c, "permission_denied", "reason", "read_failed");
            }
            return;
        }
        if (bytes_read == 0) {
            break;   /* end of stream */
        }
        wrote = 0;
        if (!WriteFile(hFile, buf, bytes_read, &wrote, NULL) ||
            wrote != bytes_read) {
            CloseHandle(hFile);
            DeleteFileA(a.pos[1]);
            InternetCloseHandle(hReq);
            InternetCloseHandle(hSession);
            rh_err_kv(c, "permission_denied", "reason", "write_failed");
            return;
        }
        total += wrote;
    }

    CloseHandle(hFile);
    InternetCloseHandle(hReq);
    InternetCloseHandle(hSession);

    j = (RhJson*)malloc(sizeof(RhJson));
    if (j == NULL) {
        rh_err(c, "wire_desync");
        return;
    }
    rh_json_init(j);
    rh_json_begin_obj(j);
    /* Defensive clamp at INT_MAX (~2 GB) to avoid sign-flip on cast (R2
     * review). Classic targets (FAT32 / NTFS on NT 4-2000) can theoretically
     * exceed 2 GB; until the JSON emitter grows a 64-bit helper, clamp. */
    if (total > 0x7FFFFFFFul) {
        total = 0x7FFFFFFFul;
    }
    rh_json_key(j, "bytes_written"); rh_json_int(j, (i32)total);
    rh_json_key(j, "content_type");  rh_json_str(j, content_type);
    rh_json_key(j, "http_status");   rh_json_int(j, (i32)http_status);
    rh_json_end_obj(j);
    r = rh_json_finish(j);
    if (r == NULL) {
        rh_err(c, "wire_desync");
    } else {
        rh_ok_json(c, r);
    }
    free(j);
}
