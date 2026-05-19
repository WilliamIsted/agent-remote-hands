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

/* power_watcher.c -- WM_POWERBROADCAST hidden-window + message-pump thread.
 *
 * Creates an invisible top-level window (WS_POPUP, no WS_VISIBLE) solely to
 * receive WM_POWERBROADCAST messages.  Using a real desktop window (rather
 * than HWND_MESSAGE) keeps the code compatible with NT4/9x targets where
 * HWND_MESSAGE is absent.
 *
 * On PBT_APMSUSPEND  : calls rh_server_suspend() to close the socket cleanly.
 * On any resume event: calls rh_server_rebind() to open a new socket.
 *
 * C89 / VC98 constraints:
 *   - All variable declarations at the top of each block (before statements).
 *   - No // comments.
 *   - ANSI API throughout (RegisterClassA, CreateWindowExA, etc.).
 *   - _snprintf, not snprintf. */

#include "power_watcher.h"
#include "server.h"
#include "debug.h"

#include <windows.h>
#include <stdio.h>

/* WS_EX_NOACTIVATE is gated on WINVER >= 0x0500 in older SDK headers.
 * The classic build targets NT4 (WINVER=0x0400) so we define it manually.
 * The value is the same on all Windows versions. */
#ifndef WS_EX_NOACTIVATE
#define WS_EX_NOACTIVATE 0x08000000L
#endif

/* APM event codes; define fallbacks for older SDK versions that omit them. */
#ifndef PBT_APMSUSPEND
#define PBT_APMSUSPEND          0x0004
#endif
#ifndef PBT_APMRESUMESUSPEND
#define PBT_APMRESUMESUSPEND    0x0007
#endif
#ifndef PBT_APMRESUMESTANDBY
#define PBT_APMRESUMESTANDBY    0x0008
#endif
#ifndef PBT_APMRESUMECRITICAL
#define PBT_APMRESUMECRITICAL   0x0006
#endif
#ifndef PBT_APMRESUMEAUTOMATIC
#define PBT_APMRESUMEAUTOMATIC  0x0012
#endif

/* Thread ID of the pump thread; used by rh_power_watcher_stop(). */
static DWORD g_pump_tid  = 0;
static u16   g_pwr_port  = 0;

static LRESULT CALLBACK power_wndproc(HWND hwnd, UINT msg,
                                       WPARAM wp, LPARAM lp)
{
    if (msg == WM_POWERBROADCAST) {
        if (wp == (WPARAM)PBT_APMSUSPEND) {
#ifdef RH_DEBUG
            rh_dbg("power: PBT_APMSUSPEND -- closing listen socket");
#endif
            rh_server_suspend();
        } else if (wp == (WPARAM)PBT_APMRESUMEAUTOMATIC ||
                   wp == (WPARAM)PBT_APMRESUMESUSPEND    ||
                   wp == (WPARAM)PBT_APMRESUMESTANDBY    ||
                   wp == (WPARAM)PBT_APMRESUMECRITICAL) {
#ifdef RH_DEBUG
            rh_dbg("power: resume event %lu -- triggering socket rebind",
                   (unsigned long)wp);
#endif
            rh_server_rebind(g_pwr_port);
        }
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static DWORD WINAPI power_pump_thread(LPVOID param)
{
    WNDCLASSA wc;
    HWND      hwnd;
    MSG       msg;
    char      cls[64];

    (void)param;

    /* Unique class name per process to avoid conflicts if two agent
     * instances run simultaneously or a stale registration exists. */
    _snprintf(cls, sizeof(cls), "RhPowerWatcher_%lu",
              (unsigned long)GetCurrentProcessId());
    cls[sizeof(cls) - 1] = '\0'; /* guarantee NUL termination */

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = power_wndproc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = cls;

    if (!RegisterClassA(&wc)) {
#ifdef RH_DEBUG
        rh_dbg("power: RegisterClassA failed (%lu)",
               (unsigned long)GetLastError());
#endif
        return 1;
    }

    /* Invisible window: WS_POPUP without WS_VISIBLE.
     * WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE prevent taskbar/Alt+Tab entries
     * and stop the window from stealing focus. */
    hwnd = CreateWindowExA(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        cls, "",
        WS_POPUP,
        0, 0, 0, 0,
        NULL,                    /* NULL parent = real desktop; safe on NT4/9x */
        NULL,
        GetModuleHandleA(NULL),
        NULL);

    if (hwnd == NULL) {
#ifdef RH_DEBUG
        rh_dbg("power: CreateWindowExA failed (%lu)",
               (unsigned long)GetLastError());
#endif
        UnregisterClassA(cls, GetModuleHandleA(NULL));
        return 1;
    }

    /* Message loop: exits on WM_QUIT (posted by rh_power_watcher_stop). */
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    DestroyWindow(hwnd);
    UnregisterClassA(cls, GetModuleHandleA(NULL));
    return 0;
}

void rh_power_watcher_start(u16 port)
{
    HANDLE th;

    g_pwr_port = port;

    /* Store the thread ID so rh_power_watcher_stop() can post WM_QUIT. */
    th = CreateThread(NULL, 0, power_pump_thread, NULL, 0, &g_pump_tid);
    if (th != NULL) {
        CloseHandle(th); /* detach */
    }
}

void rh_power_watcher_stop(void)
{
    if (g_pump_tid != 0) {
        PostThreadMessageA(g_pump_tid, WM_QUIT, 0, 0);
    }
}
