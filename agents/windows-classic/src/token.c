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

/* Bare VC98 wincrypt.h pre-dates these CryptoAPI definitions; supply them
 * when missing.  The #ifndef HCRYPTPROV guard is safe: the PSDK wincrypt.h
 * emits a matching #define sentinel so our copy is skipped when PSDK headers
 * are in play.  Values are stable MSDN constants. */
#ifndef HCRYPTPROV
typedef ULONG_PTR HCRYPTPROV;
#define HCRYPTPROV HCRYPTPROV
#endif
#ifndef PROV_RSA_FULL
#define PROV_RSA_FULL        1
#endif
#ifndef CRYPT_VERIFYCONTEXT
#define CRYPT_VERIFYCONTEXT  0xF0000000UL
#endif

static char g_token_hex[RH_TOKEN_HEX_LEN + 1];
static int  g_token_ready  = RH_FALSE;
static int  g_per_session  = RH_FALSE;

static const char kHex[]        = "0123456789abcdef";
static const char kTokenPath[]  = "C:\\ProgramData\\AgentRemoteHands\\token";

/* CryptoAPI function-pointer typedefs for GetProcAddress binding.
 * VC98's advapi32.lib predates these exports; binding at runtime keeps the
 * static link clean and also handles Win9x without the Crypto Pack (where
 * GetProcAddress returns NULL and we fall through to weak_random). */
typedef BOOL (WINAPI *PFN_CryptAcquireContextA)(HCRYPTPROV*, LPCSTR, LPCSTR,
                                                DWORD, DWORD);
typedef BOOL (WINAPI *PFN_CryptGenRandom)(HCRYPTPROV, DWORD, BYTE*);
typedef BOOL (WINAPI *PFN_CryptReleaseContext)(HCRYPTPROV, DWORD);

/* Fill `out` with `n` cryptographically-random bytes via CryptGenRandom
 * (advapi32, available NT4 SP3+ / Win9x with the crypto pack). Returns 1 on
 * success. */
static int crypto_random(unsigned char* out, int n)
{
    HMODULE                  adv;
    PFN_CryptAcquireContextA pfnAcquire;
    PFN_CryptGenRandom       pfnRandom;
    PFN_CryptReleaseContext  pfnRelease;
    HCRYPTPROV               prov;
    int                      ok;

    adv = GetModuleHandleA("advapi32.dll");
    if (adv == NULL) { return RH_FALSE; }
    pfnAcquire = (PFN_CryptAcquireContextA)GetProcAddress(adv,
                                               "CryptAcquireContextA");
    pfnRandom  = (PFN_CryptGenRandom)GetProcAddress(adv, "CryptGenRandom");
    pfnRelease = (PFN_CryptReleaseContext)GetProcAddress(adv,
                                             "CryptReleaseContext");
    if (!pfnAcquire || !pfnRandom || !pfnRelease) { return RH_FALSE; }

    if (!pfnAcquire(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        return RH_FALSE;
    }
    ok = pfnRandom(prov, (DWORD)n, out) ? RH_TRUE : RH_FALSE;
    pfnRelease(prov, 0);
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

/* Try to read and validate the existing token file.  Returns RH_TRUE and
 * populates g_token_hex on success, RH_FALSE on any failure. */
static int load_token_file(void)
{
    HANDLE h;
    DWORD  n;
    char   buf[RH_TOKEN_HEX_LEN + 2]; /* tolerance for trailing newline */
    int    i;

    h = CreateFileA(kTokenPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { return RH_FALSE; }
    n = 0;
    ReadFile(h, buf, (DWORD)sizeof(buf), &n, NULL);
    CloseHandle(h);
    if (n < (DWORD)RH_TOKEN_HEX_LEN) { return RH_FALSE; }
    for (i = 0; i < RH_TOKEN_HEX_LEN; ++i) {
        char c = buf[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return RH_FALSE;
        }
    }
    memcpy(g_token_hex, buf, RH_TOKEN_HEX_LEN);
    g_token_hex[RH_TOKEN_HEX_LEN] = '\0';
    return RH_TRUE;
}

/* Return the age of the token file in hours, or -1 if the file is absent.
 * Uses FindFirstFile (Win9x+ / NT4+) rather than GetFileAttributesEx
 * (Win2000+) to maintain the full classic target range. */
static __int64 token_file_age_hours(void)
{
    WIN32_FIND_DATAA fd;
    HANDLE           h;
    FILETIME         now;
    ULARGE_INTEGER   li_now;
    ULARGE_INTEGER   li_create;

    h = FindFirstFileA(kTokenPath, &fd);
    if (h == INVALID_HANDLE_VALUE) { return (__int64)-1; }
    FindClose(h);

    GetSystemTimeAsFileTime(&now);
    li_now.LowPart     = now.dwLowDateTime;
    li_now.HighPart    = now.dwHighDateTime;
    li_create.LowPart  = fd.ftCreationTime.dwLowDateTime;
    li_create.HighPart = fd.ftCreationTime.dwHighDateTime;

    if (li_now.QuadPart < li_create.QuadPart) { return (__int64)0; }
    return (__int64)((li_now.QuadPart - li_create.QuadPart)
                     / ((__int64)10000000 * 3600));
}

/* Delete then create the token file so the filesystem creation time is
 * fresh for subsequent TTL checks. Falls back to CREATE_ALWAYS if the
 * initial CREATE_NEW races with another process. */
static void write_token_file(void)
{
    HANDLE h;
    DWORD  written;

    /* Best-effort directory creation; ignore "already exists". On NT4/2000
     * %ProgramData% does not exist -- the per-OS token path + ACL is a W9 /
     * installer concern. The in-memory token still gates tier_raise. */
    CreateDirectoryA("C:\\ProgramData", NULL);
    CreateDirectoryA("C:\\ProgramData\\AgentRemoteHands", NULL);

    /* Delete-then-write preserves an accurate creation timestamp so TTL
     * comparisons remain correct across restarts. */
    DeleteFileA(kTokenPath);

    h = CreateFileA(kTokenPath, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        h = CreateFileA(kTokenPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (h == INVALID_HANDLE_VALUE) { return; }
    WriteFile(h, g_token_hex, (DWORD)RH_TOKEN_HEX_LEN, &written, NULL);
    CloseHandle(h);
}

static void generate_token(void)
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
}

void rh_token_init(int ttl_hours)
{
    __int64 age;

    g_per_session = (ttl_hours == 0) ? RH_TRUE : RH_FALSE;

    if (ttl_hours == -1) {
        /* FOREVER: reuse existing token if valid. */
        if (load_token_file()) {
            g_token_ready = RH_TRUE;
            return;
        }
    } else if (ttl_hours > 0) {
        /* Hourly TTL: reload if file is young enough. */
        age = token_file_age_hours();
        if (age >= 0 && age < (__int64)ttl_hours) {
            if (load_token_file()) {
                g_token_ready = RH_TRUE;
                return;
            }
        }
    }

    /* Per-session (0), expired, or load failed: generate a fresh token. */
    generate_token();
    g_token_ready = RH_TRUE;
    write_token_file();
}

void rh_token_cleanup(void)
{
    if (g_per_session) {
        DeleteFileA(kTokenPath);
    }
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
