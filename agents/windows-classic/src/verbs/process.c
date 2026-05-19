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

#include "process.h"
#include "common.h"
#include "../json.h"
#include "../protocol.h"

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <stdlib.h>
#include <string.h>

/* --- process.start handle cache (regression #16) ----------------------- */

#define RH_PROC_CACHE 64

static struct {
    DWORD  pid;
    HANDLE h;
} g_proc[RH_PROC_CACHE];
static int g_proc_next = 0;

static void proc_cache_put(DWORD pid, HANDLE h)
{
    if (g_proc[g_proc_next].h != NULL) {
        CloseHandle(g_proc[g_proc_next].h);
    }
    g_proc[g_proc_next].pid = pid;
    g_proc[g_proc_next].h   = h;
    g_proc_next = (g_proc_next + 1) % RH_PROC_CACHE;
}

static HANDLE proc_cache_get(DWORD pid)
{
    int i;
    for (i = 0; i < RH_PROC_CACHE; ++i) {
        if (g_proc[i].h != NULL && g_proc[i].pid == pid) {
            return g_proc[i].h;
        }
    }
    return NULL;
}

/* --- case-insensitive substring (filter) ------------------------------- */

static int ci_find(const char* hay, const char* needle)
{
    size_t nl;
    size_t i;
    if (needle == NULL || needle[0] == '\0') return 1;
    if (hay == NULL) return 0;
    nl = strlen(needle);
    for (i = 0; hay[i] != '\0'; ++i) {
        size_t k = 0;
        while (k < nl) {
            char a = hay[i + k];
            char b = needle[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
            ++k;
        }
        if (k == nl) return 1;
    }
    return 0;
}

/* --- process.list ------------------------------------------------------ */

typedef HANDLE (WINAPI *PFN_CTS)(DWORD, DWORD);
typedef BOOL   (WINAPI *PFN_P32F)(HANDLE, LPPROCESSENTRY32);
typedef BOOL   (WINAPI *PFN_P32N)(HANDLE, LPPROCESSENTRY32);

typedef BOOL   (WINAPI *PFN_ENUMPROC)(DWORD*, DWORD, DWORD*);
typedef DWORD  (WINAPI *PFN_GMBN)(HANDLE, HMODULE, LPSTR, DWORD);

static int list_via_toolhelp(RhJson* j, const char* filter)
{
    HMODULE       k32;
    PFN_CTS       cts;
    PFN_P32F      p32f;
    PFN_P32N      p32n;
    HANDLE        snap;
    PROCESSENTRY32 pe;

    k32 = GetModuleHandleA("kernel32.dll");
    if (k32 == NULL) {
        return 0;
    }
    cts  = (PFN_CTS) GetProcAddress(k32, "CreateToolhelp32Snapshot");
    p32f = (PFN_P32F)GetProcAddress(k32, "Process32First");
    p32n = (PFN_P32N)GetProcAddress(k32, "Process32Next");
    if (cts == NULL || p32f == NULL || p32n == NULL) {
        return 0;
    }
    snap = cts(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return 0;
    }
    memset(&pe, 0, sizeof(pe));
    pe.dwSize = sizeof(pe);
    if (p32f(snap, &pe)) {
        do {
            if (!ci_find(pe.szExeFile, filter)) {
                continue;
            }
            rh_json_arr_elem(j);
            rh_json_begin_obj(j);
            rh_json_key(j, "pid");
            rh_json_int(j, (i32)pe.th32ProcessID);
            rh_json_key(j, "image");
            rh_json_str(j, pe.szExeFile);
            rh_json_key(j, "ppid");
            rh_json_int(j, (i32)pe.th32ParentProcessID);
            rh_json_end_obj(j);
        } while (p32n(snap, &pe));
    }
    CloseHandle(snap);
    return 1;
}

static int list_via_psapi(RhJson* j, const char* filter)
{
    HMODULE      ps;
    PFN_ENUMPROC enum_proc;
    PFN_GMBN     gmbn;
    DWORD        pids[1024];
    DWORD        needed;
    int          n;
    int          i;

    ps = LoadLibraryA("psapi.dll");
    if (ps == NULL) {
        return 0;
    }
    enum_proc = (PFN_ENUMPROC)GetProcAddress(ps, "EnumProcesses");
    gmbn      = (PFN_GMBN)    GetProcAddress(ps, "GetModuleBaseNameA");
    if (enum_proc == NULL || gmbn == NULL) {
        FreeLibrary(ps);
        return 0;
    }
    if (!enum_proc(pids, sizeof(pids), &needed)) {
        FreeLibrary(ps);
        return 0;
    }
    n = (int)(needed / sizeof(DWORD));
    for (i = 0; i < n; ++i) {
        HANDLE ph;
        char   name[MAX_PATH];

        name[0] = '\0';
        ph = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                         FALSE, pids[i]);
        if (ph != NULL) {
            gmbn(ph, NULL, name, (DWORD)sizeof(name));
            CloseHandle(ph);
        }
        if (name[0] == '\0') {
            continue;
        }
        if (!ci_find(name, filter)) {
            continue;
        }
        rh_json_arr_elem(j);
        rh_json_begin_obj(j);
        rh_json_key(j, "pid");   rh_json_int(j, (i32)pids[i]);
        rh_json_key(j, "image"); rh_json_str(j, name);
        rh_json_key(j, "ppid");  rh_json_int(j, 0);
        rh_json_end_obj(j);
    }
    FreeLibrary(ps);
    return 1;
}

void rh_verb_process_list(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = { { "--filter", 1 } };
    RhArgs      a;
    RhJson*     j;
    const char* r;
    int         ok;

    rh_args_parse(req, defs, 1, &a);
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
    rh_json_key(j, "processes");
    rh_json_begin_arr(j);

    ok = list_via_toolhelp(j, a.val[0]);
    if (!ok) {
        ok = list_via_psapi(j, a.val[0]);
    }

    rh_json_end_arr(j);
    rh_json_end_obj(j);

    if (!ok) {
        free(j);
        rh_err_msg(c, "not_supported",
                   "no process enumeration API on this OS");
        return;
    }
    r = rh_json_finish(j);
    if (r == NULL) {
        rh_err(c, "wire_desync");
    } else {
        rh_ok_json(c, r);
    }
    free(j);
}

/* --- process.start ----------------------------------------------------- */

void rh_verb_process_start(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = { { "--stdin", 1 } };
    RhArgs              a;
    char                cmd[1024];
    STARTUPINFOA        si;
    PROCESS_INFORMATION pi;
    RhJson*             j;
    const char*         r;

    rh_args_parse(req, defs, 1, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 1) {
        rh_err_msg(c, "invalid_args", "process.start requires <argv>");
        return;
    }

    /* Optional length-prefixed stdin payload: drained but not piped on
     * classic (anonymous-pipe stdin redirection adds Win9x handle-
     * inheritance fragility for no conformance benefit). Documented. */
    if (a.seen[0] && a.val[0] != NULL) {
        int   n = atoi(a.val[0]);
        char* tmp;
        if (n > 0 && n < (1 * 1024 * 1024)) {
            tmp = (char*)malloc((size_t)n);
            if (tmp != NULL) {
                rh_read_payload(&c->reader, tmp, n);
                free(tmp);
            }
        }
    }

    _snprintf(cmd, sizeof(cmd), "%s", a.pos[0]);
    cmd[sizeof(cmd) - 1] = '\0';

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                        0, NULL, NULL, &si, &pi)) {
        rh_err(c, rh_win32_code(GetLastError()));
        return;
    }
    /* Keep the process handle alive so process.wait works after exit;
     * the thread handle is not needed. */
    CloseHandle(pi.hThread);
    proc_cache_put(pi.dwProcessId, pi.hProcess);

    j = (RhJson*)malloc(sizeof(RhJson));
    if (j == NULL) {
        rh_err(c, "wire_desync");
        return;
    }
    rh_json_init(j);
    rh_json_begin_obj(j);
    rh_json_key(j, "pid");
    rh_json_int(j, (i32)pi.dwProcessId);
    rh_json_end_obj(j);
    r = rh_json_finish(j);
    if (r == NULL) {
        rh_err(c, "wire_desync");
    } else {
        rh_ok_json(c, r);
    }
    free(j);
}

/* --- process.shell (ShellExecuteExA) ----------------------------------- */

void rh_verb_process_shell(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = {
        { "--args", 1 },
        { "--verb", 1 }
    };
    RhArgs           a;
    SHELLEXECUTEINFOA se;
    RhJson*          j;
    const char*      r;
    DWORD            pid;

    rh_args_parse(req, defs, 2, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 1) {
        rh_err_msg(c, "invalid_args", "process.shell requires <path>");
        return;
    }

    memset(&se, 0, sizeof(se));
    se.cbSize       = sizeof(se);
    se.fMask        = SEE_MASK_NOCLOSEPROCESS;
    se.lpVerb       = a.val[1];          /* NULL => default verb */
    se.lpFile       = a.pos[0];
    se.lpParameters = a.val[0];
    se.nShow        = SW_SHOWNORMAL;

    if (!ShellExecuteExA(&se)) {
        rh_err(c, rh_win32_code(GetLastError()));
        return;
    }
    /* GetProcessId is XP+ (absent on the NT4/Win9x/Win2000 classic range),
     * so the spawned PID is not recoverable here. PROTOCOL.md 4.8 permits
     * pid 0 for process.shell. The handle is closed rather than cached
     * because process.wait is keyed by PID. */
    pid = 0;
    if (se.hProcess != NULL) {
        CloseHandle(se.hProcess);
    }

    j = (RhJson*)malloc(sizeof(RhJson));
    if (j == NULL) {
        rh_err(c, "wire_desync");
        return;
    }
    rh_json_init(j);
    rh_json_begin_obj(j);
    rh_json_key(j, "pid");
    rh_json_int(j, (i32)pid);
    rh_json_end_obj(j);
    r = rh_json_finish(j);
    if (r == NULL) {
        rh_err(c, "wire_desync");
    } else {
        rh_ok_json(c, r);
    }
    free(j);
}

/* --- process.kill ------------------------------------------------------ */

void rh_verb_process_kill(RhConn* c, const RhRequest* req)
{
    RhArgs a;
    DWORD  pid;
    HANDLE h;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 1) {
        rh_err_msg(c, "invalid_args", "process.kill requires <pid>");
        return;
    }
    pid = (DWORD)strtoul(a.pos[0], NULL, 10);
    h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (h == NULL) {
        rh_err(c, "not_found");
        return;
    }
    if (!TerminateProcess(h, 1)) {
        CloseHandle(h);
        rh_err(c, rh_win32_code(GetLastError()));
        return;
    }
    CloseHandle(h);
    rh_ok(c);
}

/* --- process.wait ------------------------------------------------------ */

void rh_verb_process_wait(RhConn* c, const RhRequest* req)
{
    RhArgs      a;
    DWORD       pid;
    int         timeout_ms;
    HANDLE      h;
    int         opened;
    DWORD       wr;
    DWORD       code;
    RhJson*     j;
    const char* r;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 2) {
        rh_err_msg(c, "invalid_args",
                   "process.wait requires <pid> <timeout-ms>");
        return;
    }
    pid        = (DWORD)strtoul(a.pos[0], NULL, 10);
    timeout_ms = atoi(a.pos[1]);

    opened = 0;
    h = proc_cache_get(pid);             /* survives OS reaping (#16) */
    if (h == NULL) {
        h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION,
                        FALSE, pid);
        opened = 1;
    }
    if (h == NULL) {
        rh_err(c, "not_found");
        return;
    }
    wr = WaitForSingleObject(h, (DWORD)timeout_ms);
    if (wr == WAIT_TIMEOUT) {
        if (opened) CloseHandle(h);
        rh_err_kv(c, "timeout", "deadline", a.pos[1]);
        return;
    }
    code = 0;
    GetExitCodeProcess(h, &code);
    if (opened) {
        CloseHandle(h);
    }

    j = (RhJson*)malloc(sizeof(RhJson));
    if (j == NULL) {
        rh_err(c, "wire_desync");
        return;
    }
    rh_json_init(j);
    rh_json_begin_obj(j);
    rh_json_key(j, "exit_code");
    rh_json_int(j, (i32)code);
    rh_json_end_obj(j);
    r = rh_json_finish(j);
    if (r == NULL) {
        rh_err(c, "wire_desync");
    } else {
        rh_ok_json(c, r);
    }
    free(j);
}
