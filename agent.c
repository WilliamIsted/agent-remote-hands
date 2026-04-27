/* vm_agent.c
 *
 * Minimal control agent for the Windows XP build VM.
 * Listens on TCP 8765 (or VM_AGENT_PORT env). Text protocol, one command per
 * line, response always begins "OK" or "ERR". Commands that return data use
 * "OK <length>\n<bytes>".
 *
 * Commands:
 *   SHOT                          - capture screen as 24-bit BMP, stream back
 *   SCREEN                        - returns "OK <width> <height>"
 *   MPOS                          - returns "OK <x> <y>"
 *   READ <path>                   - read file, stream back
 *   WRITE <path> <length>\n<bytes>- write bytes to file
 *   KEY <combo>                   - e.g. "enter", "alt-F4", "ctrl-shift-s"
 *   KEYS <string>                 - type literal text (single line)
 *   KEYDOWN <key>                 - press and hold (for chords with mouse)
 *   KEYUP <key>                   - release a previously-held key
 *   MOVE <x> <y>                  - move cursor (instant)
 *   MOVEREL <dx> <dy>             - move cursor relative
 *   CLICK [left|right|middle|N]   - click at current pos (N = button 1..5)
 *   DCLICK [button]               - double-click
 *   MDOWN [button]                - mouse button down (for drag/hold)
 *   MUP [button]                  - mouse button up
 *   DRAG <x> <y> [button]         - press button at current pos, move to xy, release
 *   WHEEL <delta>                 - vertical wheel (positive up, negative down)
 *   EXEC <cmdline>                - CreateProcess; returns "OK <pid>"
 *   WAIT <pid> [<timeout_ms>]     - wait for process; returns "OK <exit_code>"
 *   SLEEP <ms>                    - server-side sleep
 *   PING                          - returns "OK pong"
 *   QUIT                          - close connection (server stays running)
 *
 * Build: cl /MT /O2 agent.c /link wsock32.lib user32.lib gdi32.lib advapi32.lib
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#pragma comment(lib, "wsock32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

#define DEFAULT_PORT 8765
#define LINEBUF 8192

static int g_port = DEFAULT_PORT;

/* ------------------------------------------------------------------ */
/* Network helpers                                                    */
/* ------------------------------------------------------------------ */

static int recv_line(SOCKET s, char *buf, int max) {
    int n = 0;
    while (n < max - 1) {
        char c;
        int r = recv(s, &c, 1, 0);
        if (r <= 0) return -1;
        if (c == '\n') break;
        if (c == '\r') continue;
        buf[n++] = c;
    }
    buf[n] = 0;
    return n;
}

static int send_all(SOCKET s, const char *buf, int len) {
    while (len > 0) {
        int r = send(s, buf, len, 0);
        if (r <= 0) return -1;
        buf += r;
        len -= r;
    }
    return 0;
}

static int send_str(SOCKET s, const char *str) {
    return send_all(s, str, (int)strlen(str));
}

static int recv_n(SOCKET s, BYTE *buf, int n) {
    while (n > 0) {
        int r = recv(s, (char*)buf, n, 0);
        if (r <= 0) return -1;
        buf += r;
        n -= r;
    }
    return 0;
}

static int send_ok(SOCKET s) { return send_str(s, "OK\n"); }

static int send_err(SOCKET s, const char *msg) {
    char buf[512];
    _snprintf(buf, sizeof(buf), "ERR %s\n", msg);
    return send_str(s, buf);
}

static int send_ok_data(SOCKET s, const BYTE *data, DWORD size) {
    char hdr[64];
    int n = _snprintf(hdr, sizeof(hdr), "OK %lu\n", (unsigned long)size);
    if (send_all(s, hdr, n) < 0) return -1;
    if (size > 0) return send_all(s, (const char*)data, (int)size);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Key name --> VK mapping                                            */
/* ------------------------------------------------------------------ */

static int parse_vk(const char *name) {
    int n;
    char c;
    if (!name || !*name) return -1;
    if (name[0] && !name[1]) {
        c = (char)toupper((unsigned char)name[0]);
        if (c >= '0' && c <= '9') return c;
        if (c >= 'A' && c <= 'Z') return c;
    }
    if ((name[0] == 'f' || name[0] == 'F') && (name[1] >= '0' && name[1] <= '9')) {
        n = atoi(name + 1);
        if (n >= 1 && n <= 24) return VK_F1 + (n - 1);
    }
    if (!_stricmp(name, "enter") || !_stricmp(name, "return")) return VK_RETURN;
    if (!_stricmp(name, "tab")) return VK_TAB;
    if (!_stricmp(name, "esc") || !_stricmp(name, "escape")) return VK_ESCAPE;
    if (!_stricmp(name, "space")) return VK_SPACE;
    if (!_stricmp(name, "bsp") || !_stricmp(name, "backspace")) return VK_BACK;
    if (!_stricmp(name, "del") || !_stricmp(name, "delete")) return VK_DELETE;
    if (!_stricmp(name, "ins") || !_stricmp(name, "insert")) return VK_INSERT;
    if (!_stricmp(name, "home")) return VK_HOME;
    if (!_stricmp(name, "end")) return VK_END;
    if (!_stricmp(name, "pgup") || !_stricmp(name, "pageup")) return VK_PRIOR;
    if (!_stricmp(name, "pgdn") || !_stricmp(name, "pagedown")) return VK_NEXT;
    if (!_stricmp(name, "up")) return VK_UP;
    if (!_stricmp(name, "down")) return VK_DOWN;
    if (!_stricmp(name, "left")) return VK_LEFT;
    if (!_stricmp(name, "right")) return VK_RIGHT;
    if (!_stricmp(name, "win") || !_stricmp(name, "super") || !_stricmp(name, "lwin")) return VK_LWIN;
    if (!_stricmp(name, "rwin")) return VK_RWIN;
    if (!_stricmp(name, "ctrl") || !_stricmp(name, "control")) return VK_CONTROL;
    if (!_stricmp(name, "alt")) return VK_MENU;
    if (!_stricmp(name, "shift")) return VK_SHIFT;
    return -1;
}

static void press_key_combo(const char *spec) {
    int mods[8];
    int n_mods = 0;
    char base[64];
    const char *p = spec;
    int i, base_vk;
    while (1) {
        const char *sep = strpbrk(p, "-+");
        char tok[16];
        size_t len;
        int vk;
        if (!sep) break;
        len = sep - p;
        if (len >= sizeof(tok)) break;
        memcpy(tok, p, len);
        tok[len] = 0;
        vk = parse_vk(tok);
        if (vk < 0 || n_mods >= 7) break;
        mods[n_mods++] = vk;
        p = sep + 1;
    }
    strncpy(base, p, sizeof(base) - 1);
    base[sizeof(base) - 1] = 0;
    base_vk = parse_vk(base);
    if (base_vk < 0) return;
    for (i = 0; i < n_mods; i++) keybd_event((BYTE)mods[i], 0, 0, 0);
    keybd_event((BYTE)base_vk, 0, 0, 0);
    Sleep(20);
    keybd_event((BYTE)base_vk, 0, KEYEVENTF_KEYUP, 0);
    for (i = n_mods - 1; i >= 0; i--) keybd_event((BYTE)mods[i], 0, KEYEVENTF_KEYUP, 0);
}

static void type_string(const char *str) {
    while (*str) {
        SHORT scan = VkKeyScanA(*str);
        BYTE vk, shift;
        if (scan == -1) { str++; continue; }
        vk = (BYTE)(scan & 0xFF);
        shift = (BYTE)((scan >> 8) & 0xFF);
        if (shift & 1) keybd_event(VK_SHIFT, 0, 0, 0);
        if (shift & 2) keybd_event(VK_CONTROL, 0, 0, 0);
        if (shift & 4) keybd_event(VK_MENU, 0, 0, 0);
        keybd_event(vk, 0, 0, 0);
        keybd_event(vk, 0, KEYEVENTF_KEYUP, 0);
        if (shift & 4) keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
        if (shift & 2) keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
        if (shift & 1) keybd_event(VK_SHIFT, 0, KEYEVENTF_KEYUP, 0);
        Sleep(5);
        str++;
    }
}

/* ------------------------------------------------------------------ */
/* Mouse                                                              */
/* ------------------------------------------------------------------ */

static void button_flags(int button, DWORD *down, DWORD *up) {
    if (button == 2) { *down = MOUSEEVENTF_RIGHTDOWN;  *up = MOUSEEVENTF_RIGHTUP; }
    else if (button == 3) { *down = MOUSEEVENTF_MIDDLEDOWN; *up = MOUSEEVENTF_MIDDLEUP; }
    else { *down = MOUSEEVENTF_LEFTDOWN; *up = MOUSEEVENTF_LEFTUP; }
}

static void do_click(int button, int double_click) {
    DWORD down, up;
    button_flags(button, &down, &up);
    mouse_event(down, 0, 0, 0, 0);
    Sleep(20);
    mouse_event(up, 0, 0, 0, 0);
    if (double_click) {
        Sleep(50);
        mouse_event(down, 0, 0, 0, 0);
        Sleep(20);
        mouse_event(up, 0, 0, 0, 0);
    }
}

static void do_mdown(int button) {
    DWORD down, up;
    button_flags(button, &down, &up);
    mouse_event(down, 0, 0, 0, 0);
}

static void do_mup(int button) {
    DWORD down, up;
    button_flags(button, &down, &up);
    mouse_event(up, 0, 0, 0, 0);
}

static void do_wheel(int delta) {
    /* WHEEL_DELTA is 120 per notch; pass user value directly. */
    mouse_event(MOUSEEVENTF_WHEEL, 0, 0, (DWORD)delta, 0);
}

static int parse_button(const char *name) {
    if (!name || !*name) return 1;
    if (!_stricmp(name, "left") || !_stricmp(name, "1")) return 1;
    if (!_stricmp(name, "right") || !_stricmp(name, "2")) return 2;
    if (!_stricmp(name, "middle") || !_stricmp(name, "3")) return 3;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Screenshot                                                          */
/* ------------------------------------------------------------------ */

static int capture_bmp(BYTE **out_buf, DWORD *out_size) {
    HDC hdc_screen = GetDC(NULL);
    int w, h, row;
    HDC hdc_mem;
    HBITMAP hbm;
    HGDIOBJ old_obj;
    BITMAPINFO bi;
    DWORD px_size, total;
    BYTE *buf;
    BITMAPFILEHEADER *bfh;
    if (!hdc_screen) return -1;
    w = GetSystemMetrics(SM_CXSCREEN);
    h = GetSystemMetrics(SM_CYSCREEN);
    hdc_mem = CreateCompatibleDC(hdc_screen);
    hbm = CreateCompatibleBitmap(hdc_screen, w, h);
    old_obj = SelectObject(hdc_mem, hbm);
    BitBlt(hdc_mem, 0, 0, w, h, hdc_screen, 0, 0, SRCCOPY);

    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  /* negative = top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;

    row = ((w * 3 + 3) & ~3);
    px_size = (DWORD)row * h;
    total = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + px_size;

    buf = (BYTE*)malloc(total);
    if (!buf) {
        SelectObject(hdc_mem, old_obj);
        DeleteObject(hbm); DeleteDC(hdc_mem); ReleaseDC(NULL, hdc_screen);
        return -1;
    }
    bfh = (BITMAPFILEHEADER*)buf;
    bfh->bfType = 0x4D42;  /* 'BM' */
    bfh->bfSize = total;
    bfh->bfReserved1 = 0;
    bfh->bfReserved2 = 0;
    bfh->bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    memcpy(buf + sizeof(BITMAPFILEHEADER), &bi.bmiHeader, sizeof(BITMAPINFOHEADER));
    /* GetDIBits may modify biHeight, so we re-set it */
    bi.bmiHeader.biHeight = -h;
    GetDIBits(hdc_mem, hbm, 0, h, buf + bfh->bfOffBits, &bi, DIB_RGB_COLORS);

    SelectObject(hdc_mem, old_obj);
    DeleteObject(hbm); DeleteDC(hdc_mem); ReleaseDC(NULL, hdc_screen);

    *out_buf = buf;
    *out_size = total;
    return 0;
}

/* ------------------------------------------------------------------ */
/* File I/O                                                           */
/* ------------------------------------------------------------------ */

static int read_whole_file(const char *path, BYTE **out_buf, DWORD *out_size) {
    HANDLE h;
    DWORD sz, got = 0;
    BYTE *buf;
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    sz = GetFileSize(h, NULL);
    buf = (BYTE*)malloc(sz ? sz : 1);
    if (!buf) { CloseHandle(h); return -1; }
    if (sz > 0 && !ReadFile(h, buf, sz, &got, NULL)) {
        CloseHandle(h); free(buf); return -1;
    }
    CloseHandle(h);
    *out_buf = buf;
    *out_size = got;
    return 0;
}

static int write_whole_file(const char *path, const BYTE *buf, DWORD size) {
    HANDLE h;
    DWORD wrote = 0;
    BOOL ok;
    h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    ok = WriteFile(h, buf, size, &wrote, NULL);
    CloseHandle(h);
    return (ok && wrote == size) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Process management                                                 */
/* ------------------------------------------------------------------ */

#define MAX_PROCS 64
static HANDLE g_proc_handles[MAX_PROCS] = {0};
static DWORD  g_proc_pids[MAX_PROCS] = {0};
static int    g_proc_count = 0;

static int do_exec(const char *cmdline, DWORD *out_pid) {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char buf[1024];
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    strncpy(buf, cmdline, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    if (!CreateProcessA(NULL, buf, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return -1;
    *out_pid = pi.dwProcessId;
    if (g_proc_count < MAX_PROCS) {
        g_proc_handles[g_proc_count] = pi.hProcess;
        g_proc_pids[g_proc_count] = pi.dwProcessId;
        g_proc_count++;
    } else {
        CloseHandle(pi.hProcess);
    }
    CloseHandle(pi.hThread);
    return 0;
}

static int do_wait(DWORD pid, DWORD timeout_ms, DWORD *out_exit) {
    HANDLE h = NULL;
    int slot = -1, i;
    DWORD wr;
    for (i = 0; i < g_proc_count; i++) {
        if (g_proc_handles[i] && g_proc_pids[i] == pid) {
            h = g_proc_handles[i]; slot = i; break;
        }
    }
    if (!h) {
        h = OpenProcess(PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, pid);
        if (!h) return -1;
    }
    wr = WaitForSingleObject(h, timeout_ms);
    if (wr == WAIT_TIMEOUT) {
        if (slot < 0) CloseHandle(h);
        return -2;
    }
    GetExitCodeProcess(h, out_exit);
    if (slot >= 0) {
        CloseHandle(h);
        g_proc_handles[slot] = NULL;
        g_proc_pids[slot] = 0;
    } else {
        CloseHandle(h);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Command dispatch                                                   */
/* ------------------------------------------------------------------ */

/* Returns 0 to continue, 1 to close client connection. */
static int handle_command(SOCKET s, char *line) {
    char *cmd = line;
    char *arg = strchr(line, ' ');
    if (arg) { *arg++ = 0; while (*arg == ' ') arg++; }
    else arg = "";

    if (!_stricmp(cmd, "PING")) {
        send_str(s, "OK pong\n");
        return 0;
    }
    if (!_stricmp(cmd, "QUIT") || !_stricmp(cmd, "EXIT") || !_stricmp(cmd, "BYE")) {
        send_ok(s);
        return 1;
    }
    if (!_stricmp(cmd, "SHOT")) {
        BYTE *buf = NULL; DWORD sz = 0;
        if (capture_bmp(&buf, &sz) < 0) { send_err(s, "capture failed"); return 0; }
        send_ok_data(s, buf, sz);
        free(buf);
        return 0;
    }
    if (!_stricmp(cmd, "READ")) {
        BYTE *buf = NULL; DWORD sz = 0;
        if (!*arg) { send_err(s, "missing path"); return 0; }
        if (read_whole_file(arg, &buf, &sz) < 0) { send_err(s, "read failed"); return 0; }
        send_ok_data(s, buf, sz);
        free(buf);
        return 0;
    }
    if (!_stricmp(cmd, "WRITE")) {
        char *space = strrchr(arg, ' ');
        DWORD len;
        BYTE *buf;
        char path[MAX_PATH];
        size_t pathlen;
        if (!space) { send_err(s, "usage: WRITE <path> <length>"); return 0; }
        len = (DWORD)atol(space + 1);
        pathlen = space - arg;
        if (pathlen >= sizeof(path)) { send_err(s, "path too long"); return 0; }
        memcpy(path, arg, pathlen); path[pathlen] = 0;
        buf = (BYTE*)malloc(len ? len : 1);
        if (!buf) { send_err(s, "oom"); return 0; }
        if (len > 0 && recv_n(s, buf, (int)len) < 0) { free(buf); send_err(s, "short read"); return 0; }
        if (write_whole_file(path, buf, len) < 0) { free(buf); send_err(s, "write failed"); return 0; }
        free(buf);
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "KEY")) {
        if (!*arg) { send_err(s, "missing key"); return 0; }
        press_key_combo(arg);
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "KEYS")) {
        type_string(arg);
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "MOVE")) {
        int x, y;
        if (sscanf(arg, "%d %d", &x, &y) != 2) { send_err(s, "usage: MOVE x y"); return 0; }
        SetCursorPos(x, y);
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "MOVEREL")) {
        int dx, dy;
        POINT p;
        if (sscanf(arg, "%d %d", &dx, &dy) != 2) { send_err(s, "usage: MOVEREL dx dy"); return 0; }
        GetCursorPos(&p);
        SetCursorPos(p.x + dx, p.y + dy);
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "CLICK")) {
        do_click(parse_button(arg), 0);
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "DCLICK")) {
        do_click(parse_button(arg), 1);
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "MDOWN")) {
        do_mdown(parse_button(arg));
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "MUP")) {
        do_mup(parse_button(arg));
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "DRAG")) {
        int x, y, btn = 1;
        char btn_name[16] = "";
        int n = sscanf(arg, "%d %d %15s", &x, &y, btn_name);
        if (n < 2) { send_err(s, "usage: DRAG x y [button]"); return 0; }
        if (n >= 3) btn = parse_button(btn_name);
        do_mdown(btn);
        Sleep(20);
        SetCursorPos(x, y);
        Sleep(20);
        do_mup(btn);
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "WHEEL")) {
        int delta = atoi(arg);
        do_wheel(delta);
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "KEYDOWN")) {
        int vk = parse_vk(arg);
        if (vk < 0) { send_err(s, "unknown key"); return 0; }
        keybd_event((BYTE)vk, 0, 0, 0);
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "KEYUP")) {
        int vk = parse_vk(arg);
        if (vk < 0) { send_err(s, "unknown key"); return 0; }
        keybd_event((BYTE)vk, 0, KEYEVENTF_KEYUP, 0);
        send_ok(s);
        return 0;
    }
    if (!_stricmp(cmd, "SCREEN")) {
        char rsp[64];
        _snprintf(rsp, sizeof(rsp), "OK %d %d\n",
                  GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
        send_str(s, rsp);
        return 0;
    }
    if (!_stricmp(cmd, "MPOS")) {
        POINT p;
        char rsp[64];
        GetCursorPos(&p);
        _snprintf(rsp, sizeof(rsp), "OK %ld %ld\n", p.x, p.y);
        send_str(s, rsp);
        return 0;
    }
    if (!_stricmp(cmd, "EXEC")) {
        DWORD pid = 0;
        char rsp[64];
        if (!*arg) { send_err(s, "missing cmdline"); return 0; }
        if (do_exec(arg, &pid) < 0) { send_err(s, "CreateProcess failed"); return 0; }
        _snprintf(rsp, sizeof(rsp), "OK %lu\n", (unsigned long)pid);
        send_str(s, rsp);
        return 0;
    }
    if (!_stricmp(cmd, "WAIT")) {
        DWORD pid = 0, timeout = INFINITE, exit_code = 0;
        char rsp[64];
        int rc;
        if (sscanf(arg, "%lu %lu", &pid, &timeout) < 1) { send_err(s, "usage: WAIT pid [timeout_ms]"); return 0; }
        rc = do_wait(pid, timeout, &exit_code);
        if (rc == -1) { send_err(s, "OpenProcess failed"); return 0; }
        if (rc == -2) { send_err(s, "timeout"); return 0; }
        _snprintf(rsp, sizeof(rsp), "OK %lu\n", (unsigned long)exit_code);
        send_str(s, rsp);
        return 0;
    }
    if (!_stricmp(cmd, "SLEEP")) {
        Sleep((DWORD)atol(arg));
        send_ok(s);
        return 0;
    }
    send_err(s, "unknown command");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Server main                                                         */
/* ------------------------------------------------------------------ */

static void serve(SOCKET listener) {
    while (1) {
        struct sockaddr_in cli;
        int cli_len = sizeof(cli);
        SOCKET c = accept(listener, (struct sockaddr*)&cli, &cli_len);
        if (c == INVALID_SOCKET) continue;
        printf("[+] client connected from %s\n", inet_ntoa(cli.sin_addr));
        fflush(stdout);
        while (1) {
            char line[LINEBUF];
            int n = recv_line(c, line, sizeof(line));
            if (n < 0) break;
            if (n == 0) continue;
            printf("    > %s\n", line); fflush(stdout);
            if (handle_command(c, line) == 1) break;
        }
        closesocket(c);
        printf("[-] client disconnected\n"); fflush(stdout);
    }
}

int main(int argc, char **argv) {
    WSADATA wsa;
    SOCKET ls;
    struct sockaddr_in addr;
    int opt = 1;
    char *env_port = getenv("VM_AGENT_PORT");
    if (env_port) g_port = atoi(env_port);
    if (argc >= 2) g_port = atoi(argv[1]);

    if (WSAStartup(MAKEWORD(1, 1), &wsa) != 0) { fprintf(stderr, "WSAStartup failed\n"); return 1; }

    ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == INVALID_SOCKET) { fprintf(stderr, "socket failed\n"); return 1; }
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((u_short)g_port);
    if (bind(ls, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "bind failed (port %d in use?)\n", g_port);
        return 1;
    }
    listen(ls, 1);
    printf("vm_agent listening on 0.0.0.0:%d\n", g_port); fflush(stdout);
    serve(ls);
    closesocket(ls);
    WSACleanup();
    return 0;
}
