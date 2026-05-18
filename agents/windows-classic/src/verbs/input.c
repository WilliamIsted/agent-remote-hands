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

#include "input.h"
#include "common.h"
#include "../protocol.h"

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#ifndef WHEEL_DELTA
#define WHEEL_DELTA 120
#endif

/* --- key-name -> virtual-key ------------------------------------------- */

static int ci_eq(const char* a, const char* b)
{
    size_t i;
    for (i = 0; a[i] != '\0' && b[i] != '\0'; ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) {
            return 0;
        }
    }
    return a[i] == '\0' && b[i] == '\0';
}

/* Returns VK code, or -1 if unknown. Sets *need_shift for printable
 * characters that require shift (via VkKeyScanA). */
static int key_to_vk(const char* name, int* need_shift)
{
    *need_shift = 0;

    if (name == NULL || name[0] == '\0') {
        return -1;
    }
    if (ci_eq(name, "enter") || ci_eq(name, "return")) return VK_RETURN;
    if (ci_eq(name, "tab"))                              return VK_TAB;
    if (ci_eq(name, "escape") || ci_eq(name, "esc"))     return VK_ESCAPE;
    if (ci_eq(name, "space"))                            return VK_SPACE;
    if (ci_eq(name, "backspace") || ci_eq(name, "bksp")) return VK_BACK;
    if (ci_eq(name, "delete") || ci_eq(name, "del"))     return VK_DELETE;
    if (ci_eq(name, "insert") || ci_eq(name, "ins"))     return VK_INSERT;
    if (ci_eq(name, "home"))                             return VK_HOME;
    if (ci_eq(name, "end"))                              return VK_END;
    if (ci_eq(name, "pageup"))                           return VK_PRIOR;
    if (ci_eq(name, "pagedown"))                         return VK_NEXT;
    if (ci_eq(name, "up"))                               return VK_UP;
    if (ci_eq(name, "down"))                             return VK_DOWN;
    if (ci_eq(name, "left"))                             return VK_LEFT;
    if (ci_eq(name, "right"))                            return VK_RIGHT;

    if ((name[0] == 'F' || name[0] == 'f') && name[1] != '\0') {
        int n = atoi(name + 1);
        if (n >= 1 && n <= 24) {
            return VK_F1 + (n - 1);
        }
    }

    if (name[1] == '\0') {
        /* Single printable char: ask the active layout. */
        SHORT s = VkKeyScanA(name[0]);
        if (s == -1) {
            return -1;
        }
        if (s & 0x0100) {
            *need_shift = 1;
        }
        return s & 0xff;
    }
    return -1;
}

static void tap_vk(int vk, int with_shift)
{
    if (with_shift) {
        keybd_event(VK_SHIFT, 0, 0, 0);
    }
    keybd_event((BYTE)vk, 0, 0, 0);
    keybd_event((BYTE)vk, 0, KEYEVENTF_KEYUP, 0);
    if (with_shift) {
        keybd_event(VK_SHIFT, 0, KEYEVENTF_KEYUP, 0);
    }
}

/* --- input.click / move / scroll --------------------------------------- */

void rh_verb_input_click(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = { { "--button", 1 } };
    RhArgs      a;
    int         x;
    int         y;
    const char* btn;
    DWORD       down;
    DWORD       up;

    rh_args_parse(req, defs, 1, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 2) {
        rh_err_msg(c, "invalid_args", "input.click requires <x> <y>");
        return;
    }
    x   = atoi(a.pos[0]);
    y   = atoi(a.pos[1]);
    btn = a.val[0] ? a.val[0] : "left";

    if (ci_eq(btn, "right")) {
        down = MOUSEEVENTF_RIGHTDOWN; up = MOUSEEVENTF_RIGHTUP;
    } else if (ci_eq(btn, "middle")) {
        down = MOUSEEVENTF_MIDDLEDOWN; up = MOUSEEVENTF_MIDDLEUP;
    } else if (ci_eq(btn, "left")) {
        down = MOUSEEVENTF_LEFTDOWN; up = MOUSEEVENTF_LEFTUP;
    } else {
        rh_err_msg(c, "invalid_args", "bad --button (left|right|middle)");
        return;
    }

    SetCursorPos(x, y);
    mouse_event(down, 0, 0, 0, 0);
    mouse_event(up, 0, 0, 0, 0);
    rh_ok(c);
}

void rh_verb_input_move(RhConn* c, const RhRequest* req)
{
    RhArgs a;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 2) {
        rh_err_msg(c, "invalid_args", "input.move requires <x> <y>");
        return;
    }
    SetCursorPos(atoi(a.pos[0]), atoi(a.pos[1]));
    rh_ok(c);
}

void rh_verb_input_scroll(RhConn* c, const RhRequest* req)
{
    RhArgs a;
    int    delta;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 3) {
        rh_err_msg(c, "invalid_args",
                   "input.scroll requires <x> <y> <delta>");
        return;
    }
    SetCursorPos(atoi(a.pos[0]), atoi(a.pos[1]));
    delta = atoi(a.pos[2]);
    mouse_event(MOUSEEVENTF_WHEEL, 0, 0,
                (DWORD)(delta * WHEEL_DELTA), 0);
    rh_ok(c);
}

/* --- input.key --------------------------------------------------------- */

void rh_verb_input_key(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = { { "--modifiers", 1 } };
    RhArgs a;
    int    vk;
    int    need_shift;
    int    mods[8];
    int    nmods;
    int    i;

    rh_args_parse(req, defs, 1, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 1) {
        rh_err_msg(c, "invalid_args", "input.key requires <key>");
        return;
    }
    vk = key_to_vk(a.pos[0], &need_shift);
    if (vk < 0) {
        rh_err_kv(c, "invalid_args", "key", a.pos[0]);
        return;
    }

    nmods = 0;
    if (a.val[0] != NULL) {
        const char* p = a.val[0];
        char        tok[16];
        int         ti = 0;
        for (;;) {
            char ch = *p;
            if (ch == ',' || ch == '\0') {
                tok[ti] = '\0';
                if (ti > 0 && nmods < 8) {
                    int mvk = 0;
                    if (ci_eq(tok, "ctrl") || ci_eq(tok, "control")) {
                        mvk = VK_CONTROL;
                    } else if (ci_eq(tok, "shift")) {
                        mvk = VK_SHIFT;
                    } else if (ci_eq(tok, "alt")) {
                        mvk = VK_MENU;
                    } else if (ci_eq(tok, "win")) {
                        mvk = VK_LWIN;
                    }
                    if (mvk != 0) {
                        mods[nmods++] = mvk;
                    }
                }
                ti = 0;
                if (ch == '\0') {
                    break;
                }
            } else if (ti < (int)sizeof(tok) - 1) {
                tok[ti++] = ch;
            }
            ++p;
        }
    }

    for (i = 0; i < nmods; ++i) {
        keybd_event((BYTE)mods[i], 0, 0, 0);
    }
    tap_vk(vk, need_shift);
    for (i = nmods - 1; i >= 0; --i) {
        keybd_event((BYTE)mods[i], 0, KEYEVENTF_KEYUP, 0);
    }
    rh_ok(c);
}

/* --- input.type (length-prefixed UTF-8 payload) ------------------------ */

#define RH_TYPE_MAX (1 * 1024 * 1024)

void rh_verb_input_type(RhConn* c, const RhRequest* req)
{
    RhArgs a;
    int    n;
    char*  buf;
    int    rc;
    int    i;

    rh_args_parse(req, NULL, 0, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.npos < 1) {
        rh_err_msg(c, "invalid_args", "input.type requires <length>");
        return;
    }
    n = atoi(a.pos[0]);
    if (n < 0 || n > RH_TYPE_MAX) {
        rh_err_msg(c, "invalid_args", "bad payload length");
        return;
    }
    if (n == 0) {
        rh_ok(c);
        return;
    }
    buf = (char*)malloc((size_t)n);
    if (buf == NULL) {
        rh_err(c, "wire_desync");
        return;
    }
    rc = rh_read_payload(&c->reader, buf, n);
    if (rc != RH_PROTO_OK) {
        free(buf);
        rh_err(c, "wire_desync");
        return;
    }
    for (i = 0; i < n; ++i) {
        unsigned char ch = (unsigned char)buf[i];
        SHORT         s;
        if (ch == '\n' || ch == '\r') {
            tap_vk(VK_RETURN, 0);
            continue;
        }
        if (ch == '\t') {
            tap_vk(VK_TAB, 0);
            continue;
        }
        s = VkKeyScanA((CHAR)ch);
        if (s == -1) {
            continue;   /* not typeable on this layout: skip */
        }
        tap_vk(s & 0xff, (s & 0x0100) ? 1 : 0);
    }
    free(buf);
    rh_ok(c);
}

/* --- input.send_message / post_message --------------------------------- */

static int parse_msg_args(const RhRequest* req, RhArgs* a,
                          HWND* h, UINT* msg, WPARAM* wp, LPARAM* lp)
{
    rh_args_parse(req, NULL, 0, a);
    if (a->unknown != NULL) {
        return -1;   /* caller emits unknown_flag */
    }
    if (a->npos < 4 || !rh_hwnd_parse(a->pos[0], h)) {
        return 0;
    }
    *msg = (UINT)strtoul(a->pos[1], NULL, 0);
    *wp  = (WPARAM)strtoul(a->pos[2], NULL, 0);
    *lp  = (LPARAM)strtol(a->pos[3], NULL, 0);
    return 1;
}

void rh_verb_input_send_message(RhConn* c, const RhRequest* req)
{
    RhArgs a;
    HWND   h;
    UINT   msg;
    WPARAM wp;
    LPARAM lp;
    int    rc;

    rc = parse_msg_args(req, &a, &h, &msg, &wp, &lp);
    if (rc == -1) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (rc == 0) {
        rh_err_msg(c, "invalid_args",
                   "input.send_message requires <hwnd> <msg> <wparam> <lparam>");
        return;
    }
    if (!IsWindow(h)) {
        rh_err_kv(c, "target_gone", "handle", a.pos[0]);
        return;
    }
    SendMessageA(h, msg, wp, lp);
    rh_ok(c);
}

void rh_verb_input_post_message(RhConn* c, const RhRequest* req)
{
    RhArgs a;
    HWND   h;
    UINT   msg;
    WPARAM wp;
    LPARAM lp;
    int    rc;

    rc = parse_msg_args(req, &a, &h, &msg, &wp, &lp);
    if (rc == -1) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (rc == 0) {
        rh_err_msg(c, "invalid_args",
                   "input.post_message requires <hwnd> <msg> <wparam> <lparam>");
        return;
    }
    if (!IsWindow(h)) {
        rh_err_kv(c, "target_gone", "handle", a.pos[0]);
        return;
    }
    PostMessageA(h, msg, wp, lp);
    rh_ok(c);
}
