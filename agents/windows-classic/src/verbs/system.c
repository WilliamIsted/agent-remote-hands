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
#include "../json.h"
#include "../protocol.h"

#include <windows.h>
#include <string.h>

/* SM_CMONITORS is a Win98/2000-era metric; define defensively for a
 * barebones VC98 winuser.h (W9 build-integration guard, not a logic
 * change -- GetSystemMetrics returns the value at runtime regardless). */
#ifndef SM_CMONITORS
#define SM_CMONITORS 80
#endif

/* system.info (PROTOCOL.md 3.1). The 12 fields the conformance suite marks
 * required (tests/conformance/test_system.py REQUIRED_INFO_FIELDS) are all
 * present; a few cheap spec fields (user, integrity, uiaccess, monitors) are
 * included too. `os` is the family identifier "windows-classic" per 3.1
 * (NOT "Windows" + a separate "family" key as the W8 prompt sketched). */
void rh_verb_system_info(RhConn* c, const RhRequest* req)
{
    RhJson      j;
    char        host[MAX_COMPUTERNAME_LENGTH + 1];
    char        user[256];
    DWORD       host_len;
    DWORD       user_len;
    const char* tname;
    const char* result;

    (void)req;

    host_len = (DWORD)sizeof(host);
    if (!GetComputerNameA(host, &host_len)) {
        strcpy(host, "unknown");
    }
    user_len = (DWORD)sizeof(user);
    if (!GetUserNameA(user, &user_len)) {
        strcpy(user, "unknown");
    }
    tname = rh_tier_name(c->tier);
    if (tname == NULL) {
        tname = "read";
    }

    rh_json_init(&j);
    rh_json_begin_obj(&j);

    rh_json_key(&j, "name");            rh_json_str(&j, "agent-remote-hands");
    rh_json_key(&j, "version");         rh_json_str(&j, "0.3.0");
    rh_json_key(&j, "protocol");        rh_json_str(&j, "2.1");
    rh_json_key(&j, "os");              rh_json_str(&j, "windows-classic");
    rh_json_key(&j, "arch");            rh_json_str(&j, "x86");
    rh_json_key(&j, "hostname");        rh_json_str(&j, host);
    rh_json_key(&j, "user");            rh_json_str(&j, user);
    rh_json_key(&j, "integrity");       rh_json_null(&j);
    rh_json_key(&j, "uiaccess");        rh_json_bool(&j, RH_FALSE);
    rh_json_key(&j, "monitors");
    rh_json_int(&j, (i32)GetSystemMetrics(SM_CMONITORS));

    rh_json_key(&j, "tiers");
    rh_json_begin_arr(&j);
    rh_json_arr_str(&j, "read");
    rh_json_arr_str(&j, "create");
    rh_json_arr_str(&j, "update");
    rh_json_arr_str(&j, "delete");
    rh_json_arr_str(&j, "extra_risky");
    rh_json_end_arr(&j);

    rh_json_key(&j, "current_tier");    rh_json_str(&j, tname);

    rh_json_key(&j, "auth");
    rh_json_begin_arr(&j);
    rh_json_arr_str(&j, "token");
    rh_json_end_arr(&j);

    rh_json_key(&j, "max_connections");
    rh_json_int(&j, (i32)RH_MAX_CONNECTIONS);

    rh_json_key(&j, "namespaces");
    rh_json_begin_arr(&j);
    rh_json_arr_str(&j, "system");
    rh_json_arr_str(&j, "connection");
    rh_json_arr_str(&j, "screen");
    rh_json_arr_str(&j, "window");
    rh_json_arr_str(&j, "input");
    rh_json_arr_str(&j, "file");
    rh_json_arr_str(&j, "directory");
    rh_json_arr_str(&j, "process");
    rh_json_arr_str(&j, "registry");
    rh_json_arr_str(&j, "clipboard");
    rh_json_arr_str(&j, "watch");
    rh_json_end_arr(&j);

    /* Sub-capabilities. Classic encodes screen.capture as PNG (a tiny
     * stored-DEFLATE encoder, no GDI+) or BMP -- see verbs/screen.c. No
     * UIA (element.* unsupported, D9); no mDNS unless multicast init
     * succeeds (advertised by src/mdns.c at runtime is out of scope here,
     * so discovery is omitted). */
    rh_json_key(&j, "capabilities");
    rh_json_begin_obj(&j);
    rh_json_key(&j, "image_formats");
    rh_json_begin_arr(&j);
    rh_json_arr_str(&j, "png");
    rh_json_arr_str(&j, "bmp");
    rh_json_end_arr(&j);
    rh_json_end_obj(&j);

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
