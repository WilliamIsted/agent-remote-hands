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

#include "registry.h"
#include "common.h"
#include "../json.h"

#include <windows.h>
#include <stdlib.h>
#include <string.h>

/* REG_QWORD arrived in the Win2000-era SDK; guard for a barebones VC98
 * winnt.h if the Platform SDK include path is absent. */
#ifndef REG_QWORD
#define REG_QWORD 11
#endif

/* Split "HKLM\Sub\Key" into a root HKEY + subkey pointer. Returns 1 on a
 * recognised root, 0 otherwise (-> ERR invalid_args). `subkey` points into
 * the original string (may be ""). */
static int split_root(const char* path, HKEY* root, const char** subkey)
{
    const char* bs;
    int         n;
    char        head[32];

    bs = strchr(path, '\\');
    if (bs != NULL) {
        n = (int)(bs - path);
    } else {
        n = (int)strlen(path);
    }
    if (n <= 0 || n >= (int)sizeof(head)) {
        return 0;
    }
    memcpy(head, path, (size_t)n);
    head[n] = '\0';

    if (strcmp(head, "HKLM") == 0 ||
        strcmp(head, "HKEY_LOCAL_MACHINE") == 0) {
        *root = HKEY_LOCAL_MACHINE;
    } else if (strcmp(head, "HKCU") == 0 ||
               strcmp(head, "HKEY_CURRENT_USER") == 0) {
        *root = HKEY_CURRENT_USER;
    } else if (strcmp(head, "HKCR") == 0 ||
               strcmp(head, "HKEY_CLASSES_ROOT") == 0) {
        *root = HKEY_CLASSES_ROOT;
    } else if (strcmp(head, "HKU") == 0 ||
               strcmp(head, "HKEY_USERS") == 0) {
        *root = HKEY_USERS;
    } else if (strcmp(head, "HKCC") == 0 ||
               strcmp(head, "HKEY_CURRENT_CONFIG") == 0) {
        *root = HKEY_CURRENT_CONFIG;
    } else {
        return 0;
    }
    *subkey = (bs != NULL) ? (bs + 1) : "";
    return 1;
}

static const char* reg_type_name(DWORD t)
{
    switch (t) {
        case REG_SZ:                 return "REG_SZ";
        case REG_EXPAND_SZ:          return "REG_EXPAND_SZ";
        case REG_DWORD:              return "REG_DWORD";
        case REG_BINARY:             return "REG_BINARY";
        case REG_MULTI_SZ:           return "REG_MULTI_SZ";
        case REG_QWORD:              return "REG_QWORD";
        default:                     return "REG_NONE";
    }
}

/* --- registry.read ----------------------------------------------------- */

void rh_verb_registry_read(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = { { "--value", 1 } };
    RhArgs      a;
    HKEY        root;
    const char* subkey;
    HKEY        key;
    LONG        rc;
    RhJson*     j;
    const char* r;

    rh_args_parse(req, defs, 1, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 1) {
        rh_err_msg(c, "invalid_args", "registry.read requires <path>");
        return;
    }
    if (!split_root(a.pos[0], &root, &subkey)) {
        rh_err_msg(c, "invalid_args", "unknown registry root");
        return;
    }
    rc = RegOpenKeyExA(root, subkey, 0, KEY_READ, &key);
    if (rc != ERROR_SUCCESS) {
        rh_err(c, "not_found");
        return;
    }

    j = (RhJson*)malloc(sizeof(RhJson));
    if (j == NULL) {
        RegCloseKey(key);
        rh_err(c, "wire_desync");
        return;
    }
    rh_json_init(j);

    if (a.val[0] != NULL) {
        /* Single value. */
        DWORD type;
        BYTE  data[4096];
        DWORD dlen = (DWORD)sizeof(data);
        rc = RegQueryValueExA(key, a.val[0], NULL, &type, data, &dlen);
        if (rc != ERROR_SUCCESS) {
            free(j);
            RegCloseKey(key);
            rh_err(c, "not_found");
            return;
        }
        rh_json_begin_obj(j);
        rh_json_key(j, "type");
        rh_json_str(j, reg_type_name(type));
        rh_json_key(j, "data");
        if (type == REG_DWORD && dlen == 4) {
            char num[16];
            _snprintf(num, sizeof(num), "%lu",
                      (unsigned long)(*(DWORD*)data));
            num[sizeof(num) - 1] = '\0';
            rh_json_str(j, num);
        } else if (type == REG_SZ || type == REG_EXPAND_SZ) {
            data[sizeof(data) - 1] = 0;
            rh_json_str(j, (const char*)data);
        } else {
            rh_json_str(j, "");
        }
        rh_json_end_obj(j);
    } else {
        /* Whole key: values + subkeys. */
        DWORD i;
        char  name[512];
        DWORD nlen;
        DWORD type;
        BYTE  data[2048];
        DWORD dlen;

        rh_json_begin_obj(j);
        rh_json_key(j, "values");
        rh_json_begin_obj(j);
        i = 0;
        for (;;) {
            nlen = (DWORD)sizeof(name);
            dlen = (DWORD)sizeof(data);
            rc = RegEnumValueA(key, i, name, &nlen, NULL,
                               &type, data, &dlen);
            if (rc != ERROR_SUCCESS) {
                break;
            }
            rh_json_key(j, name);
            if (type == REG_DWORD && dlen == 4) {
                rh_json_int(j, (i32)(*(DWORD*)data));
            } else if ((type == REG_SZ || type == REG_EXPAND_SZ)) {
                data[sizeof(data) - 1] = 0;
                rh_json_str(j, (const char*)data);
            } else {
                rh_json_str(j, "");
            }
            ++i;
        }
        rh_json_end_obj(j);

        rh_json_key(j, "subkeys");
        rh_json_begin_arr(j);
        i = 0;
        for (;;) {
            nlen = (DWORD)sizeof(name);
            rc = RegEnumKeyExA(key, i, name, &nlen,
                               NULL, NULL, NULL, NULL);
            if (rc != ERROR_SUCCESS) {
                break;
            }
            rh_json_arr_str(j, name);
            ++i;
        }
        rh_json_end_arr(j);
        rh_json_end_obj(j);
    }

    RegCloseKey(key);
    r = rh_json_finish(j);
    if (r == NULL) {
        rh_err(c, "wire_desync");
    } else {
        rh_ok_json(c, r);
    }
    free(j);
}

/* --- registry.write ---------------------------------------------------- */

void rh_verb_registry_write(RhConn* c, const RhRequest* req)
{
    RhArgs      a;
    HKEY        root;
    const char* subkey;
    HKEY        key;
    LONG        rc;
    const char* tname;
    const char* data;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 4) {
        rh_err_msg(c, "invalid_args",
                   "registry.write requires <path> <name> <type> <data>");
        return;
    }
    if (!split_root(a.pos[0], &root, &subkey)) {
        rh_err_msg(c, "invalid_args", "unknown registry root");
        return;
    }
    rc = RegCreateKeyExA(root, subkey, 0, NULL, 0,
                         KEY_SET_VALUE, NULL, &key, NULL);
    if (rc != ERROR_SUCCESS) {
        rh_err(c, rh_win32_code((DWORD)rc));
        return;
    }
    tname = a.pos[2];
    data  = a.pos[3];
    if (strcmp(tname, "REG_DWORD") == 0) {
        DWORD v = (DWORD)strtoul(data, NULL, 0);
        rc = RegSetValueExA(key, a.pos[1], 0, REG_DWORD,
                            (const BYTE*)&v, sizeof(v));
    } else {
        /* REG_SZ / REG_EXPAND_SZ / default: store as string. */
        DWORD t = (strcmp(tname, "REG_EXPAND_SZ") == 0)
                  ? REG_EXPAND_SZ : REG_SZ;
        rc = RegSetValueExA(key, a.pos[1], 0, t,
                            (const BYTE*)data,
                            (DWORD)(strlen(data) + 1));
    }
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) {
        rh_err(c, rh_win32_code((DWORD)rc));
        return;
    }
    rh_ok(c);
}

/* --- registry.delete --------------------------------------------------- */

void rh_verb_registry_delete(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = { { "--value", 1 } };
    RhArgs      a;
    HKEY        root;
    const char* subkey;
    LONG        rc;

    rh_args_parse(req, defs, 1, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 1) {
        rh_err_msg(c, "invalid_args", "registry.delete requires <path>");
        return;
    }
    if (!split_root(a.pos[0], &root, &subkey)) {
        rh_err_msg(c, "invalid_args", "unknown registry root");
        return;
    }
    if (a.val[0] != NULL) {
        HKEY key;
        rc = RegOpenKeyExA(root, subkey, 0, KEY_SET_VALUE, &key);
        if (rc != ERROR_SUCCESS) {
            rh_err(c, "not_found");
            return;
        }
        rc = RegDeleteValueA(key, a.val[0]);
        RegCloseKey(key);
    } else {
        rc = RegDeleteKeyA(root, subkey);
    }
    if (rc != ERROR_SUCCESS) {
        rh_err(c, (rc == ERROR_FILE_NOT_FOUND) ? "not_found"
                                               : rh_win32_code((DWORD)rc));
        return;
    }
    rh_ok(c);
}

/* --- registry.wait ----------------------------------------------------- */

void rh_verb_registry_wait(RhConn* c, const RhRequest* req)
{
    RhArgs      a;
    HKEY        root;
    const char* subkey;
    HKEY        key;
    LONG        rc;
    HANDLE      ev;
    int         timeout_ms;
    DWORD       wr;
    RhJson*     j;
    const char* r;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 2) {
        rh_err_msg(c, "invalid_args",
                   "registry.wait requires <path> <timeout-ms>");
        return;
    }
    if (!split_root(a.pos[0], &root, &subkey)) {
        rh_err_msg(c, "invalid_args", "unknown registry root");
        return;
    }
    timeout_ms = atoi(a.pos[1]);
    rc = RegOpenKeyExA(root, subkey, 0, KEY_NOTIFY, &key);
    if (rc != ERROR_SUCCESS) {
        rh_err(c, "not_found");
        return;
    }
    ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (ev == NULL) {
        RegCloseKey(key);
        rh_err(c, "wire_desync");
        return;
    }
    rc = RegNotifyChangeKeyValue(key, TRUE,
                                 REG_NOTIFY_CHANGE_NAME |
                                 REG_NOTIFY_CHANGE_LAST_SET,
                                 ev, TRUE);
    if (rc != ERROR_SUCCESS) {
        CloseHandle(ev);
        RegCloseKey(key);
        rh_err_msg(c, "not_supported", "registry change notify unavailable");
        return;
    }
    wr = WaitForSingleObject(ev, (DWORD)timeout_ms);
    CloseHandle(ev);
    RegCloseKey(key);
    if (wr == WAIT_TIMEOUT) {
        rh_err_kv(c, "timeout", "deadline", a.pos[1]);
        return;
    }
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
}
