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

/* Heap JSON doc (RhJson is 64 KB) or NULL after replying wire_desync. */
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

/* Finish, send and free a document built with jopen(). */
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

/* Look up `path` as a directory. Replies not_found / not_a_directory and
 * returns 0 on failure. */
static int require_dir(RhConn* c, const char* path, DWORD* attr_out)
{
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        rh_err(c, "not_found");
        return 0;
    }
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        rh_err_msg(c, "not_a_directory", "path is a file");
        return 0;
    }
    if (attr_out != NULL) {
        *attr_out = attr;
    }
    return 1;
}

/* --- directory.list ---------------------------------------------------- */

typedef struct {
    RhJson*     j;
    const char* pattern;   /* NULL = all                                   */
    int         recursive;
    long        limit;     /* < 0 = unlimited                              */
    long        emitted;
} ListWalk;

/* Emit entries of `dir`; `rel` is the walk-root-relative prefix using '/'
 * separators ("" at the root). Returns 0 once `limit` is reached. */
static int list_walk(ListWalk* w, const char* dir, const char* rel)
{
    char             pat[MAX_PATH + 4];
    char             child[MAX_PATH];
    char             name[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE           h;
    int              more;

    rh_path_join(dir, "*", pat, (int)sizeof(pat));
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return 1;
    }
    more = 1;
    do {
        if (rh_is_dot(fd.cFileName)) {
            continue;
        }
        if (rel[0] != '\0') {
            _snprintf(name, sizeof(name), "%s/%s", rel, fd.cFileName);
        } else {
            _snprintf(name, sizeof(name), "%s", fd.cFileName);
        }
        name[sizeof(name) - 1] = '\0';

        if (w->pattern == NULL || rh_glob_match(w->pattern, fd.cFileName)) {
            if (w->limit >= 0 && w->emitted >= w->limit) {
                more = 0;
                break;
            }
            rh_json_arr_elem(w->j);
            rh_json_begin_obj(w->j);
            rh_json_key(w->j, "name");
            rh_json_str(w->j, name);
            rh_json_find_stat(w->j, &fd);
            rh_json_end_obj(w->j);
            w->emitted += 1;
        }

        /* Recurse into real subdirectories (not junctions: avoids loops). */
        if (w->recursive &&
            (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !(fd.dwFileAttributes & 0x400 /* REPARSE_POINT */)) {
            rh_path_join(dir, fd.cFileName, child, (int)sizeof(child));
            if (!list_walk(w, child, name)) {
                more = 0;
                break;
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return more;
}

void rh_verb_directory_list(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = {
        { "--recursive", RH_FLAG_BOOL  },
        { "--pattern",   RH_FLAG_VALUE },
        { "--limit",     RH_FLAG_VALUE },
        { "--path",      RH_FLAG_VALUE }
    };
    RhArgs      a;
    ListWalk    w;
    const char* path;

    rh_args_parse(req, defs, 4, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    path = rh_arg_named_or_pos(&a, 3, 0);
    if (path == NULL || a.missing_val) {
        rh_err_msg(c, "invalid_args", "directory.list requires <path>");
        return;
    }
    w.limit = -1;
    if (a.val[2] != NULL &&
        (!rh_parse_long(a.val[2], &w.limit) || w.limit < 0)) {
        rh_err_msg(c, "invalid_args", "limit must be a non-negative integer");
        return;
    }
    if (!require_dir(c, path, NULL)) {
        return;
    }

    w.j = jopen(c);
    if (w.j == NULL) {
        return;
    }
    w.pattern   = a.val[1];
    w.recursive = rh_arg_bool(&a, 0, 0);
    w.emitted   = 0;

    rh_json_begin_obj(w.j);
    rh_json_key(w.j, "entries");
    rh_json_begin_arr(w.j);
    list_walk(&w, path, "");
    rh_json_end_arr(w.j);
    rh_json_end_obj(w.j);
    jsend(c, w.j);
}

/* --- directory.stat ---------------------------------------------------- */

void rh_verb_directory_stat(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = { { "--path", RH_FLAG_VALUE } };
    RhArgs           a;
    const char*      path;
    DWORD            attr;
    char             pat[MAX_PATH + 4];
    WIN32_FIND_DATAA fd;
    HANDLE           h;
    int              count;
    unsigned long    mtime;
    RhJson*          j;

    rh_args_parse(req, defs, 1, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    path = rh_arg_named_or_pos(&a, 0, 0);
    if (path == NULL) {
        rh_err_msg(c, "invalid_args", "directory.stat requires <path>");
        return;
    }
    if (!require_dir(c, path, &attr)) {
        return;
    }

    /* Directory's own mtime (FindFirstFile on a drive root fails; 0). */
    mtime = 0;
    h = FindFirstFileA(path, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        mtime = rh_filetime_unix(&fd.ftLastWriteTime);
        FindClose(h);
    }

    count = 0;
    rh_path_join(path, "*", pat, (int)sizeof(pat));
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!rh_is_dot(fd.cFileName)) {
                ++count;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    j = jopen(c);
    if (j == NULL) {
        return;
    }
    rh_json_begin_obj(j);
    rh_json_key(j, "type");         rh_json_str(j, "directory");
    rh_json_key(j, "entry_count");  rh_json_int(j, (i32)count);
    rh_json_key(j, "mtime_unix_s"); rh_json_int(j, (i32)mtime);
    rh_json_key(j, "flags");        rh_json_attr_flags(j, attr);
    rh_json_end_obj(j);
    jsend(c, j);
}

/* --- directory.exists -------------------------------------------------- */

void rh_verb_directory_exists(RhConn* c, const RhRequest* req)
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
        rh_err_msg(c, "invalid_args", "directory.exists requires <path>");
        return;
    }
    attr = GetFileAttributesA(path);

    j = jopen(c);
    if (j == NULL) {
        return;
    }
    rh_json_begin_obj(j);
    rh_json_key(j, "exists");
    rh_json_bool(j, (attr != INVALID_FILE_ATTRIBUTES &&
                     (attr & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0);
    rh_json_end_obj(j);
    jsend(c, j);
}

/* --- directory.create -------------------------------------------------- */

static int make_parents(const char* path)
{
    char buf[MAX_PATH];
    int  i;
    int  n;

    n = (int)strlen(path);
    if (n >= (int)sizeof(buf)) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return 0;
    }
    for (i = 0; i <= n; ++i) {
        char ch = path[i];
        if (ch == '\\' || ch == '/' || ch == '\0') {
            if (i > 0 && !(i == 2 && path[1] == ':')) {
                buf[i] = '\0';
                if (GetFileAttributesA(buf) == INVALID_FILE_ATTRIBUTES &&
                    !CreateDirectoryA(buf, NULL)) {
                    return 0;
                }
            }
        }
        buf[i] = ch;
    }
    return 1;
}

void rh_verb_directory_create(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = {
        { "--parents", RH_FLAG_BOOL  },
        { "--mode",    RH_FLAG_VALUE },   /* accepted, ignored (ACLs) */
        { "--path",    RH_FLAG_VALUE }
    };
    RhArgs      a;
    const char* path;
    DWORD       e;
    RhJson*     j;

    rh_args_parse(req, defs, 3, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    path = rh_arg_named_or_pos(&a, 2, 0);
    if (path == NULL || a.missing_val) {
        rh_err_msg(c, "invalid_args", "directory.create requires <path>");
        return;
    }
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
        rh_err(c, "already_exists");
        return;
    }
    if (rh_arg_bool(&a, 0, 0)) {
        if (!make_parents(path)) {
            rh_err(c, rh_win32_code(GetLastError()));
            return;
        }
    } else if (!CreateDirectoryA(path, NULL)) {
        e = GetLastError();
        rh_err(c, (e == ERROR_ALREADY_EXISTS) ? "already_exists"
                                              : rh_win32_code(e));
        return;
    }

    j = jopen(c);
    if (j == NULL) {
        return;
    }
    rh_json_begin_obj(j);
    rh_json_key(j, "created"); rh_json_bool(j, RH_TRUE);
    rh_json_end_obj(j);
    jsend(c, j);
}

/* --- directory.rename -------------------------------------------------- */

/* Recursive copy of a directory tree (cross-filesystem rename fallback). */
static int copy_tree(const char* src, const char* dst)
{
    char             pat[MAX_PATH + 4];
    char             s[MAX_PATH];
    char             d[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE           h;
    int              ok;

    if (!CreateDirectoryA(dst, NULL)) {
        return 0;
    }
    ok = 1;
    rh_path_join(src, "*", pat, (int)sizeof(pat));
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (rh_is_dot(fd.cFileName)) {
                continue;
            }
            rh_path_join(src, fd.cFileName, s, (int)sizeof(s));
            rh_path_join(dst, fd.cFileName, d, (int)sizeof(d));
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                ok = copy_tree(s, d);
            } else {
                ok = CopyFileA(s, d, TRUE) ? 1 : 0;
            }
        } while (ok && FindNextFileA(h, &fd));
        FindClose(h);
    }
    return ok;
}

static int remove_tree(const char* dir);

static void send_renamed(RhConn* c, const char* fallback)
{
    RhJson* j = jopen(c);
    if (j == NULL) {
        return;
    }
    rh_json_begin_obj(j);
    rh_json_key(j, "renamed");       rh_json_bool(j, RH_TRUE);
    rh_json_key(j, "fallback_used"); rh_json_str(j, fallback);
    rh_json_end_obj(j);
    jsend(c, j);
}

void rh_verb_directory_rename(RhConn* c, const RhRequest* req)
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
    DWORD       e;
    DWORD       dattr;

    rh_args_parse(req, defs, 4, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    src = rh_arg_named_or_pos(&a, 2, 0);
    dst = rh_arg_named_or_pos(&a, 3, a.seen[2] ? 0 : 1);
    if (src == NULL || dst == NULL) {
        rh_err_msg(c, "invalid_args",
                   "directory.rename requires <src> <dst>");
        return;
    }
    if (!require_dir(c, src, NULL)) {
        return;
    }
    dattr = GetFileAttributesA(dst);
    if (dattr != INVALID_FILE_ATTRIBUTES) {
        if (!rh_arg_bool(&a, 0, 0)) {
            rh_err(c, "already_exists");
            return;
        }
        if (!(dattr & FILE_ATTRIBUTE_DIRECTORY) || remove_tree(dst) < 0) {
            rh_err(c, rh_win32_code(GetLastError()));
            return;
        }
    }
    if (MoveFileA(src, dst)) {
        send_renamed(c, "none");
        return;
    }
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
    if (!copy_tree(src, dst)) {
        e = GetLastError();
        remove_tree(dst);
        rh_err(c, rh_win32_code(e));
        return;
    }
    if (remove_tree(src) < 0) {
        rh_err(c, rh_win32_code(GetLastError()));
        return;
    }
    send_renamed(c, "copy_delete");
}

/* --- directory.delete (v2.0 alias: directory.remove) ------------------- */

/* Recursively delete the contents of `dir` then the directory itself.
 * Returns the number of filesystem entries removed (files + subdirs,
 * including `dir`), or -1 on failure. */
static int remove_tree(const char* dir)
{
    char             pat[MAX_PATH + 4];
    char             child[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE           h;
    int              removed;

    removed = 0;
    rh_path_join(dir, "*", pat, (int)sizeof(pat));
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (rh_is_dot(fd.cFileName)) {
                continue;
            }
            rh_path_join(dir, fd.cFileName, child, (int)sizeof(child));
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                !(fd.dwFileAttributes & 0x400 /* REPARSE_POINT */)) {
                int sub = remove_tree(child);
                if (sub < 0) {
                    FindClose(h);
                    return -1;
                }
                removed += sub;
            } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                /* Junction: remove the link, never its target. */
                if (!RemoveDirectoryA(child)) {
                    FindClose(h);
                    return -1;
                }
                ++removed;
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
    static const RhFlagDef defs[] = {
        { "--recursive", RH_FLAG_BOOL  },
        { "--path",      RH_FLAG_VALUE }
    };
    RhArgs      a;
    const char* path;
    RhJson*     j;
    int         entries;
    DWORD       e;

    rh_args_parse(req, defs, 2, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    path = rh_arg_named_or_pos(&a, 1, 0);
    if (path == NULL) {
        rh_err_msg(c, "invalid_args", "directory.delete requires <path>");
        return;
    }
    if (!require_dir(c, path, NULL)) {
        return;
    }

    if (rh_arg_bool(&a, 0, 0)) {
        entries = remove_tree(path);
        if (entries < 0) {
            rh_err(c, rh_win32_code(GetLastError()));
            return;
        }
        /* remove_tree counts the directory itself; report contents. */
        if (entries > 0) {
            --entries;
        }
    } else {
        if (!RemoveDirectoryA(path)) {
            e = GetLastError();
            rh_err(c, (e == ERROR_DIR_NOT_EMPTY) ? "not_empty"
                                                 : rh_win32_code(e));
            return;
        }
        entries = 0;
    }

    j = jopen(c);
    if (j == NULL) {
        return;
    }
    rh_json_begin_obj(j);
    rh_json_key(j, "entries_removed"); rh_json_int(j, (i32)entries);
    rh_json_end_obj(j);
    jsend(c, j);
}
