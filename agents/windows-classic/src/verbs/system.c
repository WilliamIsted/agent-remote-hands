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

#include "system.h"
#include "common.h"
#include "../json.h"
#include "../protocol.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

/* SM_CMONITORS is a Win98/2000-era metric; define defensively for a
 * barebones VC98 winuser.h (W9 build-integration guard, not a logic
 * change -- GetSystemMetrics returns the value at runtime regardless). */
#ifndef SM_CMONITORS
#define SM_CMONITORS 80
#endif

/* --- system.info helpers ---------------------------------------------- */

#ifndef PROCESSOR_ARCHITECTURE_AMD64
#define PROCESSOR_ARCHITECTURE_AMD64 9
#endif
#ifndef PROCESSOR_ARCHITECTURE_ARM
#define PROCESSOR_ARCHITECTURE_ARM 5
#endif
#ifndef PROCESSOR_ARCHITECTURE_ARM64
#define PROCESSOR_ARCHITECTURE_ARM64 12
#endif
#ifndef SPI_GETWHEELSCROLLLINES
#define SPI_GETWHEELSCROLLLINES 104
#endif

typedef VOID (WINAPI *GetNativeSystemInfoFn)(LPSYSTEM_INFO);
typedef PUCHAR (WINAPI *GetSidSubAuthorityCountFn)(PSID);
typedef PDWORD (WINAPI *GetSidSubAuthorityFn)(PSID, DWORD);

/* Read a REG_SZ under HKLM; "" when absent. */
static void reg_hklm_str(const char* key, const char* name,
                         char* out, DWORD cap)
{
    HKEY  k;
    DWORD type;
    DWORD len;

    out[0] = '\0';
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &k) !=
            ERROR_SUCCESS) {
        return;
    }
    len = cap - 1;
    if (RegQueryValueExA(k, name, NULL, &type, (LPBYTE)out, &len) !=
            ERROR_SUCCESS || type != REG_SZ) {
        out[0] = '\0';
    } else {
        out[(len < cap) ? len : cap - 1] = '\0';
    }
    RegCloseKey(k);
}

/* os_name: the registry ProductName ("Windows 2000", "Microsoft Windows
 * 98", ...) plus the service pack, else a name composed from
 * GetVersionEx. os_version: "<major>.<minor>.<build>". */
static void os_strings(char* name, int name_cap, char* ver, int ver_cap)
{
    OSVERSIONINFOA vi;
    char           product[128];
    int            nt;

    memset(&vi, 0, sizeof(vi));
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (!GetVersionExA(&vi)) {
        _snprintf(name, (size_t)name_cap, "Windows");
        _snprintf(ver, (size_t)ver_cap, "unknown");
        name[name_cap - 1] = '\0';
        ver[ver_cap - 1] = '\0';
        return;
    }
    nt = (vi.dwPlatformId == VER_PLATFORM_WIN32_NT) ? 1 : 0;
    reg_hklm_str(nt ? "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion"
                    : "SOFTWARE\\Microsoft\\Windows\\CurrentVersion",
                 "ProductName", product, (DWORD)sizeof(product));
    if (product[0] == '\0') {
        _snprintf(product, sizeof(product), "Windows %s %lu.%lu",
                  nt ? "NT" : "", (unsigned long)vi.dwMajorVersion,
                  (unsigned long)vi.dwMinorVersion);
        product[sizeof(product) - 1] = '\0';
    }
    if (vi.szCSDVersion[0] != '\0') {
        _snprintf(name, (size_t)name_cap, "%s %s", product,
                  vi.szCSDVersion);
    } else {
        _snprintf(name, (size_t)name_cap, "%s", product);
    }
    name[name_cap - 1] = '\0';
    /* 9x packs extra bits into the high word of the build number. */
    _snprintf(ver, (size_t)ver_cap, "%lu.%lu.%lu",
              (unsigned long)vi.dwMajorVersion,
              (unsigned long)vi.dwMinorVersion,
              (unsigned long)(nt ? vi.dwBuildNumber
                                 : (vi.dwBuildNumber & 0xFFFF)));
    ver[ver_cap - 1] = '\0';
}

static const char* cpu_arch(void)
{
    SYSTEM_INFO           si;
    GetNativeSystemInfoFn native;
    HMODULE               k32;

    memset(&si, 0, sizeof(si));
    k32 = GetModuleHandleA("kernel32.dll");
    native = (k32 != NULL)
             ? (GetNativeSystemInfoFn)GetProcAddress(k32,
                                                      "GetNativeSystemInfo")
             : NULL;
    if (native != NULL) {
        native(&si);       /* XP+: the OS arch, not the WOW64 view */
    } else {
        GetSystemInfo(&si);
    }
    switch (si.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: return "x64";
        case PROCESSOR_ARCHITECTURE_ARM64: return "arm64";
        case PROCESSOR_ARCHITECTURE_ARM:   return "arm";
        default:                           return "x86";
    }
}

/* Token integrity level (Vista+). Pre-Vista OSes have no ILs: "none". */
static const char* integrity_level(void)
{
    HANDLE        tok;
    unsigned char buf[64];
    DWORD         len;
    PSID          sid;
    DWORD         rid;
    DWORD         count;
    HMODULE       adv;
    GetSidSubAuthorityCountFn sub_count;
    GetSidSubAuthorityFn      sub_auth;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        return "none";
    }
    /* 25 = TokenIntegrityLevel; the call fails on NT 4 / 2000 / XP. */
    if (!GetTokenInformation(tok, (TOKEN_INFORMATION_CLASS)25, buf,
                             (DWORD)sizeof(buf), &len)) {
        CloseHandle(tok);
        return "none";
    }
    CloseHandle(tok);
    /* Resolved at runtime: only reached on Vista+, and keeps the import
     * table free of SID helpers the 9x advapi32 may not export. */
    adv = GetModuleHandleA("advapi32.dll");
    sub_count = (adv != NULL)
        ? (GetSidSubAuthorityCountFn)GetProcAddress(adv,
                                                    "GetSidSubAuthorityCount")
        : NULL;
    sub_auth = (adv != NULL)
        ? (GetSidSubAuthorityFn)GetProcAddress(adv, "GetSidSubAuthority")
        : NULL;
    if (sub_count == NULL || sub_auth == NULL) {
        return "none";
    }
    sid   = ((SID_AND_ATTRIBUTES*)buf)->Sid;
    count = *sub_count(sid);
    if (count == 0) {
        return "none";
    }
    rid = *sub_auth(sid, count - 1);
    if (rid >= 0x4000) return "system";
    if (rid >= 0x3000) return "high";
    if (rid >= 0x2000) return "medium";
    return "low";
}

static void emit_input_settings(RhJson* j)
{
    int  delay;
    int  speed;
    UINT lines;

    rh_json_begin_obj(j);
    rh_json_key(j, "double_click_time_ms");
    rh_json_int(j, (i32)GetDoubleClickTime());
    rh_json_key(j, "double_click_rect");
    rh_json_begin_obj(j);
    rh_json_key(j, "w"); rh_json_int(j, (i32)GetSystemMetrics(SM_CXDOUBLECLK));
    rh_json_key(j, "h"); rh_json_int(j, (i32)GetSystemMetrics(SM_CYDOUBLECLK));
    rh_json_end_obj(j);
    /* Same mappings as windows-modern: delay 0..3 -> 250 ms steps; speed
     * 0..31 -> ~2.5..30 cps. */
    delay = 1;
    if (SystemParametersInfoA(SPI_GETKEYBOARDDELAY, 0, &delay, 0)) {
        if (delay < 0) delay = 0;
        if (delay > 3) delay = 3;
        rh_json_key(j, "keyboard_repeat_delay_ms");
        rh_json_int(j, (i32)((delay + 1) * 250));
    }
    speed = 31;
    if (SystemParametersInfoA(SPI_GETKEYBOARDSPEED, 0, &speed, 0)) {
        if (speed < 0)  speed = 0;
        if (speed > 31) speed = 31;
        rh_json_key(j, "keyboard_repeat_rate_cps");
        rh_json_int(j, (i32)((25 + (275 * speed) / 31 + 5) / 10));
    }
    /* Absent on pre-wheel hosts; WHEEL_PAGESCROLL can't be expressed. */
    lines = 0;
    if (SystemParametersInfoA(SPI_GETWHEELSCROLLLINES, 0, &lines, 0) &&
        lines != (UINT)-1) {
        rh_json_key(j, "wheel_scroll_lines");
        rh_json_int(j, (i32)lines);
    }
    rh_json_end_obj(j);
}

/* system.info: the v2.1 schema (spec/verbs/system.info.json, strict field
 * set -- additionalProperties:false outside `capabilities`). */
void rh_verb_system_info(RhConn* c, const RhRequest* req)
{
    RhJson      j;
    char        host[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD       host_len;
    char        os_name[192];
    char        os_ver[48];
    const char* integ;
    const char* tname;
    const char* result;
    RhScreen    screens[RH_MAX_SCREENS];
    int         nscreens;
    int         i;

    (void)req;

    host_len = (DWORD)sizeof(host);
    if (!GetComputerNameA(host, &host_len)) {
        strcpy(host, "unknown");
    }
    tname = rh_tier_name(c->tier);
    if (tname == NULL) {
        tname = "read";
    }
    os_strings(os_name, (int)sizeof(os_name), os_ver, (int)sizeof(os_ver));
    integ    = integrity_level();
    nscreens = rh_screens(screens, RH_MAX_SCREENS);

    rh_json_init(&j);
    rh_json_begin_obj(&j);
    rh_json_key(&j, "family");         rh_json_str(&j, "windows-classic");
    rh_json_key(&j, "agent");          rh_json_str(&j, "agent-remote-hands");
    rh_json_key(&j, "agent_protocol"); rh_json_str(&j, "2.1");
    rh_json_key(&j, "os_name");        rh_json_str(&j, os_name);
    rh_json_key(&j, "os_version");     rh_json_str(&j, os_ver);
    rh_json_key(&j, "cpu_arch");       rh_json_str(&j, cpu_arch());
    rh_json_key(&j, "integrity");      rh_json_str(&j, integ);
    rh_json_key(&j, "uiaccess");       rh_json_bool(&j, RH_FALSE);
    rh_json_key(&j, "hostname");       rh_json_str(&j, host);

    rh_json_key(&j, "screens");
    rh_json_begin_arr(&j);
    for (i = 0; i < nscreens; ++i) {
        rh_json_arr_elem(&j);
        rh_json_begin_obj(&j);
        rh_json_key(&j, "index");   rh_json_int(&j, (i32)i);
        rh_json_key(&j, "bounds");  rh_json_bounds(&j, &screens[i].bounds);
        rh_json_key(&j, "primary"); rh_json_bool(&j, screens[i].primary);
        rh_json_end_obj(&j);
    }
    rh_json_end_arr(&j);

    /* Open-ended sub-capability map. Classic encodes screen.capture as PNG
     * (a tiny stored-DEFLATE encoder, no GDI+) or BMP -- see verbs/screen.c.
     * No UIA (element.* unsupported, D9). */
    rh_json_key(&j, "capabilities");
    rh_json_begin_obj(&j);
    rh_json_key(&j, "ui_automation");    rh_json_str(&j, "no");
    rh_json_key(&j, "wgc");              rh_json_str(&j, "no");
    rh_json_key(&j, "gdi");              rh_json_str(&j, "yes");
    rh_json_key(&j, "uipi");
    rh_json_str(&j, (strcmp(integ, "none") == 0) ? "no" : "yes");
    rh_json_key(&j, "integrity_levels");
    rh_json_str(&j, (strcmp(integ, "none") == 0) ? "none" : "limited");
    rh_json_key(&j, "image_formats");
    rh_json_begin_arr(&j);
    rh_json_arr_str(&j, "png");
    rh_json_arr_str(&j, "bmp");
    rh_json_end_arr(&j);
    rh_json_key(&j, "input_settings");
    emit_input_settings(&j);
    rh_json_end_obj(&j);

    rh_json_key(&j, "current_tier");   rh_json_str(&j, tname);
    rh_json_end_obj(&j);

    result = rh_json_finish(&j);
    if (result == NULL) {
        rh_send_err(c->sock, "wire_desync");
        return;
    }
    rh_send_ok_json(c->sock, result);
}

/* system.capabilities (PROTOCOL.md 3.2): verb -> {"tier": "<tier>"}.
 * W8 advertises only the implemented read-tier verbs (the W8 prompt's bare
 * ["system.info","system.capabilities"] array does not match the contract;
 * test_capabilities_advertises_system_info expects caps["system.info"]
 * ["tier"] == "read"). The OS verb surface arrives in W9. */
void rh_verb_system_capabilities(RhConn* c, const RhRequest* req)
{
    RhJson      j;
    const char* result;
    int         i;
    int         n;
    const char* verb;
    const char* tier;

    (void)req;

    /* Built from the dispatch table (rh_verb_table_*) so the advertised
     * surface is exactly what dispatch() implements -- no parallel list to
     * drift out of sync (the divergence the modern build's W4 flagged). */
    n = rh_verb_table_count();

    rh_json_init(&j);
    rh_json_begin_obj(&j);
    for (i = 0; i < n; ++i) {
        rh_verb_table_get(i, &verb, &tier);
        if (verb == NULL || tier == NULL) {
            continue;
        }
        rh_json_key(&j, verb);
        rh_json_begin_obj(&j);
        rh_json_key(&j, "tier");
        rh_json_str(&j, tier);
        rh_json_end_obj(&j);
    }
    rh_json_end_obj(&j);

    result = rh_json_finish(&j);
    if (result == NULL) {
        rh_send_err(c->sock, "wire_desync");
        return;
    }
    rh_send_ok_json(c->sock, result);
}

/* system.health (PROTOCOL.md 4.1): liveness, OK 0. */
void rh_verb_system_health(RhConn* c, const RhRequest* req)
{
    (void)req;
    rh_send_ok(c->sock);
}
