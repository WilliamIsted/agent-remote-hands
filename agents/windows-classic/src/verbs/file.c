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
#include <wininet.h>
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

/* --- file.create ------------------------------------------------------- */

/* Transcode UTF-8 input (n bytes) -> wide (UTF-16 LE host order) then ->
 * target single-byte code page. Returns a malloc'd buffer in *out (caller
 * frees) and length in *out_len, or 0 on failure. */
static int transcode_utf8_to_cp(const char* in, int n, UINT cp,
                                unsigned char** out, int* out_len)
{
    int      wlen;
    wchar_t* wbuf;
    int      blen;
    char*    bbuf;

    wlen = MultiByteToWideChar(CP_UTF8, 0, in, n, NULL, 0);
    if (wlen <= 0) {
        return 0;
    }
    wbuf = (wchar_t*)malloc((size_t)wlen * sizeof(wchar_t));
    if (wbuf == NULL) {
        return 0;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, in, n, wbuf, wlen) <= 0) {
        free(wbuf);
        return 0;
    }
    blen = WideCharToMultiByte(cp, 0, wbuf, wlen, NULL, 0, NULL, NULL);
    if (blen <= 0) {
        free(wbuf);
        return 0;
    }
    bbuf = (char*)malloc((size_t)blen);
    if (bbuf == NULL) {
        free(wbuf);
        return 0;
    }
    if (WideCharToMultiByte(cp, 0, wbuf, wlen, bbuf, blen, NULL, NULL) <= 0) {
        free(wbuf);
        free(bbuf);
        return 0;
    }
    free(wbuf);
    *out     = (unsigned char*)bbuf;
    *out_len = blen;
    return 1;
}

/* Transcode UTF-8 input (n bytes) -> UTF-16LE/BE byte stream (no BOM).
 * Returns a malloc'd buffer in *out (caller frees) and length in *out_len. */
static int transcode_utf8_to_utf16(const char* in, int n, int big_endian,
                                   unsigned char** out, int* out_len)
{
    int            wlen;
    wchar_t*       wbuf;
    unsigned char* bbuf;
    int            i;

    wlen = MultiByteToWideChar(CP_UTF8, 0, in, n, NULL, 0);
    if (wlen < 0) {
        return 0;
    }
    if (wlen == 0) {
        *out     = NULL;
        *out_len = 0;
        return 1;
    }
    wbuf = (wchar_t*)malloc((size_t)wlen * sizeof(wchar_t));
    if (wbuf == NULL) {
        return 0;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, in, n, wbuf, wlen) <= 0) {
        free(wbuf);
        return 0;
    }
    bbuf = (unsigned char*)malloc((size_t)wlen * 2);
    if (bbuf == NULL) {
        free(wbuf);
        return 0;
    }
    for (i = 0; i < wlen; ++i) {
        unsigned u = (unsigned short)wbuf[i];
        if (big_endian) {
            bbuf[i * 2]     = (unsigned char)((u >> 8) & 0xFF);
            bbuf[i * 2 + 1] = (unsigned char)(u & 0xFF);
        } else {
            bbuf[i * 2]     = (unsigned char)(u & 0xFF);
            bbuf[i * 2 + 1] = (unsigned char)((u >> 8) & 0xFF);
        }
    }
    free(wbuf);
    *out     = bbuf;
    *out_len = wlen * 2;
    return 1;
}

/* CREATE_NEW write: refuses if `path` already exists. Returns 1 / 0. */
static int rh_classic_write_new(const char* path,
                                const unsigned char* bytes, int len)
{
    HANDLE h;
    DWORD  written;
    BOOL   ok;

    h = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    if (len == 0) {
        CloseHandle(h);
        return 1;
    }
    written = 0;
    ok = WriteFile(h, bytes, (DWORD)len, &written, NULL);
    CloseHandle(h);
    return (ok && written == (DWORD)len) ? 1 : 0;
}

void rh_verb_file_create(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = {
        { "--encoding",  1 },
        { "--no-atomic", 0 }
    };
    RhArgs         a;
    int            n;
    char*          body;
    int            got;
    const char*    encoding;
    int            atomic;
    unsigned char* bytes;
    int            bytes_len;
    int            owned;
    DWORD          attr;
    char           tmp[MAX_PATH + 8];
    DWORD          err;
    RhJson*        j;
    const char*    r;

    rh_args_parse(req, defs, 2, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 2) {
        rh_err_msg(c, "invalid_args",
                   "file.create requires <path> <length>");
        return;
    }
    n = atoi(a.pos[1]);
    got = read_body(c, n, &body);
    if (got == -1) {
        rh_err_msg(c, "invalid_args", "bad payload length");
        return;
    }
    if (got == -2) {
        rh_err(c, "wire_desync");
        return;
    }

    encoding = a.val[0];
    if (encoding == NULL) {
        encoding = "utf-8";
    }
    atomic = a.seen[1] ? 0 : 1;

    /* Decode/transcode payload into `bytes` / `bytes_len`. `owned` tracks
     * whether `bytes` is a fresh allocation we must free, vs aliasing
     * `body`. */
    bytes     = NULL;
    bytes_len = 0;
    owned     = 0;
    if (strcmp(encoding, "utf-8") == 0 ||
        strcmp(encoding, "binary") == 0) {
        bytes     = (unsigned char*)body;
        bytes_len = got;
        owned     = 0;
    } else if (strcmp(encoding, "utf-16le") == 0 ||
               strcmp(encoding, "utf-16be") == 0) {
        int be = (encoding[6] == 'b') ? 1 : 0;
        if (!transcode_utf8_to_utf16(body, got, be, &bytes, &bytes_len)) {
            if (body != NULL) free(body);
            rh_err_msg(c, "invalid_args",
                       "file.create encoding transcode failed");
            return;
        }
        owned = 1;
    } else if (strcmp(encoding, "cp1252") == 0 ||
               strcmp(encoding, "ascii") == 0 ||
               strcmp(encoding, "latin-1") == 0) {
        UINT cp = (strcmp(encoding, "ascii") == 0)   ? 20127u
                 : (strcmp(encoding, "latin-1") == 0) ? 28591u
                                                      : 1252u;
        if (!transcode_utf8_to_cp(body, got, cp, &bytes, &bytes_len)) {
            if (body != NULL) free(body);
            rh_err_msg(c, "invalid_args",
                       "file.create encoding transcode failed");
            return;
        }
        owned = 1;
    } else {
        if (body != NULL) free(body);
        rh_err_msg(c, "invalid_args",
                   "file.create 'encoding' must be one of utf-8|utf-16le"
                   "|utf-16be|ascii|latin-1|cp1252|binary");
        return;
    }

    /* Refuse-if-exists probe. CREATE_NEW would also catch this, but the
     * explicit probe lets us return already_exists before touching the
     * temp file in the atomic path. */
    attr = GetFileAttributesA(a.pos[0]);
    if (attr != INVALID_FILE_ATTRIBUTES) {
        if (owned && bytes != NULL) free(bytes);
        if (body != NULL) free(body);
        rh_err_msg(c, "already_exists",
                   "file already exists; use file.write to overwrite");
        return;
    }

    if (atomic) {
        _snprintf(tmp, sizeof(tmp), "%s.rh-tmp", a.pos[0]);
        tmp[sizeof(tmp) - 1] = '\0';
        DeleteFileA(tmp);   /* best-effort cleanup */
        if (!rh_classic_write_new(tmp, bytes, bytes_len)) {
            err = GetLastError();
            DeleteFileA(tmp);
            if (owned && bytes != NULL) free(bytes);
            if (body != NULL) free(body);
            if (err == ERROR_PATH_NOT_FOUND) {
                rh_err_msg(c, "not_found",
                           "parent directory does not exist "
                           "(use directory.create first)");
            } else {
                rh_err(c, rh_win32_code(err));
            }
            return;
        }
        /* MoveFileA without MOVEFILE_REPLACE_EXISTING -- CREATE_NEW above
         * guarantees the target does not exist (and we re-probed before
         * the temp write). */
        if (!MoveFileA(tmp, a.pos[0])) {
            err = GetLastError();
            DeleteFileA(tmp);
            if (owned && bytes != NULL) free(bytes);
            if (body != NULL) free(body);
            rh_err(c, rh_win32_code(err));
            return;
        }
    } else {
        if (!rh_classic_write_new(a.pos[0], bytes, bytes_len)) {
            err = GetLastError();
            if (owned && bytes != NULL) free(bytes);
            if (body != NULL) free(body);
            if (err == ERROR_FILE_EXISTS) {
                rh_err_msg(c, "already_exists",
                           "file already exists; use file.write to overwrite");
            } else if (err == ERROR_PATH_NOT_FOUND) {
                rh_err_msg(c, "not_found",
                           "parent directory does not exist "
                           "(use directory.create first)");
            } else {
                rh_err(c, rh_win32_code(err));
            }
            return;
        }
    }

    if (owned && bytes != NULL) free(bytes);
    if (body != NULL) free(body);

    j = (RhJson*)malloc(sizeof(RhJson));
    if (j == NULL) {
        rh_err(c, "wire_desync");
        return;
    }
    rh_json_init(j);
    rh_json_begin_obj(j);
    rh_json_key(j, "bytes_written"); rh_json_int(j, (i32)bytes_len);
    rh_json_key(j, "encoding");      rh_json_str(j, encoding);
    rh_json_end_obj(j);
    r = rh_json_finish(j);
    if (r == NULL) {
        rh_err(c, "wire_desync");
    } else {
        rh_ok_json(c, r);
    }
    free(j);
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
