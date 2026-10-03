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
#include <stdio.h>
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

/* --- JSON helpers ----------------------------------------------------- */

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

/* Integer as a raw JSON number (values can exceed i32, e.g. byte counts). */
static void json_ulong(RhJson* j, unsigned long v)
{
    char num[16];
    _snprintf(num, sizeof(num), "%lu", v);
    num[sizeof(num) - 1] = '\0';
    rh_json_raw(j, num);
}

/* --- process.list ------------------------------------------------------ */

typedef HANDLE (WINAPI *PFN_CTS)(DWORD, DWORD);
typedef BOOL   (WINAPI *PFN_P32F)(HANDLE, LPPROCESSENTRY32);
typedef BOOL   (WINAPI *PFN_P32N)(HANDLE, LPPROCESSENTRY32);

typedef BOOL   (WINAPI *PFN_ENUMPROC)(DWORD*, DWORD, DWORD*);
typedef DWORD  (WINAPI *PFN_GMBN)(HANDLE, HMODULE, LPSTR, DWORD);

/* PROCESS_MEMORY_COUNTERS (psapi.h is not in a stock VC98). */
typedef struct {
    DWORD cb;                         /* SIZE_T fields: x86-only build */
    DWORD PageFaultCount;
    DWORD PeakWorkingSetSize;
    DWORD WorkingSetSize;
    DWORD QuotaPeakPagedPoolUsage;
    DWORD QuotaPagedPoolUsage;
    DWORD QuotaPeakNonPagedPoolUsage;
    DWORD QuotaNonPagedPoolUsage;
    DWORD PagefileUsage;
    DWORD PeakPagefileUsage;
} RhProcMem;
typedef BOOL (WINAPI *PFN_GPMI)(HANDLE, RhProcMem*, DWORD);
typedef BOOL (WINAPI *PFN_GPHC)(HANDLE, PDWORD);

typedef struct {
    RhJson*      j;
    const char*  pattern;
    int          counters;
    PFN_GPMI     mem_info;      /* psapi GetProcessMemoryInfo, NT 4+ */
    PFN_GPHC     handle_count;  /* kernel32 GetProcessHandleCount, XP+ */
} ListCtx;

/* Members for one entry; `threads` < 0 when unknown. */
static void emit_proc(ListCtx* ctx, DWORD pid, const char* image,
                      DWORD ppid, long threads)
{
    RhJson*   j = ctx->j;
    HANDLE    ph;
    RhProcMem pm;
    DWORD     handles;

    rh_json_arr_elem(j);
    rh_json_begin_obj(j);
    rh_json_key(j, "pid");   rh_json_int(j, (i32)pid);
    rh_json_key(j, "image"); rh_json_str(j, image);
    rh_json_key(j, "ppid");  rh_json_int(j, (i32)ppid);
    if (ctx->counters) {
        if (threads >= 0) {
            rh_json_key(j, "thread_count"); rh_json_int(j, (i32)threads);
        }
        ph = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                         FALSE, pid);
        /* Protected processes refuse OpenProcess: zeroed counters. */
        memset(&pm, 0, sizeof(pm));
        pm.cb = sizeof(pm);
        if (ph == NULL || ctx->mem_info == NULL ||
            !ctx->mem_info(ph, &pm, sizeof(pm))) {
            memset(&pm, 0, sizeof(pm));
        }
        handles = 0;
        if (ph != NULL && ctx->handle_count != NULL) {
            ctx->handle_count(ph, &handles);
        }
        if (ph != NULL) {
            CloseHandle(ph);
        }
        rh_json_key(j, "rss_bytes");
        json_ulong(j, (unsigned long)pm.WorkingSetSize);
        rh_json_key(j, "working_set_bytes");
        json_ulong(j, (unsigned long)pm.WorkingSetSize);
        rh_json_key(j, "private_bytes");
        json_ulong(j, (unsigned long)pm.PagefileUsage);
        rh_json_key(j, "handle_count");
        rh_json_int(j, (i32)handles);
    }
    rh_json_end_obj(j);
}

static int list_via_toolhelp(ListCtx* ctx)
{
    HMODULE        k32;
    PFN_CTS        cts;
    PFN_P32F       p32f;
    PFN_P32N       p32n;
    HANDLE         snap;
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
            if (ctx->pattern != NULL &&
                !rh_contains_ci(pe.szExeFile, ctx->pattern)) {
                continue;
            }
            emit_proc(ctx, pe.th32ProcessID, pe.szExeFile,
                      pe.th32ParentProcessID, (long)pe.cntThreads);
        } while (p32n(snap, &pe));
    }
    CloseHandle(snap);
    return 1;
}

static int list_via_psapi(ListCtx* ctx, HMODULE ps)
{
    PFN_ENUMPROC enum_proc;
    PFN_GMBN     gmbn;
    DWORD        pids[1024];
    DWORD        needed;
    int          n;
    int          i;
    HANDLE       ph;
    char         name[MAX_PATH];

    if (ps == NULL) {
        return 0;
    }
    enum_proc = (PFN_ENUMPROC)GetProcAddress(ps, "EnumProcesses");
    gmbn      = (PFN_GMBN)    GetProcAddress(ps, "GetModuleBaseNameA");
    if (enum_proc == NULL || gmbn == NULL ||
        !enum_proc(pids, sizeof(pids), &needed)) {
        return 0;
    }
    n = (int)(needed / sizeof(DWORD));
    for (i = 0; i < n; ++i) {
        name[0] = '\0';
        ph = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                         FALSE, pids[i]);
        if (ph != NULL) {
            gmbn(ph, NULL, name, (DWORD)sizeof(name));
            CloseHandle(ph);
        }
        /* Unqueryable (protected / kernel) processes report image "". */
        if (ctx->pattern != NULL && !rh_contains_ci(name, ctx->pattern)) {
            continue;
        }
        emit_proc(ctx, pids[i], name, 0, -1);
    }
    return 1;
}

void rh_verb_process_list(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = {
        { "--pattern",          RH_FLAG_VALUE },
        { "--include-counters", RH_FLAG_BOOL  }
    };
    RhArgs  a;
    RhJson* j;
    ListCtx ctx;
    HMODULE ps;
    HMODULE k32;
    int     ok;

    rh_args_parse(req, defs, 2, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.missing_val) {
        rh_err_msg(c, "invalid_args", "--pattern requires a value");
        return;
    }
    j = jopen(c);
    if (j == NULL) {
        return;
    }
    ps  = LoadLibraryA("psapi.dll");   /* absent on 9x: counters zero */
    k32 = GetModuleHandleA("kernel32.dll");
    ctx.j            = j;
    ctx.pattern      = a.val[0];
    ctx.counters     = rh_arg_bool(&a, 1, 0);
    ctx.mem_info     = (ps != NULL)
                       ? (PFN_GPMI)GetProcAddress(ps, "GetProcessMemoryInfo")
                       : NULL;
    ctx.handle_count = (k32 != NULL)
                       ? (PFN_GPHC)GetProcAddress(k32,
                                                  "GetProcessHandleCount")
                       : NULL;

    rh_json_begin_obj(j);
    rh_json_key(j, "processes");
    rh_json_begin_arr(j);
    ok = list_via_toolhelp(&ctx);
    if (!ok) {
        ok = list_via_psapi(&ctx, ps);
    }
    rh_json_end_arr(j);
    rh_json_end_obj(j);
    if (ps != NULL) {
        FreeLibrary(ps);
    }

    if (!ok) {
        /* NT 4 without psapi.dll: no enumeration API at all. Ledgered
         * known divergence (not_supported is outside the v2.1 set). */
        free(j);
        rh_err_msg(c, "not_supported",
                   "no process enumeration API on this OS");
        return;
    }
    jsend(c, j);
}

/* --- process.start ----------------------------------------------------- */

/* Append `arg` to cmd with MSVCRT argv quoting (CommandLineToArgvW rules):
 * quoted when empty or containing space/tab/quote; backslashes doubled
 * only where they precede a quote. Returns 0 when it does not fit. */
static int append_arg(char* cmd, int cap, const char* arg)
{
    int len = (int)strlen(cmd);
    int quote;
    int bs;
    const char* p;

    quote = (arg[0] == '\0' || strpbrk(arg, " \t\"") != NULL) ? 1 : 0;
    if (len > 0) {
        if (len + 1 >= cap) return 0;
        cmd[len++] = ' ';
    }
    if (quote) {
        if (len + 1 >= cap) return 0;
        cmd[len++] = '"';
    }
    for (p = arg; ; ++p) {
        bs = 0;
        while (*p == '\\') {
            ++bs;
            ++p;
        }
        if (*p == '\0') {
            if (quote) bs *= 2;          /* before the closing quote */
        } else if (*p == '"') {
            bs = bs * 2 + 1;             /* escape the quote itself */
        }
        if (len + bs + 2 >= cap) return 0;
        while (bs-- > 0) cmd[len++] = '\\';
        if (*p == '\0') break;
        cmd[len++] = *p;
    }
    if (quote) {
        if (len + 1 >= cap) return 0;
        cmd[len++] = '"';
    }
    cmd[len] = '\0';
    return 1;
}

void rh_verb_process_start(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = {
        { "--stdin", RH_FLAG_VALUE },
        { "--cwd",   RH_FLAG_VALUE }
    };
    RhArgs              a;
    char                cmd[2048];
    int                 i;
    STARTUPINFOA        si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa;
    HANDLE              rd;
    HANDLE              wr;
    DWORD               wrote;
    BOOL                ok;
    DWORD               e;
    RhJson*             j;

    rh_args_parse(req, defs, 2, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 1 || a.missing_val) {
        rh_err_msg(c, "invalid_args", "process.start requires <argv>");
        return;
    }

    /* v2.1 grammar: one positional is the whole command line (the suite
     * sends "cmd.exe /c exit 7"); several positionals are an argv array,
     * quoted element-by-element. */
    cmd[0] = '\0';
    if (a.npos == 1) {
        _snprintf(cmd, sizeof(cmd), "%s", a.pos[0]);
        cmd[sizeof(cmd) - 1] = '\0';
    } else {
        for (i = 0; i < a.npos; ++i) {
            if (!append_arg(cmd, (int)sizeof(cmd), a.pos[i])) {
                rh_err_msg(c, "invalid_args", "command line too long");
                return;
            }
        }
    }

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    rd = NULL;
    wr = NULL;

    /* --stdin: UTF-8 text piped to the child, then closed. */
    if (a.val[0] != NULL) {
        sa.nLength              = sizeof(sa);
        sa.lpSecurityDescriptor = NULL;
        sa.bInheritHandle       = TRUE;
        if (!CreatePipe(&rd, &wr, &sa, 0)) {
            rh_err(c, rh_win32_code(GetLastError()));
            return;
        }
        /* Child must not inherit our write end (else its stdin never sees
         * EOF). DuplicateHandle rather than SetHandleInformation, which
         * is a non-functional stub on Win9x. */
        if (!DuplicateHandle(GetCurrentProcess(), wr, GetCurrentProcess(),
                             &wr, 0, FALSE,
                             DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE)) {
            CloseHandle(rd);
            rh_err(c, rh_win32_code(GetLastError()));
            return;
        }
        si.dwFlags   = STARTF_USESTDHANDLES;
        si.hStdInput = rd;
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    }

    ok = CreateProcessA(NULL, cmd, NULL, NULL, (rd != NULL) ? TRUE : FALSE,
                        0, NULL, a.val[1], &si, &pi);
    e = GetLastError();
    if (rd != NULL) {
        CloseHandle(rd);
    }
    if (!ok) {
        if (wr != NULL) CloseHandle(wr);
        rh_err(c, rh_win32_code(e));
        return;
    }
    if (wr != NULL) {
        WriteFile(wr, a.val[0], (DWORD)strlen(a.val[0]), &wrote, NULL);
        CloseHandle(wr);
    }
    /* Keep the process handle alive so process.wait works after exit;
     * the thread handle is not needed. */
    CloseHandle(pi.hThread);
    proc_cache_put(pi.dwProcessId, pi.hProcess);

    j = jopen(c);
    if (j == NULL) {
        return;
    }
    rh_json_begin_obj(j);
    rh_json_key(j, "pid"); rh_json_int(j, (i32)pi.dwProcessId);
    rh_json_end_obj(j);
    jsend(c, j);
}

/* --- process.shell (ShellExecuteExA) ----------------------------------- */

#ifndef ERROR_NO_ASSOCIATION
#define ERROR_NO_ASSOCIATION 1155
#endif
#ifndef SE_ERR_NOASSOC
#define SE_ERR_NOASSOC 31
#endif

typedef DWORD (WINAPI *PFN_GPID)(HANDLE);

static const char* shell_err_code(DWORD e)
{
    switch (e) {
        case ERROR_NO_ASSOCIATION:
        case SE_ERR_NOASSOC:
            return "no_handler";
        case ERROR_CANCELLED:
            return "user_cancelled";
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_BAD_NETPATH:
            return "not_found";
        case ERROR_ACCESS_DENIED:
            return "permission_denied";
        default:
            return "invalid_args";
    }
}

void rh_verb_process_shell(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = {
        { "--args", RH_FLAG_VALUE },
        { "--verb", RH_FLAG_VALUE },
        { "--cwd",  RH_FLAG_VALUE },
        { "--path", RH_FLAG_VALUE }
    };
    static const char* kVerbs[] = {
        "open", "runas", "print", "edit", "explore", "find"
    };
    RhArgs            a;
    SHELLEXECUTEINFOA se;
    const char*       path;
    const char*       verb;
    int               i;
    HMODULE           k32;
    PFN_GPID          get_pid;
    DWORD             pid;
    RhJson*           j;

    rh_args_parse(req, defs, 4, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    path = rh_arg_named_or_pos(&a, 3, 0);
    if (path == NULL || a.missing_val) {
        rh_err_msg(c, "invalid_args", "process.shell requires <path>");
        return;
    }
    verb = (a.val[1] != NULL) ? a.val[1] : "open";
    for (i = 0; i < (int)(sizeof(kVerbs) / sizeof(kVerbs[0])); ++i) {
        if (strcmp(verb, kVerbs[i]) == 0) {
            break;
        }
    }
    if (i == (int)(sizeof(kVerbs) / sizeof(kVerbs[0]))) {
        rh_err_msg(c, "invalid_args",
                   "verb must be open|runas|print|edit|explore|find");
        return;
    }

    memset(&se, 0, sizeof(se));
    se.cbSize       = sizeof(se);
    se.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    se.lpVerb       = verb;
    se.lpFile       = path;
    se.lpParameters = a.val[0];
    se.lpDirectory  = a.val[2];
    se.nShow        = SW_SHOWNORMAL;

    if (!ShellExecuteExA(&se)) {
        DWORD e = GetLastError();
        if (e == 0 && (DWORD)se.hInstApp <= 32) {
            e = (DWORD)se.hInstApp;            /* SE_ERR_* fallback */
        }
        rh_err(c, shell_err_code(e));
        return;
    }
    /* GetProcessId is XP SP1+; older classic hosts report pid null (the
     * spec allows null when the pid is not known). */
    pid = 0;
    if (se.hProcess != NULL) {
        k32 = GetModuleHandleA("kernel32.dll");
        get_pid = (k32 != NULL)
                  ? (PFN_GPID)GetProcAddress(k32, "GetProcessId") : NULL;
        if (get_pid != NULL) {
            pid = get_pid(se.hProcess);
        }
        if (pid != 0) {
            proc_cache_put(pid, se.hProcess);   /* process.wait by pid */
        } else {
            CloseHandle(se.hProcess);
        }
    }

    j = jopen(c);
    if (j == NULL) {
        return;
    }
    rh_json_begin_obj(j);
    rh_json_key(j, "pid");
    if (pid != 0) {
        rh_json_int(j, (i32)pid);
    } else {
        rh_json_null(j);
    }
    rh_json_end_obj(j);
    jsend(c, j);
}

/* --- process.kill ------------------------------------------------------ */

/* pid from positional 0 or --pid; replies invalid_args on failure. */
static int get_pid_arg(RhConn* c, const RhArgs* a, int idx, DWORD* out)
{
    long v;
    const char* s = rh_arg_named_or_pos(a, idx, 0);

    if (s == NULL || !rh_parse_long(s, &v) || v < 1) {
        rh_err_msg(c, "invalid_args", "requires a pid >= 1");
        return 0;
    }
    *out = (DWORD)v;
    return 1;
}

void rh_verb_process_kill(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = {
        { "--exit-code", RH_FLAG_VALUE },
        { "--pid",       RH_FLAG_VALUE }
    };
    RhArgs a;
    DWORD  pid;
    long   code;
    HANDLE h;
    DWORD  e;

    rh_args_parse(req, defs, 2, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (!get_pid_arg(c, &a, 1, &pid)) {
        return;
    }
    code = 1;
    if (a.val[0] != NULL && !rh_parse_long(a.val[0], &code)) {
        rh_err_msg(c, "invalid_args", "exit_code must be an integer");
        return;
    }
    h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (h == NULL) {
        e = GetLastError();
        rh_err(c, (e == ERROR_ACCESS_DENIED) ? "permission_denied"
                                             : "not_found");
        return;
    }
    if (!TerminateProcess(h, (UINT)code)) {
        e = GetLastError();
        CloseHandle(h);
        rh_err(c, rh_win32_code(e));
        return;
    }
    CloseHandle(h);
    rh_ok(c);
}

/* --- process.wait ------------------------------------------------------ */

void rh_verb_process_wait(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = {
        { "--timeout-ms", RH_FLAG_VALUE },
        { "--pid",        RH_FLAG_VALUE }
    };
    RhArgs      a;
    DWORD       pid;
    const char* to_s;
    long        timeout_ms;
    HANDLE      h;
    int         opened;
    DWORD       wr;
    DWORD       code;
    RhJson*     j;

    rh_args_parse(req, defs, 2, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (!get_pid_arg(c, &a, 1, &pid)) {
        return;
    }
    /* Optional; absent = wait indefinitely. The v2.0 grammar passed it
     * as the second positional, still accepted. */
    to_s = rh_arg_named_or_pos(&a, 0, a.seen[1] ? 0 : 1);
    timeout_ms = -1;
    if (to_s != NULL && (!rh_parse_long(to_s, &timeout_ms) ||
                         timeout_ms < 0)) {
        rh_err_msg(c, "invalid_args", "timeout_ms must be >= 0");
        return;
    }

    opened = 0;
    h = proc_cache_get(pid);             /* survives OS reaping (#16) */
    if (h == NULL) {
        h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION,
                        FALSE, pid);
        opened = 1;
    }
    if (h == NULL) {
        rh_err(c, (GetLastError() == ERROR_ACCESS_DENIED)
                  ? "permission_denied" : "not_found");
        return;
    }
    wr = WaitForSingleObject(h, (timeout_ms < 0) ? INFINITE
                                                 : (DWORD)timeout_ms);
    if (wr == WAIT_TIMEOUT) {
        if (opened) CloseHandle(h);
        rh_err_kv(c, "timeout", "pid", (a.npos > 0) ? a.pos[0] : a.val[1]);
        return;
    }
    code = 0;
    GetExitCodeProcess(h, &code);
    if (opened) {
        CloseHandle(h);
    }

    j = jopen(c);
    if (j == NULL) {
        return;
    }
    rh_json_begin_obj(j);
    rh_json_key(j, "exit_code"); rh_json_int(j, (i32)code);
    rh_json_end_obj(j);
    jsend(c, j);
}
