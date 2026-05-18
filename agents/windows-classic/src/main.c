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

/* windows-classic agent entry point. C89, MBCS (not Unicode) -- the classic
 * target range predates reliable Unicode console support. */

#include "types.h"
#include "server.h"

#include <windows.h>
#include <stdio.h>

static BOOL WINAPI ctrl_handler(DWORD ctrl_type)
{
    (void)ctrl_type;
    /* The OS reclaims the listening socket and Winsock resources on process
     * exit. Graceful EVENT-drain on shutdown is a W9 concern. */
    ExitProcess(0);
    return TRUE;
}

int main(void)
{
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    fprintf(stderr,
            "rha-win.classic.x86 v0.3.0 listening on :%d\n",
            RH_PORT_DEFAULT);
    return rh_server_run((u16)RH_PORT_DEFAULT);
}
