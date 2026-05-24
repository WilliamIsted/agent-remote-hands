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
#include "../json.h"
#include "../protocol.h"

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#ifndef WHEEL_DELTA
#define WHEEL_DELTA 120
#endif

/* SendInput / INPUT struct constants -- arrived with the Win98/2000 SDK.
 * VS6's bundled winuser.h predates them; the runtime values are stable so
 * define them defensively the same way screen.c does the virtual-screen
 * metrics. We do not depend on SendInput at link time -- it is
 * GetProcAddress'd at first use so the binary still loads on NT 4 / 9x
 * where the symbol is absent. */
#ifndef INPUT_MOUSE
#define INPUT_MOUSE     0
#endif
#ifndef INPUT_KEYBOARD
#define INPUT_KEYBOARD  1
#endif
#ifndef MOUSEEVENTF_MOVE
#define MOUSEEVENTF_MOVE         0x0001
#endif
#ifndef MOUSEEVENTF_ABSOLUTE
#define MOUSEEVENTF_ABSOLUTE     0x8000
#endif
#ifndef MOUSEEVENTF_VIRTUALDESK
#define MOUSEEVENTF_VIRTUALDESK  0x4000
#endif
#ifndef MOUSEEVENTF_XDOWN
#define MOUSEEVENTF_XDOWN        0x0080
#endif
#ifndef MOUSEEVENTF_XUP
#define MOUSEEVENTF_XUP          0x0100
#endif
#ifndef XBUTTON1
#define XBUTTON1 1
#endif
#ifndef XBUTTON2
#define XBUTTON2 2
#endif
#ifndef SM_CXVIRTUALSCREEN
#define SM_CXVIRTUALSCREEN 78
#endif
#ifndef SM_CYVIRTUALSCREEN
#define SM_CYVIRTUALSCREEN 79
#endif

/* INPUT layout per the Platform SDK. We mirror the union by hand so VS6's
 * pre-SendInput winuser.h does not need to define it; the byte layout is
 * documented and stable across every Windows release that exports
 * SendInput. The MOUSEINPUT / KEYBDINPUT / HARDWAREINPUT union is sized
 * by HARDWAREINPUT (the smallest member); pad to MOUSEINPUT (the largest)
 * so we never under-size the struct passed to SendInput. */
typedef struct {
    LONG    dx;
    LONG    dy;
    DWORD   mouseData;
    DWORD   dwFlags;
    DWORD   time;
    ULONG_PTR dwExtraInfo;
} RhMouseInput;

typedef struct {
    WORD    wVk;
    WORD    wScan;
    DWORD   dwFlags;
    DWORD   time;
    ULONG_PTR dwExtraInfo;
} RhKeybdInput;

typedef struct {
    DWORD type;
    union {
        RhMouseInput mi;
        RhKeybdInput ki;
        /* HARDWAREINPUT (uMsg+wParamL+wParamH) is smaller than MOUSEINPUT
         * so it does not affect sizeof(); omitted to keep the union tight. */
    } u;
} RhInput;

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

/* --- shared SendInput / monitor probes (R2 extended input) ------------- */

/* HMONITOR is declared by the Win98+/Win2000+ Platform SDK; VS6's bundled
 * winuser.h pre-dates it. We only ever pass the handle through (the OS owns
 * the lifetime), so a void* alias is sufficient. */
typedef void* RhHMonitor;

/* user32!SendInput is XP+ / Win 2000+. On NT 4 / 9x the symbol is absent and
 * we fall through to mouse_event / keybd_event (deprecated but functional on
 * those OS versions). The probe runs at most once per process. */
typedef UINT (WINAPI *rh_send_input_fn)(UINT, RhInput*, int);
typedef RhHMonitor (WINAPI *rh_monitor_from_point_fn)(POINT, DWORD);
/* MONITORENUMPROC is in the Win98+/Win2000+ Platform SDK too; spell the
 * callback signature out so the declaration compiles regardless. */
typedef BOOL (CALLBACK *rh_mon_enum_proc)(RhHMonitor, HDC, LPRECT, LPARAM);
typedef BOOL (WINAPI *rh_enum_display_monitors_fn)(HDC, LPCRECT,
                                                    rh_mon_enum_proc,
                                                    LPARAM);

static rh_send_input_fn            g_fnSendInput            = NULL;
static rh_monitor_from_point_fn    g_fnMonitorFromPoint     = NULL;
static rh_enum_display_monitors_fn g_fnEnumDisplayMonitors  = NULL;
static int                         g_input_api_probed       = 0;

static void probe_input_api(void)
{
    HMODULE h;

    if (g_input_api_probed) {
        return;
    }
    g_input_api_probed = 1;

    h = GetModuleHandleA("user32.dll");
    if (h == NULL) {
        return;
    }
    g_fnSendInput =
        (rh_send_input_fn)GetProcAddress(h, "SendInput");
    g_fnMonitorFromPoint =
        (rh_monitor_from_point_fn)GetProcAddress(h, "MonitorFromPoint");
    g_fnEnumDisplayMonitors =
        (rh_enum_display_monitors_fn)GetProcAddress(h, "EnumDisplayMonitors");
}

#ifndef MONITOR_DEFAULTTONEAREST
#define MONITOR_DEFAULTTONEAREST 0x00000002
#endif

/* Common send path: one INPUT_MOUSE event. If `abs_pos` is non-NULL the
 * event carries an absolute virtual-desktop coordinate (rescaled to the
 * 0..65535 grid SendInput requires); otherwise it is a flag-only event
 * (button down/up at the live cursor position). On the keybd_event /
 * mouse_event fallback path the cursor is moved via SetCursorPos first
 * since mouse_event cannot carry virtual-desktop absolute coords. */
static void rh_classic_send_mouse_event(DWORD flags, DWORD mouse_data,
                                        POINT* abs_pos)
{
    RhInput in;
    int     sx;
    int     sy;

    probe_input_api();

    if (g_fnSendInput != NULL) {
        memset(&in, 0, sizeof(in));
        in.type = INPUT_MOUSE;
        in.u.mi.mouseData = mouse_data;
        if (abs_pos != NULL) {
            sx = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            sy = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            if (sx <= 0) sx = GetSystemMetrics(SM_CXSCREEN);
            if (sy <= 0) sy = GetSystemMetrics(SM_CYSCREEN);
            if (sx <= 0) sx = 1;
            if (sy <= 0) sy = 1;
            in.u.mi.dx = (LONG)(((long)abs_pos->x * 65535L) / (long)sx);
            in.u.mi.dy = (LONG)(((long)abs_pos->y * 65535L) / (long)sy);
            in.u.mi.dwFlags = flags
                            | MOUSEEVENTF_ABSOLUTE
                            | MOUSEEVENTF_VIRTUALDESK;
        } else {
            in.u.mi.dwFlags = flags;
        }
        g_fnSendInput(1, &in, (int)sizeof(RhInput));
        return;
    }

    /* NT 4 / 9x fallback: mouse_event takes relative deltas + the same flag
     * set, but cannot describe a virtual-desktop absolute. Move the cursor
     * first when an absolute position was requested. */
    if (abs_pos != NULL) {
        SetCursorPos(abs_pos->x, abs_pos->y);
    }
    mouse_event(flags, 0, 0, mouse_data, 0);
}

/* Single keystroke (down or up). Same SendInput-preferred / keybd_event
 * fallback shape as the mouse variant. */
static void rh_classic_send_key(int vk, int down)
{
    RhInput in;

    probe_input_api();

    if (g_fnSendInput != NULL) {
        memset(&in, 0, sizeof(in));
        in.type = INPUT_KEYBOARD;
        in.u.ki.wVk = (WORD)vk;
        in.u.ki.dwFlags = down ? (DWORD)0 : (DWORD)KEYEVENTF_KEYUP;
        g_fnSendInput(1, &in, (int)sizeof(RhInput));
        return;
    }
    keybd_event((BYTE)vk, 0, down ? (DWORD)0 : (DWORD)KEYEVENTF_KEYUP, 0);
}

/* Map a button name to the MOUSEEVENTF_*DOWN flag. Returns 0 on unknown. */
static DWORD mouse_down_flag(const char* btn)
{
    if (ci_eq(btn, "left"))   return MOUSEEVENTF_LEFTDOWN;
    if (ci_eq(btn, "right"))  return MOUSEEVENTF_RIGHTDOWN;
    if (ci_eq(btn, "middle")) return MOUSEEVENTF_MIDDLEDOWN;
    if (ci_eq(btn, "x1"))     return MOUSEEVENTF_XDOWN;
    if (ci_eq(btn, "x2"))     return MOUSEEVENTF_XDOWN;
    return 0;
}

/* Bitwise relationship: *UP == *DOWN << 1 for LEFT/RIGHT/MIDDLE; for the
 * X buttons MOUSEEVENTF_XUP == MOUSEEVENTF_XDOWN << 1 also holds (0x80
 * -> 0x100). Express it that way so we don't carry a parallel up-flag
 * lookup table. */
static DWORD mouse_up_flag(const char* btn)
{
    DWORD d = mouse_down_flag(btn);
    if (d == 0) {
        return 0;
    }
    return d << 1;
}

static DWORD mouse_data_for(const char* btn)
{
    if (ci_eq(btn, "x1")) return XBUTTON1;
    if (ci_eq(btn, "x2")) return XBUTTON2;
    return 0;
}

/* Modifier name -> VK. Mirrors the parser in rh_verb_input_key. */
static int modifier_to_vk(const char* name)
{
    if (ci_eq(name, "ctrl") || ci_eq(name, "control")) return VK_CONTROL;
    if (ci_eq(name, "shift"))                          return VK_SHIFT;
    if (ci_eq(name, "alt"))                            return VK_MENU;
    if (ci_eq(name, "win"))                            return VK_LWIN;
    return 0;
}

/* Resolve the "vk" argument for input.keyboard.key_down / key_up: the test
 * suite (test_rebuild_v030.py / test_input_keyboard.py) passes the key
 * identifier either as the leading positional ("F24") or via `--vk <name>`.
 * Modifier names (shift/ctrl/alt/win) are accepted here too -- the rebuild
 * test calls `--vk shift` which is not in the key_to_vk table. Returns the
 * VK code, or 0 if no recognised identifier is present. */
static int resolve_vk_arg(const RhArgs* a, const char* flag_val,
                          const char** out_name)
{
    const char* name = NULL;
    int         vk;
    int         dummy;

    if (flag_val != NULL && flag_val[0] != '\0') {
        name = flag_val;
    } else if (a->npos >= 1) {
        name = a->pos[0];
    }
    *out_name = name;
    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    vk = modifier_to_vk(name);
    if (vk != 0) {
        return vk;
    }
    vk = key_to_vk(name, &dummy);
    if (vk < 0) {
        return 0;
    }
    return vk;
}

/* EnumDisplayMonitors callback context for rh_classic_pt_to_monitor. */
typedef struct {
    RhHMonitor target;
    int        found;   /* -1 until target is matched */
    int        seen;
} MonEnumCtx;

static BOOL CALLBACK mon_enum_cb(RhHMonitor h, HDC hdc,
                                 LPRECT rc, LPARAM lp)
{
    MonEnumCtx* ctx = (MonEnumCtx*)lp;
    (void)hdc;
    (void)rc;
    if (h == ctx->target && ctx->found < 0) {
        ctx->found = ctx->seen;
    }
    ctx->seen += 1;
    return TRUE;
}

/* Returns the 0-based EnumDisplayMonitors index of the monitor containing
 * `pt`, matching system.info.screens[].index. Falls back to 0 on NT 4 /
 * single-monitor systems where MonitorFromPoint / EnumDisplayMonitors are
 * absent at runtime. */
static int rh_classic_pt_to_monitor(POINT pt)
{
    RhHMonitor target;
    MonEnumCtx ctx;

    probe_input_api();
    if (g_fnMonitorFromPoint == NULL || g_fnEnumDisplayMonitors == NULL) {
        return 0;
    }
    target = g_fnMonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (target == NULL) {
        return 0;
    }
    ctx.target = target;
    ctx.found  = -1;
    ctx.seen   = 0;
    g_fnEnumDisplayMonitors(NULL, NULL, mon_enum_cb, (LPARAM)&ctx);
    return (ctx.found >= 0) ? ctx.found : 0;
}

/* --- input.position (R; new in v2.2 on classic) ------------------------ */

void rh_verb_input_position(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = {
        { "--include-monitor", 0 }
    };
    RhArgs  a;
    POINT   pt;
    RhJson* j;
    const char* body;
    int     include_monitor;

    rh_args_parse(req, defs, 1, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    include_monitor = a.seen[0] ? 1 : 0;

    if (!GetCursorPos(&pt)) {
        rh_err_msg(c, "permission_denied", "GetCursorPos failed");
        return;
    }

    j = (RhJson*)malloc(sizeof(RhJson));
    if (j == NULL) {
        rh_err(c, "wire_desync");   /* malloc fail; matches every other handler */
        return;
    }
    rh_json_init(j);
    rh_json_begin_obj(j);
    rh_json_key(j, "x");
    rh_json_int(j, (i32)pt.x);
    rh_json_key(j, "y");
    rh_json_int(j, (i32)pt.y);
    if (include_monitor) {
        rh_json_key(j, "monitor_index");
        rh_json_int(j, (i32)rh_classic_pt_to_monitor(pt));
    }
    rh_json_end_obj(j);
    body = rh_json_finish(j);
    if (body == NULL) {
        free(j);
        rh_err(c, "wire_desync");   /* JSON-finish fail; per R2 review */
        return;
    }
    rh_ok_json(c, body);
    free(j);
}

/* --- input.mouse.press / release (U; new in v2.2 on classic) ---------- */

/* Shared parser for press + release. Accepts --button (default "left"),
 * --x, --y (must be both-or-neither). Returns 1 on success; on failure
 * emits the wire error and returns 0. */
static int parse_press_release_args(RhConn* c, const RhRequest* req,
                                    const char** btn_out,
                                    int* has_xy_out, POINT* xy_out)
{
    static const RhFlagDef defs[] = {
        { "--button", 1 },
        { "--x",      1 },
        { "--y",      1 }
    };
    RhArgs a;

    rh_args_parse(req, defs, 3, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return 0;
    }
    if (a.missing_val) {
        rh_err_msg(c, "invalid_args", "flag requires a value");
        return 0;
    }
    *btn_out = (a.val[0] != NULL) ? a.val[0] : "left";
    if (a.seen[1] != a.seen[2]) {
        rh_err_msg(c, "invalid_args", "--x and --y are both-or-neither");
        return 0;
    }
    if (a.seen[1]) {
        if (a.val[1] == NULL || a.val[2] == NULL) {
            rh_err_msg(c, "invalid_args", "missing --x / --y value");
            return 0;
        }
        xy_out->x = (LONG)atoi(a.val[1]);
        xy_out->y = (LONG)atoi(a.val[2]);
        *has_xy_out = 1;
    } else {
        xy_out->x = 0;
        xy_out->y = 0;
        *has_xy_out = 0;
    }
    return 1;
}

/* Emit {actual_position: {x, y}} OK body. Matches modern/legacy semantics
 * for press/release/drag per R2 review (cross-family consistency). */
static void rh_classic_emit_actual_position(RhConn* c)
{
    POINT   after;
    RhJson* j;
    char*   body;

    GetCursorPos(&after);
    j = (RhJson*)malloc(sizeof(RhJson));
    if (j == NULL) {
        rh_err(c, "wire_desync");
        return;
    }
    rh_json_init(j);
    rh_json_begin_obj(j);
    rh_json_key(j, "actual_position");
    rh_json_begin_obj(j);
    rh_json_key(j, "x");
    rh_json_int(j, (i32)after.x);
    rh_json_key(j, "y");
    rh_json_int(j, (i32)after.y);
    rh_json_end_obj(j);
    rh_json_end_obj(j);
    body = rh_json_finish(j);
    if (body == NULL) {
        free(j);
        rh_err(c, "wire_desync");
        return;
    }
    rh_ok_json(c, body);
    free(j);
}

void rh_verb_input_mouse_press(RhConn* c, const RhRequest* req)
{
    const char* btn = "left";
    int         has_xy = 0;
    POINT       xy;
    DWORD       f;
    DWORD       dat;

    xy.x = 0;
    xy.y = 0;

    if (!parse_press_release_args(c, req, &btn, &has_xy, &xy)) {
        return;
    }
    f = mouse_down_flag(btn);
    if (f == 0) {
        rh_err_kv(c, "invalid_args", "button", btn);
        return;
    }
    dat = mouse_data_for(btn);

    if (has_xy) {
        rh_classic_send_mouse_event(MOUSEEVENTF_MOVE, 0, &xy);
        rh_classic_send_mouse_event(f, dat, &xy);
    } else {
        rh_classic_send_mouse_event(f, dat, NULL);
    }
    rh_classic_emit_actual_position(c);
}

void rh_verb_input_mouse_release(RhConn* c, const RhRequest* req)
{
    const char* btn = "left";
    int         has_xy = 0;
    POINT       xy;
    DWORD       f;
    DWORD       dat;

    xy.x = 0;
    xy.y = 0;

    if (!parse_press_release_args(c, req, &btn, &has_xy, &xy)) {
        return;
    }
    f = mouse_up_flag(btn);
    if (f == 0) {
        rh_err_kv(c, "invalid_args", "button", btn);
        return;
    }
    dat = mouse_data_for(btn);

    if (has_xy) {
        rh_classic_send_mouse_event(MOUSEEVENTF_MOVE, 0, &xy);
        rh_classic_send_mouse_event(f, dat, &xy);
    } else {
        rh_classic_send_mouse_event(f, dat, NULL);
    }
    rh_classic_emit_actual_position(c);
}

/* --- input.mouse.drag (U; new in v2.2 on classic) ---------------------- */

void rh_verb_input_mouse_drag(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = {
        { "--button",      1 },
        { "--x",           1 },
        { "--y",           1 },
        { "--steps",       1 },
        { "--duration-ms", 1 }
    };
    RhArgs      a;
    const char* btn;
    DWORD       f_down;
    DWORD       f_up;
    DWORD       dat;
    POINT       start;
    POINT       step;
    int         end_x;
    int         end_y;
    int         steps;
    int         duration_ms;
    int         step_delay_ms;
    int         i;

    rh_args_parse(req, defs, 5, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.missing_val) {
        rh_err_msg(c, "invalid_args", "flag requires a value");
        return;
    }

    /* Required: --x and --y (the drag's target). Schema treats start as the
     * live cursor position; this matches the modern/legacy semantics in
     * agents/shared/verbs/input.cpp (mouse_drag). */
    if (!a.seen[1] || !a.seen[2] ||
        a.val[1] == NULL || a.val[2] == NULL) {
        rh_err_msg(c, "invalid_args", "input.mouse.drag requires --x and --y");
        return;
    }
    end_x = atoi(a.val[1]);
    end_y = atoi(a.val[2]);

    btn = (a.val[0] != NULL) ? a.val[0] : "left";
    f_down = mouse_down_flag(btn);
    f_up   = mouse_up_flag(btn);
    if (f_down == 0) {
        rh_err_kv(c, "invalid_args", "button", btn);
        return;
    }
    dat = mouse_data_for(btn);

    steps = (a.seen[3] && a.val[3] != NULL) ? atoi(a.val[3]) : 16;
    if (steps < 2) {
        steps = 2;
    }
    duration_ms = (a.seen[4] && a.val[4] != NULL) ? atoi(a.val[4]) : 200;
    if (duration_ms < 0) {
        duration_ms = 0;
    }
    step_delay_ms = (duration_ms > 0) ? (duration_ms / steps) : 0;

    /* Start at the live cursor position; press, walk, release. */
    GetCursorPos(&start);
    rh_classic_send_mouse_event(f_down, dat, &start);

    for (i = 1; i <= steps; ++i) {
        step.x = start.x + (((LONG)end_x - start.x) * (LONG)i) / (LONG)steps;
        step.y = start.y + (((LONG)end_y - start.y) * (LONG)i) / (LONG)steps;
        rh_classic_send_mouse_event(MOUSEEVENTF_MOVE, 0, &step);
        if (step_delay_ms > 0) {
            Sleep((DWORD)step_delay_ms);
        }
    }

    step.x = (LONG)end_x;
    step.y = (LONG)end_y;
    rh_classic_send_mouse_event(f_up, dat, &step);

    rh_classic_emit_actual_position(c);
}

/* --- input.keyboard.key_down / key_up (U; new in v2.2 on classic) ----- */

/* Parse comma-separated --modifiers value into VK codes. Returns count
 * via *nmods (capped at 8). Emits the wire error and returns 0 on an
 * unknown modifier name; returns 1 on success (including no modifiers). */
static int parse_modifiers_arg(RhConn* c, const char* val,
                               int* mods, int* nmods)
{
    const char* p;
    char        tok[16];
    int         ti = 0;
    int         n  = 0;

    if (val == NULL || val[0] == '\0') {
        *nmods = 0;
        return 1;
    }
    p = val;
    for (;;) {
        char ch = *p;
        if (ch == ',' || ch == '\0') {
            tok[ti] = '\0';
            if (ti > 0) {
                int mvk = modifier_to_vk(tok);
                if (mvk == 0) {
                    rh_err_kv(c, "invalid_args", "unknown_modifier", tok);
                    return 0;
                }
                if (n < 8) {
                    mods[n++] = mvk;
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
    *nmods = n;
    return 1;
}

void rh_verb_input_keyboard_key_down(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = {
        { "--vk",        1 },
        { "--modifiers", 1 }
    };
    RhArgs      a;
    int         vk;
    const char* name;
    int         mods[8];
    int         nmods = 0;
    int         i;

    rh_args_parse(req, defs, 2, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.missing_val) {
        rh_err_msg(c, "invalid_args", "flag requires a value");
        return;
    }
    vk = resolve_vk_arg(&a, a.val[0], &name);
    if (vk == 0) {
        if (name == NULL || name[0] == '\0') {
            rh_err_msg(c, "invalid_args",
                       "input.keyboard.key_down requires 'vk' (a key name)");
        } else {
            rh_err_kv(c, "invalid_args", "vk", name);
        }
        return;
    }
    if (a.seen[1]) {
        if (!parse_modifiers_arg(c, a.val[1], mods, &nmods)) {
            return;
        }
    }

    /* Modifier downs first, in conventional order, then the main key. */
    for (i = 0; i < nmods; ++i) {
        rh_classic_send_key(mods[i], 1);
    }
    rh_classic_send_key(vk, 1);
    rh_ok(c);   /* x-output-schema: null -> bare OK 0 */
}

void rh_verb_input_keyboard_key_up(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = {
        { "--vk",        1 },
        { "--modifiers", 1 }
    };
    RhArgs      a;
    int         vk;
    const char* name;
    int         mods[8];
    int         nmods = 0;
    int         i;

    rh_args_parse(req, defs, 2, &a);
    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.missing_val) {
        rh_err_msg(c, "invalid_args", "flag requires a value");
        return;
    }
    vk = resolve_vk_arg(&a, a.val[0], &name);
    if (vk == 0) {
        if (name == NULL || name[0] == '\0') {
            rh_err_msg(c, "invalid_args",
                       "input.keyboard.key_up requires 'vk' (a key name)");
        } else {
            rh_err_kv(c, "invalid_args", "vk", name);
        }
        return;
    }
    if (a.seen[1]) {
        if (!parse_modifiers_arg(c, a.val[1], mods, &nmods)) {
            return;
        }
    }

    /* Release the main key first, then the modifiers in reverse down-order
     * (matches the modern semantics in agents/shared/verbs/input.cpp). The
     * verb is idempotent -- releasing a key that was never held is a no-op
     * at the OS layer and still returns OK (test_input_keyboard.py
     * key_up_idempotent_when_not_held). */
    rh_classic_send_key(vk, 0);
    for (i = nmods - 1; i >= 0; --i) {
        rh_classic_send_key(mods[i], 0);
    }
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
