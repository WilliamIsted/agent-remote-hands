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

#include "directory.h"
#include "common.h"
#include "../json.h"

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#ifndef INVALID_FILE_ATTRIBUTES
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#endif

/* Join dir + "\\" + name into out (cap bytes). Tolerates a trailing
 * backslash already on `dir`. */
static void path_join(const char* dir, const char* name,
                      char* out, int cap)
{
    int dl = (int)strlen(dir);
    if (dl > 0 && (dir[dl - 1] == '\\' || dir[dl - 1] == '/')) {
        _snprintf(out, (size_t)cap, "%s%s", dir, name);
    } else {
        _snprintf(out, (size_t)cap, "%s\\%s", dir, name);
    }
    out[cap - 1] = '\0';
}

static int is_dot(const char* n)
{
    return (n[0] == '.' && (n[1] == '\0' ||
            (n[1] == '.' && n[2] == '\0'))) ? 1 : 0;
}

/* --- directory.list ---------------------------------------------------- */

void rh_verb_directory_list(RhConn* c, const RhRequest* req)
{
    RhArgs           a;
    char             pat[MAX_PATH + 4];
    WIN32_FIND_DATAA fd;
    HANDLE           h;
    RhJson*          j;
    const char*      r;
    DWORD            attr;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 1) {
        rh_err_msg(c, "invalid_args", "directory.list requires <path>");
        return;
    }
    attr = GetFileAttributesA(a.pos[0]);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        rh_err(c, "not_found");
        return;
    }
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        rh_err_msg(c, "not_a_directory", "path is a file");
        return;
    }

    path_join(a.pos[0], "*", pat, (int)sizeof(pat));

    j = (RhJson*)malloc(sizeof(RhJson));
    if (j == NULL) {
        rh_err(c, "wire_desync");
        return;
    }
    rh_json_init(j);
    rh_json_begin_obj(j);
    rh_json_key(j, "entries");
    rh_json_begin_arr(j);

    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            int is_dir;
            if (is_dot(fd.cFileName)) {
                continue;
            }
            is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                     ? 1 : 0;
            rh_json_arr_elem(j);
            rh_json_begin_obj(j);
            rh_json_key(j, "name"); rh_json_str(j, fd.cFileName);
            rh_json_key(j, "type");
            rh_json_str(j, is_dir ? "dir" : "file");
            rh_json_key(j, "size");
            rh_json_int(j, (i32)fd.nFileSizeLow);
            rh_json_key(j, "mtime_unix");
            rh_json_int(j, (i32)rh_filetime_unix(&fd.ftLastWriteTime));
            rh_json_end_obj(j);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

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

/* --- directory.stat ---------------------------------------------------- */

void rh_verb_directory_stat(RhConn* c, const RhRequest* req)
{
    RhArgs           a;
    DWORD            attr;
    char             pat[MAX_PATH + 4];
    WIN32_FIND_DATAA fd;
    HANDLE           h;
    int              count;
    unsigned long    mtime;
    RhJson*          j;
    const char*      r;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 1) {
        rh_err_msg(c, "invalid_args", "directory.stat requires <path>");
        return;
    }
    attr = GetFileAttributesA(a.pos[0]);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        rh_err(c, "not_found");
        return;
    }
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        rh_err_msg(c, "not_a_directory", "path is a file");
        return;
    }

    /* Directory's own mtime. */
    mtime = 0;
    h = FindFirstFileA(a.pos[0], &fd);
    if (h != INVALID_HANDLE_VALUE) {
        mtime = rh_filetime_unix(&fd.ftLastWriteTime);
        FindClose(h);
    }

    /* Entry count. */
    count = 0;
    path_join(a.pos[0], "*", pat, (int)sizeof(pat));
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!is_dot(fd.cFileName)) {
                ++count;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    j = (RhJson*)malloc(sizeof(RhJson));
    if (j == NULL) {
        rh_err(c, "wire_desync");
        return;
    }
    rh_json_init(j);
    rh_json_begin_obj(j);
    rh_json_key(j, "type");        rh_json_str(j, "dir");
    rh_json_key(j, "entry_count"); rh_json_int(j, (i32)count);
    rh_json_key(j, "mtime_unix");  rh_json_int(j, (i32)mtime);
    rh_json_end_obj(j);
    r = rh_json_finish(j);
    if (r == NULL) {
        rh_err(c, "wire_desync");
    } else {
        rh_ok_json(c, r);
    }
    free(j);
}

/* --- directory.exists -------------------------------------------------- */

void rh_verb_directory_exists(RhConn* c, const RhRequest* req)
{
    RhArgs      a;
    DWORD       attr;
    int         exists;
    RhJson*     j;
    const char* r;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 1) {
        rh_err_msg(c, "invalid_args", "directory.exists requires <path>");
        return;
    }
    attr   = GetFileAttributesA(a.pos[0]);
    exists = (attr != INVALID_FILE_ATTRIBUTES &&
              (attr & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0;

    j = (RhJson*)malloc(sizeof(RhJson));
    if (j == NULL) {
        rh_err(c, "wire_desync");
        return;
    }
    rh_json_init(j);
    rh_json_begin_obj(j);
    rh_json_key(j, "exists");
    rh_json_bool(j, exists);
    rh_json_end_obj(j);
    r = rh_json_finish(j);
    if (r == NULL) {
        rh_err(c, "wire_desync");
    } else {
        rh_ok_json(c, r);
    }
    free(j);
}

/* --- directory.create -------------------------------------------------- */

static int make_parents(const char* path)
{
    char buf[MAX_PATH];
    int  i;
    int  n;

    n = (int)strlen(path);
    if (n >= (int)sizeof(buf)) {
        return 0;
    }
    for (i = 0; i <= n; ++i) {
        char ch = path[i];
        if (ch == '\\' || ch == '/' || ch == '\0') {
            if (i > 0 && !(i == 2 && path[1] == ':')) {
                buf[i] = '\0';
                if (GetFileAttributesA(buf) == INVALID_FILE_ATTRIBUTES) {
                    CreateDirectoryA(buf, NULL);
                }
            }
        }
        buf[i] = ch;
    }
    return (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) ? 1 : 0;
}

void rh_verb_directory_create(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = { { "--parents", 0 } };
    RhArgs a;

    rh_args_parse(req, defs, 1, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 1) {
        rh_err_msg(c, "invalid_args", "directory.create requires <path>");
        return;
    }
    if (a.seen[0]) {
        if (!make_parents(a.pos[0])) {
            rh_err(c, rh_win32_code(GetLastError()));
            return;
        }
        rh_ok(c);
        return;
    }
    if (!CreateDirectoryA(a.pos[0], NULL)) {
        DWORD e = GetLastError();
        rh_err(c, (e == ERROR_ALREADY_EXISTS) ? "already_exists"
                                              : rh_win32_code(e));
        return;
    }
    rh_ok(c);
}

/* --- directory.rename -------------------------------------------------- */

void rh_verb_directory_rename(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = {
        { "--overwrite", 0 },
        { "--cross-fs",  0 }
    };
    RhArgs      a;
    RhJson*     j;
    const char* r;

    rh_args_parse(req, defs, 2, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 2) {
        rh_err_msg(c, "invalid_args",
                   "directory.rename requires <src> <dst>");
        return;
    }
    if (!MoveFileA(a.pos[0], a.pos[1])) {
        DWORD e = GetLastError();
        if (e == ERROR_NOT_SAME_DEVICE && !a.seen[1]) {
            rh_err_msg(c, "cross_device",
                       "cross-filesystem move needs --cross-fs");
            return;
        }
        rh_err(c, rh_win32_code(e));
        return;
    }
    j = (RhJson*)malloc(sizeof(RhJson));
    if (j == NULL) {
        rh_err(c, "wire_desync");
        return;
    }
    rh_json_init(j);
    rh_json_begin_obj(j);
    rh_json_key(j, "renamed");
    rh_json_bool(j, RH_TRUE);
    rh_json_end_obj(j);
    r = rh_json_finish(j);
    if (r == NULL) {
        rh_err(c, "wire_desync");
    } else {
        rh_ok_json(c, r);
    }
    free(j);
}

/* --- directory.remove -------------------------------------------------- */

/* Recursively delete the contents of `dir` then the directory itself.
 * Returns the number of filesystem entries removed (files + subdirs),
 * or -1 on failure. */
static int remove_tree(const char* dir)
{
    char             pat[MAX_PATH + 4];
    char             child[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE           h;
    int              removed;

    removed = 0;
    path_join(dir, "*", pat, (int)sizeof(pat));
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (is_dot(fd.cFileName)) {
                continue;
            }
            path_join(dir, fd.cFileName, child, (int)sizeof(child));
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                int sub = remove_tree(child);
                if (sub < 0) {
                    FindClose(h);
                    return -1;
                }
                removed += sub;
            } else {
                SetFileAttributesA(child, FILE_ATTRIBUTE_NORMAL);
                if (!DeleteFileA(child)) {
                    FindClose(h);
                    return -1;
                }
                ++removed;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (!RemoveDirectoryA(dir)) {
        return -1;
    }
    ++removed;   /* the directory itself */
    return removed;
}

void rh_verb_directory_remove(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = { { "--recursive", 0 } };
    RhArgs      a;
    DWORD       attr;
    RhJson*     j;
    const char* r;
    int         entries;

    rh_args_parse(req, defs, 1, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 1) {
        rh_err_msg(c, "invalid_args", "directory.remove requires <path>");
        return;
    }
    attr = GetFileAttributesA(a.pos[0]);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        rh_err(c, "not_found");
        return;
    }
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        rh_err_msg(c, "not_a_directory", "path is a file");
        return;
    }

    if (a.seen[0]) {
        entries = remove_tree(a.pos[0]);
        if (entries < 0) {
            rh_err(c, rh_win32_code(GetLastError()));
            return;
        }
        /* entries counts the directory itself; report contained entries. */
        if (entries > 0) {
            --entries;
        }
    } else {
        if (!RemoveDirectoryA(a.pos[0])) {
            DWORD e = GetLastError();
            rh_err(c, (e == ERROR_DIR_NOT_EMPTY) ? "not_empty"
                                                 : rh_win32_code(e));
            return;
        }
        entries = 0;
    }

    j = (RhJson*)malloc(sizeof(RhJson));
    if (j == NULL) {
        rh_err(c, "wire_desync");
        return;
    }
    rh_json_init(j);
    rh_json_begin_obj(j);
    rh_json_key(j, "removed");         rh_json_bool(j, RH_TRUE);
    rh_json_key(j, "entries_removed"); rh_json_int(j, (i32)entries);
    rh_json_end_obj(j);
    r = rh_json_finish(j);
    if (r == NULL) {
        rh_err(c, "wire_desync");
    } else {
        rh_ok_json(c, r);
    }
    free(j);
}
