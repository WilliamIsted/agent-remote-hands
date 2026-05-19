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

/* socket_watchdog.c -- defensive listen-socket liveness check.
 *
 * A background thread wakes every WATCHDOG_INTERVAL_MS milliseconds and
 * probes the listen socket via getsockopt(SO_ERROR).  If the socket is dead
 * (post-resume, driver reset, etc.) it calls rh_server_rebind() to open a
 * replacement.  Silent on a healthy socket; logs only on problems.
 *
 * C89 / VC98 constraints:
 *   - All variable declarations at the top of each block (before statements).
 *   - No // comments.
 *   - No long long; use __int64 when 64-bit integers are required (none here).
 *   - getsockopt takes char* for the option-value parameter in Winsock 1.1.
 *   - _snprintf, not snprintf. */

#include "socket_watchdog.h"
#include "server.h"
#include "debug.h"

#include <windows.h>
#include <stdio.h>

#define WATCHDOG_INTERVAL_MS 30000

/* Shared state protected by g_watch_cs. */
static SOCKET           g_watch_sock = INVALID_SOCKET;
static CRITICAL_SECTION g_watch_cs;
static u16              g_watch_port = 0;

void rh_watchdog_update_socket(SOCKET s)
{
    EnterCriticalSection(&g_watch_cs);
    g_watch_sock = s;
    LeaveCriticalSection(&g_watch_cs);
}

static DWORD WINAPI watchdog_thread(LPVOID param)
{
    SOCKET s;
    int    err;
    int    len;
    int    dead;

    (void)param;

    for (;;) {
        Sleep(WATCHDOG_INTERVAL_MS);

        /* Snapshot the current socket under the lock so we don't race with
         * rh_watchdog_update_socket() or rh_server_rebind(). */
        EnterCriticalSection(&g_watch_cs);
        s = g_watch_sock;
        LeaveCriticalSection(&g_watch_cs);

        if (s == INVALID_SOCKET) {
            /* Rebind in progress or not yet started; skip this cycle. */
            continue;
        }

        err  = 0;
        len  = sizeof(err);
        dead = 0;

        if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &len)
                == SOCKET_ERROR) {
            dead = 1; /* can't even query the socket */
        } else if (err != 0) {
            dead = 1;
        }

        if (dead) {
#ifdef RH_DEBUG
            rh_dbg("watchdog: listen socket dead (SO_ERROR=%d)"
                   " -- triggering rebind", err);
#endif
            rh_server_rebind(g_watch_port);
        }
        /* silent on success */
    }

    return 0; /* unreachable; suppresses C4715 on MSVC */
}

void rh_watchdog_start(u16 port)
{
    HANDLE th;

    InitializeCriticalSection(&g_watch_cs);
    g_watch_port = port;

    th = CreateThread(NULL, 0, watchdog_thread, NULL, 0, NULL);
    if (th != NULL) {
        CloseHandle(th); /* detach: we never join the watchdog */
    }
}
