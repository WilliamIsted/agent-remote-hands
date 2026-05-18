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

#include "token.h"
#include "types.h"

#include <windows.h>
#include <wincrypt.h>
#include <string.h>

static char  g_token_hex[RH_TOKEN_HEX_LEN + 1];
static int   g_token_ready = RH_FALSE;

static const char kHex[] = "0123456789abcdef";

/* Fill `out` with `n` cryptographically-random bytes via CryptGenRandom
 * (advapi32, available NT4 SP3+ / Win9x with the crypto pack). Returns 1 on
 * success. */
static int crypto_random(unsigned char* out, int n)
{
    HCRYPTPROV prov;
    int        ok;

    if (!CryptAcquireContext(&prov, NULL, NULL, PROV_RSA_FULL,
                             CRYPT_VERIFYCONTEXT)) {
        return RH_FALSE;
    }
    ok = CryptGenRandom(prov, (DWORD)n, out) ? RH_TRUE : RH_FALSE;
    CryptReleaseContext(prov, 0);
    return ok;
}

/* Documented weak fallback when no crypto provider is available (very old
 * Win95 without the crypto pack). Real elevation security is the installer's
 * restrictive ACL on the token directory, not token entropy; and the W8
 * connection/system gate only requires that a wrong token mismatches. */
static void weak_random(unsigned char* out, int n)
{
    unsigned int s;
    int          i;

    s = (unsigned int)GetTickCount()
      ^ ((unsigned int)GetCurrentProcessId() << 16)
      ^ (unsigned int)GetCurrentThreadId();
    for (i = 0; i < n; ++i) {
        s = s * 1103515245u + 12345u;          /* glibc-style LCG */
        out[i] = (unsigned char)((s >> 16) & 0xff);
    }
}

static void write_token_file(void)
{
    HANDLE h;
    DWORD  written;

    /* Best-effort directory creation; ignore "already exists". On NT4/2000
     * %ProgramData% does not exist -- the per-OS token path + ACL is a W9 /
     * installer concern. The in-memory token still gates tier_raise. */
    CreateDirectoryA("C:\\ProgramData", NULL);
    CreateDirectoryA("C:\\ProgramData\\AgentRemoteHands", NULL);

    h = CreateFileA("C:\\ProgramData\\AgentRemoteHands\\token",
                    GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    WriteFile(h, g_token_hex, (DWORD)RH_TOKEN_HEX_LEN, &written, NULL);
    CloseHandle(h);
}

void rh_token_init(void)
{
    unsigned char raw[RH_TOKEN_BYTES];
    int           i;

    if (!crypto_random(raw, RH_TOKEN_BYTES)) {
        weak_random(raw, RH_TOKEN_BYTES);
    }
    for (i = 0; i < RH_TOKEN_BYTES; ++i) {
        g_token_hex[i * 2]     = kHex[(raw[i] >> 4) & 0x0f];
        g_token_hex[i * 2 + 1] = kHex[raw[i] & 0x0f];
    }
    g_token_hex[RH_TOKEN_HEX_LEN] = '\0';
    g_token_ready = RH_TRUE;

    write_token_file();
}

int rh_token_verify(const char* presented)
{
    unsigned int diff;
    int          i;

    if (!g_token_ready || presented == NULL) {
        return RH_FALSE;
    }
    /* Length check is not constant-time, but the token length is public. */
    if ((int)strlen(presented) != RH_TOKEN_HEX_LEN) {
        return RH_FALSE;
    }
    diff = 0;
    for (i = 0; i < RH_TOKEN_HEX_LEN; ++i) {
        diff |= (unsigned int)((unsigned char)presented[i]
                             ^ (unsigned char)g_token_hex[i]);
    }
    return (diff == 0) ? RH_TRUE : RH_FALSE;
}
