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
#include "token.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static BOOL WINAPI ctrl_handler(DWORD ctrl_type)
{
    (void)ctrl_type;
    /* Per-session cleanup: delete the token file before the OS reclaims
     * resources so a fresh token is generated on the next start.
     * rh_token_cleanup() is a no-op for non-per-session TTL modes. */
    rh_token_cleanup();
    ExitProcess(0);
    return TRUE;
}

/* Parse a --token-ttl value string.  Accepts "FOREVER" (-1), "0" (per-
 * session), or a positive integer (hours).  Returns the default (720) for
 * any string that does not match a recognised form. */
static int parse_ttl_str(const char* s)
{
    int v;

    if (strcmp(s, "FOREVER") == 0) { return -1; }
    /* Accept only digit strings to avoid atoi("garbage") == 0 being
     * silently treated as per-session. */
    if (s[0] >= '0' && s[0] <= '9') {
        v = atoi(s);
        return v; /* 0 = per-session, >0 = hours */
    }
    return 720; /* unrecognised: fall back to default */
}

int main(int argc, char* argv[])
{
    const char* env_val;
    int         token_ttl    = 720;  /* default: 30 days */
    int         ttl_from_cli = 0;
    int         i;

    /* CLI args: --token-ttl <FOREVER|0|hours> */
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--token-ttl") == 0 && i + 1 < argc) {
            ++i;
            token_ttl    = parse_ttl_str(argv[i]);
            ttl_from_cli = 1;
        }
    }

    /* Env var REMOTE_HANDS_TOKEN_TTL (lower priority than CLI). */
    if (!ttl_from_cli) {
        env_val = getenv("REMOTE_HANDS_TOKEN_TTL");
        if (env_val != NULL) {
            token_ttl = parse_ttl_str(env_val);
        }
    }

    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    return rh_server_run((u16)RH_PORT_DEFAULT, token_ttl);
}
