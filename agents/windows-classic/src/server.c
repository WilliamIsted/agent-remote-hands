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

#include "server.h"
#include "connection.h"
#include "token.h"
#include "debug.h"
#include "socket_watchdog.h"
#include "power_watcher.h"
#include "verbs/system_power.h"

#include <stdio.h>
#include <string.h>

#include <winsock.h>

/* -------------------------------------------------------------------------
 * Global listen-socket state (protected by g_rebind_cs).
 *
 * The accept loop reads g_listen_sock under the lock on each iteration.
 * rh_server_rebind() and rh_server_suspend() write it under the lock.
 * The g_is_rebinding flag prevents two threads from racing into the rebind
 * path simultaneously. */

static SOCKET           g_listen_sock  = INVALID_SOCKET;
static u16              g_listen_port  = 0;
static CRITICAL_SECTION g_rebind_cs;
static int              g_is_rebinding = 0;

/* -------------------------------------------------------------------------
 * Print every IPv4 address the machine answers on, each with the port.
 * gethostname + gethostbyname is the Winsock 1.1 way to enumerate local
 * addresses (GetAdaptersInfo is Winsock 2 / Win2000+). Falls back to
 * 0.0.0.0 if resolution fails so the port is always visible. */

static void print_addresses(u16 port)
{
    char            host[256];
    struct hostent* he;
    struct in_addr  a;
    int             i;

    fprintf(stderr, "rha-win.classic.x86 v0.3.0\n");

    if (gethostname(host, sizeof(host)) != 0) {
        fprintf(stderr, "  listening on 0.0.0.0:%u\n", (unsigned)port);
        return;
    }
    he = gethostbyname(host);
    if (he == NULL || he->h_addr_list == NULL
            || he->h_addr_list[0] == NULL) {
        fprintf(stderr, "  listening on 0.0.0.0:%u\n", (unsigned)port);
        return;
    }
    for (i = 0; he->h_addr_list[i] != NULL; ++i) {
        memcpy(&a, he->h_addr_list[i], sizeof(a));
        fprintf(stderr, "  listening on %s:%u\n",
                inet_ntoa(a), (unsigned)port);
    }
}

/* -------------------------------------------------------------------------
 * open_listen_socket() -- create, configure, bind and listen on port.
 * Returns a valid SOCKET on success, INVALID_SOCKET on any failure.
 * Used by both rh_server_run() (initial open) and rh_server_rebind()
 * (replacement after suspend/resume). */

static SOCKET open_listen_socket(u16 port)
{
    SOCKET             s;
    struct sockaddr_in addr;
    int                reuse;

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }

    reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&reuse, sizeof(reuse));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR ||
        listen(s, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

/* -------------------------------------------------------------------------
 * rh_server_suspend() -- called on PBT_APMSUSPEND.
 * Closes the listen socket cleanly before the OS tears it down.  The accept
 * loop will see WSAEINVAL / WSAENOTSOCK and wait for the rebind on resume. */

void rh_server_suspend(void)
{
    EnterCriticalSection(&g_rebind_cs);
    if (g_listen_sock != INVALID_SOCKET) {
        closesocket(g_listen_sock);
        g_listen_sock = INVALID_SOCKET;
    }
    LeaveCriticalSection(&g_rebind_cs);
}

/* -------------------------------------------------------------------------
 * rh_server_rebind() -- open a new listen socket on port.
 * Safe to call from any thread (watchdog, power watcher, etc.).
 * No-ops immediately if a rebind is already in progress. */

void rh_server_rebind(u16 port)
{
    SOCKET new_sock;
    int    attempt;

    /* Check-and-set g_is_rebinding under the lock to prevent two threads
     * from entering the rebind sequence simultaneously. */
    EnterCriticalSection(&g_rebind_cs);
    if (g_is_rebinding) {
        LeaveCriticalSection(&g_rebind_cs);
        return;
    }
    g_is_rebinding = 1;
    LeaveCriticalSection(&g_rebind_cs);

    /* Retry loop: the network stack may not be fully re-initialised when
     * PBT_APMRESUMEAUTOMATIC fires; WSAENETDOWN / WSAEADDRINUSE are
     * expected on the first few attempts (max 10 s, 500 ms apart). */
    new_sock = INVALID_SOCKET;
    for (attempt = 1; attempt <= 20; ++attempt) {
#ifdef RH_DEBUG
        rh_dbg("rebind: binding on port %u (attempt %d)",
               (unsigned)port, attempt);
#endif
        new_sock = open_listen_socket(port);
        if (new_sock != INVALID_SOCKET) {
            break;
        }
#ifdef RH_DEBUG
        rh_dbg("rebind: bind failed (%d), retrying in 500ms",
               WSAGetLastError());
#endif
        Sleep(500);
    }

    EnterCriticalSection(&g_rebind_cs);
    if (new_sock != INVALID_SOCKET) {
        /* Close any stale socket that was left over from suspend. */
        if (g_listen_sock != INVALID_SOCKET) {
            closesocket(g_listen_sock);
        }
        g_listen_sock = new_sock;
        rh_watchdog_update_socket(new_sock);
#ifdef RH_DEBUG
        rh_dbg("rebind: listen socket rebound OK");
#endif
    } else {
        /* Use fprintf so this message survives release builds.
         * Repeated watchdog triggering will log every 30 s until resolved. */
        fprintf(stderr, "rebind: FAILED after 20 attempts, last error: %d\n",
                WSAGetLastError());
    }
    g_is_rebinding = 0;
    LeaveCriticalSection(&g_rebind_cs);
}

/* -------------------------------------------------------------------------
 * rh_server_run() -- WSAStartup, bind, start background threads, accept loop.
 *
 * Single-threaded: one connection is served to completion before the next is
 * accepted.  rh_connection_run() owns and closes the accepted socket. */

int rh_server_run(u16 port, int token_ttl_hours)
{
    WSADATA wsd;
    SOCKET  client;
    SOCKET  s;
    int     err;

    /* Winsock 1.1: present across the whole classic target range
     * (NT4 SP6a / Win95 OSR2 .. Win2000). Winsock 2 is not guaranteed on
     * Win95, so the classic agent deliberately stays on the 1.1 API. */
    if (WSAStartup(MAKEWORD(1, 1), &wsd) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

#ifdef RH_DEBUG
    rh_debug_open();
#endif

    rh_power_init();
    rh_token_init(token_ttl_hours);
    InitializeCriticalSection(&g_rebind_cs);

    g_listen_port = port;
    g_listen_sock = open_listen_socket(port);
    if (g_listen_sock == INVALID_SOCKET) {
        fprintf(stderr, "bind() failed on port %u\n", (unsigned)port);
        WSACleanup();
        return 1;
    }

    print_addresses(port);

    /* Start the socket watchdog and power-broadcast watcher. */
    rh_watchdog_start(port);
    rh_watchdog_update_socket(g_listen_sock);
    rh_power_watcher_start(port);

    /* Accept loop: snapshot g_listen_sock under the lock on each iteration
     * so we always call accept() on the current (possibly replaced) socket. */
    for (;;) {
        EnterCriticalSection(&g_rebind_cs);
        s = g_listen_sock;
        LeaveCriticalSection(&g_rebind_cs);

        if (s == INVALID_SOCKET) {
            /* Socket closed for suspend / rebind in progress — wait. */
            Sleep(100);
            continue;
        }

        client = accept(s, NULL, NULL);
        if (client == INVALID_SOCKET) {
            err = WSAGetLastError();
            /* WSAEINVAL / WSAENOTSOCK: the socket was replaced by the rebind
             * path (closesocket() from rh_server_suspend or rh_server_rebind
             * unblocks a blocking accept).  Not a fatal error; loop and pick
             * up the new socket.  All other errors are logged at debug level. */
            if (err != WSAEINVAL && err != WSAENOTSOCK) {
#ifdef RH_DEBUG
                rh_dbg("accept() error %d", err);
#endif
            }
            continue;
        }

        rh_connection_run(client);
    }

    /* The accept loop has no break: rh_server_run never returns normally.
     * MSVC (incl. VS6) suppresses C4715 for a provably-infinite loop, so
     * no trailing return is needed or added. */
}
