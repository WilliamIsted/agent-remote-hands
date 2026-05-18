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

#include <stdio.h>

#include <winsock.h>

int rh_server_run(u16 port)
{
    WSADATA            wsd;
    SOCKET             listen_sock;
    SOCKET             client;
    struct sockaddr_in addr;
    int                reuse;

    /* Winsock 1.1: present across the whole classic target range
     * (NT4 SP6a / Win95 OSR2 .. Win2000). Winsock 2 is not guaranteed on
     * Win95, so the classic agent deliberately stays on the 1.1 API. */
    if (WSAStartup(MAKEWORD(1, 1), &wsd) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    rh_token_init();

    listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) {
        fprintf(stderr, "socket() failed\n");
        WSACleanup();
        return 1;
    }

    reuse = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&reuse, sizeof(reuse));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (bind(listen_sock, (struct sockaddr*)&addr, sizeof(addr))
            == SOCKET_ERROR) {
        fprintf(stderr, "bind() failed on port %u\n", (unsigned)port);
        closesocket(listen_sock);
        WSACleanup();
        return 1;
    }
    if (listen(listen_sock, SOMAXCONN) == SOCKET_ERROR) {
        fprintf(stderr, "listen() failed\n");
        closesocket(listen_sock);
        WSACleanup();
        return 1;
    }

    /* Single-threaded: serve one connection to completion, then the next.
     * rh_connection_run() owns and closes the accepted socket. */
    for (;;) {
        client = accept(listen_sock, NULL, NULL);
        if (client == INVALID_SOCKET) {
            continue;   /* transient accept failure: keep listening */
        }
        rh_connection_run(client);
    }
    /* The accept loop has no break: rh_server_run never returns normally.
     * MSVC (incl. VS6) suppresses C4715 for a provably-infinite loop, so
     * no trailing return is needed or added. */
}
