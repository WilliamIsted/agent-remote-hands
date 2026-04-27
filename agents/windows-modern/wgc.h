#pragma once
/* wgc.h — Windows.Graphics.Capture wrapper for the windows-modern agent.
 *
 * WGC is the Vista/Win10-era capture API that handles hardware-accelerated
 * and DRM-protected surfaces correctly — DirectComposition, full-screen
 * Direct3D, modern WinUI 3 / XAML controls. BitBlt renders those areas as
 * black; WGC captures them as the user sees them. Win10 1803+ (April 2018).
 *
 * Best-effort: every entry point degrades gracefully. If WGC isn't supported
 * on the target machine, the agent falls back to BitBlt and INFO advertises
 * capture=gdi instead of capture=wgc.
 */

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True after a successful wgc_init(). */
int wgc_available(void);

/* Initialise the D3D11 device, WinRT device, and other globals. Call once
   from main() / run_server() before spawning worker threads. Returns 1 on
   success, 0 on failure (in which case wgc_available() stays 0 and the
   agent should fall back to BitBlt). */
int wgc_init(void);

/* Tear down. Call once at process exit. */
void wgc_shutdown(void);

/* Per-thread COM apartment init. Each connection_worker thread that may
   call any wgc_capture_*() function must call wgc_thread_init() at start
   and wgc_thread_uninit() at exit. Idempotent and safe to call when WGC
   is unavailable. */
void wgc_thread_init(void);
void wgc_thread_uninit(void);

/* Capture the primary monitor as a 32-bit top-down HBITMAP (BGRA, premul
   alpha). The OS cursor is included on builds that capture it by default
   (every build since Win10 1803) and always on Win10 19041+ where we set
   the explicit flag. Returns NULL on failure — caller must fall back. */
HBITMAP wgc_capture_primary_monitor(void);

/* Capture a specific top-level window. Returns NULL if the window can't
   be captured (most surfaces work; some system / overlay windows don't). */
HBITMAP wgc_capture_window(HWND hwnd);

/* Capture the entire virtual screen (all connected monitors). On a single-
   monitor host this is identical to wgc_capture_primary_monitor; on multi-
   monitor it captures each connected display via WGC and stitches them
   into a single HBITMAP at the virtual-screen rect. Returns NULL on any
   failure — caller falls back to BitBlt. */
HBITMAP wgc_capture_virtual_screen(void);

/* Capture a sub-rect of the screen via WGC. Finds the monitor containing
   the rect, captures it, and crops. Returns NULL if the rect spans multiple
   monitors (caller should use wgc_capture_virtual_screen + crop or fall
   back to BitBlt). Adds ~50 ms vs. BitBlt but captures modern surfaces
   correctly. */
HBITMAP wgc_capture_rect(int sx, int sy, int w, int h);

/* Streaming-session API for WATCH / WAITFOR. Opens a single WGC session
   and holds it open across many frame retrievals — avoids the ~50 ms
   per-call setup that would saturate the agent at any reasonable frame
   rate. Workflow:

       struct wgc_session *s = wgc_session_open_primary();
       while (...) {
           HBITMAP frame = wgc_session_get_frame(s);
           if (frame) { encode/send/release; }
       }
       wgc_session_close(s);

   The session captures into a 2-frame pool. get_frame waits up to ~1.5 s
   for a new frame; sessions deliver frames at the display's refresh rate
   so any normal interval is well within that bound. Returns NULL on any
   failure — caller falls back to BitBlt. */
struct wgc_session;
struct wgc_session *wgc_session_open_primary(void);
struct wgc_session *wgc_session_open_window(HWND hwnd);
HBITMAP wgc_session_get_frame(struct wgc_session *s);
void wgc_session_close(struct wgc_session *s);

#ifdef __cplusplus
}
#endif
