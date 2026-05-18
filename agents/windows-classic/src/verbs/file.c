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
#include "../json.h"
#include "../protocol.h"

#include <windows.h>
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

#define RH_FILE_MAX (64 * 1024 * 1024)

/* --- file.read --------------------------------------------------------- */

void rh_verb_file_read(RhConn* c, const RhRequest* req)
{
    RhArgs a;
    HANDLE fh;
    DWORD  size;
    DWORD  got;
    char*  buf;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 1) {
        rh_err_msg(c, "invalid_args", "file.read requires <path>");
        return;
    }
    fh = CreateFileA(a.pos[0], GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
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
    if (size == 0) {
        CloseHandle(fh);
        rh_ok(c);
        return;
    }
    if (size > RH_FILE_MAX) {
        CloseHandle(fh);
        rh_err_msg(c, "permission_denied", "file too large for classic read");
        return;
    }
    buf = (char*)malloc((size_t)size);
    if (buf == NULL) {
        CloseHandle(fh);
        rh_err(c, "wire_desync");
        return;
    }
    if (!ReadFile(fh, buf, size, &got, NULL)) {
        free(buf);
        CloseHandle(fh);
        rh_err(c, rh_win32_code(GetLastError()));
        return;
    }
    CloseHandle(fh);
    rh_ok_bytes(c, buf, (int)got);
    free(buf);
}

/* --- file.write -------------------------------------------------------- */

static int read_body(RhConn* c, int n, char** out)
{
    char* buf;
    int   rc;

    if (n < 0 || n > RH_FILE_MAX) {
        return -1;
    }
    if (n == 0) {
        *out = NULL;
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

void rh_verb_file_write(RhConn* c, const RhRequest* req)
{
    RhArgs a;
    int    n;
    char*  body;
    int    got;
    HANDLE fh;
    DWORD  wrote;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 2) {
        rh_err_msg(c, "invalid_args",
                   "file.write requires <path> <length>");
        return;
    }
    n   = atoi(a.pos[1]);
    got = read_body(c, n, &body);
    if (got == -1) {
        rh_err_msg(c, "invalid_args", "bad payload length");
        return;
    }
    if (got == -2) {
        rh_err(c, "wire_desync");
        return;
    }
    fh = CreateFileA(a.pos[0], GENERIC_WRITE, 0, NULL,
                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) {
        if (body != NULL) free(body);
        rh_err(c, rh_win32_code(GetLastError()));
        return;
    }
    if (got > 0) {
        if (!WriteFile(fh, body, (DWORD)got, &wrote, NULL)) {
            free(body);
            CloseHandle(fh);
            rh_err(c, rh_win32_code(GetLastError()));
            return;
        }
        free(body);
    }
    CloseHandle(fh);
    rh_ok(c);
}

void rh_verb_file_write_at(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = { { "--truncate", 0 } };
    RhArgs a;
    long   offset;
    int    n;
    char*  body;
    int    got;
    HANDLE fh;
    DWORD  wrote;
    DWORD  create;

    rh_args_parse(req, defs, 1, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 3) {
        rh_err_msg(c, "invalid_args",
                   "file.write_at requires <path> <offset> <length>");
        return;
    }
    offset = atol(a.pos[1]);
    n      = atoi(a.pos[2]);
    got    = read_body(c, n, &body);
    if (got == -1) {
        rh_err_msg(c, "invalid_args", "bad payload length");
        return;
    }
    if (got == -2) {
        rh_err(c, "wire_desync");
        return;
    }
    create = (a.seen[0] && offset == 0) ? CREATE_ALWAYS : OPEN_ALWAYS;
    fh = CreateFileA(a.pos[0], GENERIC_WRITE, 0, NULL,
                     create, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) {
        if (body != NULL) free(body);
        rh_err(c, rh_win32_code(GetLastError()));
        return;
    }
    SetFilePointer(fh, offset, NULL, FILE_BEGIN);
    if (got > 0) {
        if (!WriteFile(fh, body, (DWORD)got, &wrote, NULL)) {
            free(body);
            CloseHandle(fh);
            rh_err(c, rh_win32_code(GetLastError()));
            return;
        }
        free(body);
    }
    CloseHandle(fh);
    rh_ok(c);
}

/* --- file.stat / exists ------------------------------------------------ */

void rh_verb_file_stat(RhConn* c, const RhRequest* req)
{
    RhArgs           a;
    WIN32_FIND_DATAA fd;
    HANDLE           h;
    RhJson*          j;
    const char*      r;
    int              is_dir;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 1) {
        rh_err_msg(c, "invalid_args", "file.stat requires <path>");
        return;
    }
    h = FindFirstFileA(a.pos[0], &fd);
    if (h == INVALID_HANDLE_VALUE) {
        rh_err(c, "not_found");
        return;
    }
    FindClose(h);
    is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;

    j = (RhJson*)malloc(sizeof(RhJson));
    if (j == NULL) {
        rh_err(c, "wire_desync");
        return;
    }
    rh_json_init(j);
    rh_json_begin_obj(j);
    rh_json_key(j, "name");       rh_json_str(j, fd.cFileName);
    rh_json_key(j, "type");       rh_json_str(j, is_dir ? "dir" : "file");
    rh_json_key(j, "size");       rh_json_int(j, (i32)fd.nFileSizeLow);
    rh_json_key(j, "mtime_unix");
    rh_json_int(j, (i32)rh_filetime_unix(&fd.ftLastWriteTime));
    rh_json_end_obj(j);
    r = rh_json_finish(j);
    if (r == NULL) {
        rh_err(c, "wire_desync");
    } else {
        rh_ok_json(c, r);
    }
    free(j);
}

void rh_verb_file_exists(RhConn* c, const RhRequest* req)
{
    RhArgs      a;
    DWORD       attr;
    RhJson*     j;
    const char* r;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 1) {
        rh_err_msg(c, "invalid_args", "file.exists requires <path>");
        return;
    }
    j = (RhJson*)malloc(sizeof(RhJson));
    if (j == NULL) {
        rh_err(c, "wire_desync");
        return;
    }
    rh_json_init(j);
    rh_json_begin_obj(j);
    attr = GetFileAttributesA(a.pos[0]);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        rh_json_key(j, "exists"); rh_json_bool(j, RH_FALSE);
    } else {
        rh_json_key(j, "exists"); rh_json_bool(j, RH_TRUE);
        rh_json_key(j, "type");
        rh_json_str(j, (attr & FILE_ATTRIBUTE_DIRECTORY) ? "dir" : "file");
    }
    rh_json_end_obj(j);
    r = rh_json_finish(j);
    if (r == NULL) {
        rh_err(c, "wire_desync");
    } else {
        rh_ok_json(c, r);
    }
    free(j);
}

/* --- file.delete / rename / wait --------------------------------------- */

void rh_verb_file_delete(RhConn* c, const RhRequest* req)
{
    RhArgs a;
    DWORD  attr;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 1) {
        rh_err_msg(c, "invalid_args", "file.delete requires <path>");
        return;
    }
    attr = GetFileAttributesA(a.pos[0]);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        rh_err(c, "not_found");
        return;
    }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        if (!RemoveDirectoryA(a.pos[0])) {
            DWORD e = GetLastError();
            rh_err(c, (e == ERROR_DIR_NOT_EMPTY) ? "not_empty"
                                                 : rh_win32_code(e));
            return;
        }
    } else if (!DeleteFileA(a.pos[0])) {
        rh_err(c, rh_win32_code(GetLastError()));
        return;
    }
    rh_ok(c);
}

void rh_verb_file_rename(RhConn* c, const RhRequest* req)
{
    RhArgs a;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 2) {
        rh_err_msg(c, "invalid_args", "file.rename requires <src> <dst>");
        return;
    }
    if (!MoveFileA(a.pos[0], a.pos[1])) {
        rh_err(c, rh_win32_code(GetLastError()));
        return;
    }
    rh_ok(c);
}

void rh_verb_file_wait(RhConn* c, const RhRequest* req)
{
    RhArgs      a;
    int         timeout_ms;
    int         waited;
    RhJson*     j;
    const char* r;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 2) {
        rh_err_msg(c, "invalid_args",
                   "file.wait requires <pattern> <timeout-ms>");
        return;
    }
    timeout_ms = atoi(a.pos[1]);
    waited = 0;
    for (;;) {
        if (GetFileAttributesA(a.pos[0]) != INVALID_FILE_ATTRIBUTES) {
            j = (RhJson*)malloc(sizeof(RhJson));
            if (j == NULL) {
                rh_err(c, "wire_desync");
                return;
            }
            rh_json_init(j);
            rh_json_begin_obj(j);
            rh_json_key(j, "path");
            rh_json_str(j, a.pos[0]);
            rh_json_end_obj(j);
            r = rh_json_finish(j);
            if (r == NULL) {
                rh_err(c, "wire_desync");
            } else {
                rh_ok_json(c, r);
            }
            free(j);
            return;
        }
        if (waited >= timeout_ms) {
            rh_err_kv(c, "timeout", "deadline", a.pos[1]);
            return;
        }
        Sleep(100);
        waited += 100;
    }
}
