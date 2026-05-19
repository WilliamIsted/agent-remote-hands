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

/* system.power.* verb handlers — C89/Win32 port of shared/verbs/system.cpp.
 *
 * Platform notes:
 *   ExitWindowsEx   -- all Win32 targets (NT4 SP6a, Win95 OSR2, Win2000)
 *   SetSuspendState -- Win2000+; loaded via GetProcAddress (powrprof.dll)
 *   LockWorkStation -- Win2000+; loaded via GetProcAddress (user32.dll)
 *   ShutdownBlockReasonQuery -- Vista+; not available, blockers always empty
 *
 * Delayed power actions use a background CreateThread + manual-reset event
 * (Win32 equivalent of C++17 condition_variable::wait_until). */

#include "system_power.h"
#include "../debug.h"
#include "../protocol.h"

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* VC98 / early SDK may not have these shutdown-reason constants. */
#ifndef SHTDN_REASON_MAJOR_OPERATINGSYSTEM
#define SHTDN_REASON_MAJOR_OPERATINGSYSTEM 0x00020000UL
#endif
#ifndef SHTDN_REASON_FLAG_PLANNED
#define SHTDN_REASON_FLAG_PLANNED          0x80000000UL
#endif
#ifndef EWX_FORCE
#define EWX_FORCE 0x00000004
#endif

/* ---------------------------------------------------------------------------
 * Pending-shutdown global state
 *
 * At most one delayed power action is permitted at a time.  The CRITICAL_SECTION
 * protects g_power_active + g_power_deadline_ms.  g_power_cancel_evt is a
 * manual-reset event: ResetEvent() before arming, SetEvent() to cancel.
 * The background thread waits on it; timeout means fire, WAIT_OBJECT_0 means
 * cancelled. */

static CRITICAL_SECTION  g_power_cs;
static int               g_power_active      = 0;
static HANDLE            g_power_cancel_evt  = NULL;
static __int64           g_power_deadline_ms = 0;

/* ---------------------------------------------------------------------------
 * Thread arguments (heap-allocated; thread frees them) */

typedef struct {
    DWORD exit_flags;
    DWORD delay_ms;
} PowerThreadArgs;

/* ---------------------------------------------------------------------------
 * Internal helpers */

static void enable_shutdown_privilege(void)
{
    HANDLE           tok;
    TOKEN_PRIVILEGES tp;
    LUID             luid;

    /* On Win9x OpenProcessToken fails (no token model); ignore and proceed --
     * ExitWindowsEx does not require SeShutdownPrivilege on 9x. */
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        return;
    }
    if (LookupPrivilegeValueA(NULL, "SeShutdownPrivilege", &luid)) {
        tp.PrivilegeCount            = 1;
        tp.Privileges[0].Luid        = luid;
        tp.Privileges[0].Attributes  = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), NULL, NULL);
    }
    CloseHandle(tok);
}

static DWORD WINAPI power_delay_thread(LPVOID param)
{
    PowerThreadArgs* args;
    DWORD            exit_flags;
    DWORD            delay_ms;
    DWORD            reason;

    args       = (PowerThreadArgs*)param;
    exit_flags = args->exit_flags;
    delay_ms   = args->delay_ms;
    HeapFree(GetProcessHeap(), 0, args);

    reason = SHTDN_REASON_MAJOR_OPERATINGSYSTEM | SHTDN_REASON_FLAG_PLANNED;

    /* Fire on timeout; abort on SetEvent (cancel signal). */
    if (WaitForSingleObject(g_power_cancel_evt, delay_ms) == WAIT_TIMEOUT) {
        ExitWindowsEx(exit_flags, reason);
    }

    EnterCriticalSection(&g_power_cs);
    g_power_active = 0;
    LeaveCriticalSection(&g_power_cs);
    return 0;
}

/* Shared dispatcher for reboot / shutdown / logoff.  Handles --delay and
 * --force args, conflict detection, and the cancel-event lifecycle. */
static void do_power(RhConn* c, const RhRequest* req, DWORD exit_flags)
{
    DWORD            delay_seconds;
    int              force_flag;
    int              i;
    const char*      arg;
    long             deadline_unix;
    __int64          deadline_ms;
    PowerThreadArgs* targs;
    HANDLE           th;
    char             body[160];
    char             detail[128];
    DWORD            reason;

    delay_seconds = 0;
    force_flag    = 0;

    for (i = 0; i < req->argc; ++i) {
        arg = req->args[i];
        if (strcmp(arg, "--delay") == 0 && i + 1 < req->argc) {
            ++i;
            delay_seconds = (DWORD)strtoul(req->args[i], NULL, 10);
        } else if (strcmp(arg, "--force") == 0) {
            force_flag = 1;
        } else if (strlen(arg) >= 2 && arg[0] == '-' && arg[1] == '-') {
            _snprintf(detail, sizeof(detail),
                      "{\"unknown_flag\":\"%s\"}", arg);
            rh_send_err_json(c->sock, "invalid_args", detail);
            return;
        }
    }

    enable_shutdown_privilege();

    if (force_flag) {
        exit_flags |= EWX_FORCE;
    }
    reason = SHTDN_REASON_MAJOR_OPERATINGSYSTEM | SHTDN_REASON_FLAG_PLANNED;

    if (delay_seconds > 0) {
        EnterCriticalSection(&g_power_cs);
        if (g_power_active) {
            _snprintf(detail, sizeof(detail),
                      "{\"pending_until_ms\":%I64d}", g_power_deadline_ms);
            LeaveCriticalSection(&g_power_cs);
            rh_send_err_json(c->sock, "conflict", detail);
            return;
        }
        deadline_unix       = (long)time(NULL) + (long)delay_seconds;
        deadline_ms         = (__int64)deadline_unix * 1000;
        g_power_active      = 1;
        g_power_deadline_ms = deadline_ms;
        ResetEvent(g_power_cancel_evt);
        LeaveCriticalSection(&g_power_cs);

        targs = (PowerThreadArgs*)HeapAlloc(GetProcessHeap(), 0,
                                            sizeof(PowerThreadArgs));
        if (targs == NULL) {
            EnterCriticalSection(&g_power_cs);
            g_power_active = 0;
            LeaveCriticalSection(&g_power_cs);
            rh_send_err(c->sock, "wire_desync");
            return;
        }
        targs->exit_flags = exit_flags;
        targs->delay_ms   = delay_seconds * 1000UL;

        th = CreateThread(NULL, 0, power_delay_thread, targs, 0, NULL);
        if (th == NULL) {
            HeapFree(GetProcessHeap(), 0, targs);
            EnterCriticalSection(&g_power_cs);
            g_power_active = 0;
            LeaveCriticalSection(&g_power_cs);
            rh_send_err(c->sock, "wire_desync");
            return;
        }
        CloseHandle(th);  /* detach — thread owns itself */

        _snprintf(body, sizeof(body),
                  "{\"phase\":\"requested\",\"grace_ms\":%lu,"
                  "\"deadline_unix\":%I64d}",
                  delay_seconds * 1000UL, (__int64)deadline_unix);
        rh_send_ok_json(c->sock, body);
    } else {
        if (!ExitWindowsEx(exit_flags, reason)) {
            _snprintf(detail, sizeof(detail),
                      "{\"win32_error\":%lu}", GetLastError());
            rh_send_err_json(c->sock, "not_supported", detail);
            return;
        }
        _snprintf(body, sizeof(body),
                  "{\"phase\":\"requested\",\"grace_ms\":0,"
                  "\"deadline_unix\":%I64d}",
                  (__int64)time(NULL));
        rh_send_ok_json(c->sock, body);
    }
}

/* hibernate=TRUE -> hibernate, FALSE -> sleep (suspend-to-RAM). */
static void do_suspend(RhConn* c, BOOL hibernate)
{
    HMODULE powrprof;
    typedef BOOL (WINAPI *PFN_SetSuspendState)(BOOL, BOOL, BOOL);
    PFN_SetSuspendState pfn;

    enable_shutdown_privilege();

    powrprof = LoadLibraryA("powrprof.dll");
    pfn      = (PFN_SetSuspendState)(powrprof
                    ? GetProcAddress(powrprof, "SetSuspendState")
                    : NULL);
    if (pfn == NULL) {
        if (powrprof != NULL) FreeLibrary(powrprof);
        rh_send_err(c->sock, "not_supported");
        return;
    }
    if (!pfn(hibernate, FALSE, FALSE)) {
        FreeLibrary(powrprof);
        rh_send_err(c->sock, "not_supported");
        return;
    }
    FreeLibrary(powrprof);
    rh_send_ok(c->sock);
}

/* ---------------------------------------------------------------------------
 * Public: init */

void rh_power_init(void)
{
    InitializeCriticalSection(&g_power_cs);
    g_power_cancel_evt  = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_power_active      = 0;
    g_power_deadline_ms = 0;
}

/* ---------------------------------------------------------------------------
 * Verb handlers */

void rh_verb_power_blockers(RhConn* c, const RhRequest* req)
{
    (void)req;
    /* ShutdownBlockReasonQuery is Vista+; classic always returns empty list. */
    rh_send_ok_json(c->sock, "{\"blockers\":[]}");
}

void rh_verb_power_lock(RhConn* c, const RhRequest* req)
{
    HMODULE user32;
    typedef BOOL (WINAPI *PFN_LockWorkStation)(void);
    PFN_LockWorkStation pfn;

    (void)req;

    /* LockWorkStation is Win2000+; use GetProcAddress so the binary loads
     * on NT4 / Win9x without a missing-import error. */
    user32 = GetModuleHandleA("user32.dll");
    pfn    = (PFN_LockWorkStation)(user32
                    ? GetProcAddress(user32, "LockWorkStation")
                    : NULL);
    if (pfn == NULL || !pfn()) {
        rh_send_err(c->sock, "not_supported");
        return;
    }
    rh_send_ok(c->sock);
}

void rh_verb_power_reboot(RhConn* c, const RhRequest* req)
{
    do_power(c, req, EWX_REBOOT);
}

void rh_verb_power_shutdown(RhConn* c, const RhRequest* req)
{
    do_power(c, req, EWX_SHUTDOWN);
}

void rh_verb_power_logoff(RhConn* c, const RhRequest* req)
{
    do_power(c, req, EWX_LOGOFF);
}

void rh_verb_power_hibernate(RhConn* c, const RhRequest* req)
{
    (void)req;
    do_suspend(c, TRUE);
}

void rh_verb_power_sleep(RhConn* c, const RhRequest* req)
{
    (void)req;
    do_suspend(c, FALSE);
}

void rh_verb_power_cancel(RhConn* c, const RhRequest* req)
{
    int     was_active;
    __int64 cancelled_ms;
    char    body[80];

    (void)req;

    EnterCriticalSection(&g_power_cs);
    was_active   = g_power_active;
    cancelled_ms = g_power_deadline_ms;
    if (was_active) {
        g_power_active = 0;
        SetEvent(g_power_cancel_evt);
    }
    LeaveCriticalSection(&g_power_cs);

    if (was_active) {
        _snprintf(body, sizeof(body),
                  "{\"cancelled_until_ms\":%I64d}", cancelled_ms);
        rh_send_ok_json(c->sock, body);
    } else {
        rh_send_err_json(c->sock, "not_found",
                         "{\"message\":\"no pending shutdown\"}");
    }
}
