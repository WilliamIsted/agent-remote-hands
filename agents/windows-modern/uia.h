#pragma once
/* uia.h — UI Automation wrapper for the windows-modern agent.
 *
 * Exposes Windows UI Automation (the modern accessibility tree) to the
 * agent's element verbs. AI clients use these to find and act on controls
 * by semantic name and role rather than pixel coordinates — robust to DPI,
 * theming, layout shifts, language localisation. Pixel-based capture/input
 * verbs remain the fallback for surfaces UIA can't see.
 *
 * Best-effort: every entry point degrades gracefully. If UIA isn't
 * available on the target (very rare on Vista+; possible on stripped Win
 * server installs), uia_available() returns 0 and INFO advertises
 * ui_automation=no instead of uia.
 *
 * State is per-thread: each connection_worker has its own id-to-element
 * map, so two clients can't trip over each other's ids. Element handles
 * are released when the thread exits or when ELEMENTS rebuilds the map.
 */

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

int  uia_available(void);
int  uia_init(void);
void uia_shutdown(void);

/* Per-thread state init. Each connection_worker calls these the same way
   it calls wgc_thread_init/uninit. Idempotent and safe to call when UIA
   is unavailable. */
void uia_thread_init(void);
void uia_thread_uninit(void);

/* ELEMENTS [rect_xywh]: enumerate visible / interactable elements. If
   rect_xywh is NULL, scans the whole desktop; else restricts to the
   element subtree containing the rect's centre point. Output is a
   tab-separated table, one element per line:
     <id>\t<x>\t<y>\t<w>\t<h>\t<role>\t<name>\t<value>\t<flags>\n
   Caller frees out_buf with free(). Returns 0 on success, -1 on failure.
   Side effect: rebuilds the per-thread id map; older ids are released. */
int uia_elements(const int *rect_xywh,
                 unsigned char **out_buf, unsigned long *out_size);

/* ELEMENT_AT: hit-test the screen coordinate. On success fills *out_row
   with the same single-row format as ELEMENTS rows (without trailing
   newline). out_row is malloc'd; caller frees. Returns 0 on success,
   -1 on no element / failure. Adds a new id to the per-thread map. */
int uia_element_at(int x, int y, char **out_row);

/* ELEMENT_FIND <role> <name-substr>: linear scan for first element whose
   role matches role_token (e.g. "button") and whose name contains
   name_substr (case-insensitive). out_row format same as element_at.
   Returns 0 on success, -1 on not found. Adds a new id to the map. */
int uia_element_find(const char *role_token, const char *name_substr,
                     char **out_row);

/* ELEMENT_INVOKE <id>: trigger the element's primary action via
   InvokePattern. Returns:
     0  on success
    -1  internal failure
    -2  id not in map (likely stale or never issued)
    -3  element doesn't expose InvokePattern (not invokable). */
int uia_element_invoke(int id);

/* ELEMENT_FOCUS <id>: give keyboard focus. Returns 0 / -1 / -2 (id not
   found). */
int uia_element_focus(int id);

/* ELEMENT_TREE <id>: recursive descent under <id>. Output is a
   tab-separated table; each row carries a leading <depth> column so the
   client can reconstruct the tree:
     <depth>\t<id>\t<x>\t<y>\t<w>\t<h>\t<role>\t<name>\t<value>\t<flags>\n
   The element at <id> itself is row 0; its children are row 1, grand-
   children row 2, and so on. New child ids are appended to the per-thread
   map. Caller frees out_buf with free(). Returns 0 / -1 / -2. */
int uia_element_tree(int id, unsigned char **out_buf, unsigned long *out_size);

/* ELEMENT_TEXT <id>: read text content. Tries TextPattern (rich edits,
   documents) first, falls back to ValuePattern (simple edit fields), falls
   back to NamePropertyId. Output is the text bytes (Latin-1 / ANSI codepage
   on the wire); caller frees out_buf. Returns 0 / -1 / -2. */
int uia_element_text(int id, unsigned char **out_buf, unsigned long *out_size);

/* ELEMENT_SET_TEXT <id> <bytes>: write into a value-bearing control via
   ValuePattern::SetValue. Returns 0 / -1 / -2 / -3 (no ValuePattern). */
int uia_element_set_text(int id, const unsigned char *bytes, unsigned long len);

/* ELEMENT_TOGGLE <id>: TogglePattern::Toggle. Returns 0 / -1 / -2 / -3. */
int uia_element_toggle(int id);

/* ELEMENT_EXPAND <id> / ELEMENT_COLLAPSE <id>: ExpandCollapsePattern.
   Returns 0 / -1 / -2 / -3. */
int uia_element_expand(int id);
int uia_element_collapse(int id);

#ifdef __cplusplus
}
#endif
