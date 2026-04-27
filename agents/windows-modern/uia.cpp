/* uia.cpp — Windows UI Automation implementation for the windows-modern
 * agent. See uia.h for shape and rationale.
 *
 * Singleton IUIAutomation created at uia_init(); per-thread element map
 * lives in `thread_local` storage so each connection_worker has its own
 * id space. Tree walks use ControlViewWalker so we get interactable
 * controls (buttons, edits, menu items) without burying the AI in
 * structural panes.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ole2.h>
#include <wrl/client.h>
#include <UIAutomation.h>

#include <unordered_map>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>

#include "uia.h"

#pragma comment(lib, "uiautomationcore.lib")

using Microsoft::WRL::ComPtr;

/* Process-wide IUIAutomation. UIA's COM object is thread-safe per docs;
   one instance handles every connection. */
static ComPtr<IUIAutomation> g_uia;
static int g_available = 0;

/* Per-thread element map: id → IUIAutomationElement*. ELEMENTS rebuilds
   the map; ELEMENT_AT / ELEMENT_FIND append. Released on thread uninit
   or on next ELEMENTS rebuild. */
struct uia_state {
    int next_id = 1;
    std::unordered_map<int, IUIAutomationElement*> elements;
};
thread_local uia_state *g_state = nullptr;

extern "C" int uia_available(void) { return g_available; }

extern "C" int uia_init(void) {
    if (g_available) return 1;

    /* CoInitialize on this thread (run_server) — workers do their own. */
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (hr == RPC_E_CHANGED_MODE) {
        /* Already STA on this thread (Task Scheduler --install path).
           CoCreateInstance still works; worker threads init fresh. */
    } else if (FAILED(hr)) {
        return 0;
    }

    hr = CoCreateInstance(__uuidof(CUIAutomation), NULL, CLSCTX_INPROC_SERVER,
                          __uuidof(IUIAutomation),
                          reinterpret_cast<void**>(g_uia.GetAddressOf()));
    if (FAILED(hr) || !g_uia) return 0;

    g_available = 1;
    return 1;
}

extern "C" void uia_shutdown(void) {
    if (!g_available) return;
    g_uia.Reset();
    g_available = 0;
}

extern "C" void uia_thread_init(void) {
    if (!g_available) return;
    if (!g_state) g_state = new uia_state();
}

extern "C" void uia_thread_uninit(void) {
    if (!g_state) return;
    for (auto &kv : g_state->elements) {
        if (kv.second) kv.second->Release();
    }
    delete g_state;
    g_state = nullptr;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static const char *role_name_for(CONTROLTYPEID id) {
    switch (id) {
        case UIA_ButtonControlTypeId:       return "button";
        case UIA_CalendarControlTypeId:     return "calendar";
        case UIA_CheckBoxControlTypeId:     return "checkbox";
        case UIA_ComboBoxControlTypeId:     return "combobox";
        case UIA_EditControlTypeId:         return "edit";
        case UIA_HyperlinkControlTypeId:    return "link";
        case UIA_ImageControlTypeId:        return "image";
        case UIA_ListItemControlTypeId:     return "listitem";
        case UIA_ListControlTypeId:         return "list";
        case UIA_MenuControlTypeId:         return "menu";
        case UIA_MenuBarControlTypeId:      return "menubar";
        case UIA_MenuItemControlTypeId:     return "menuitem";
        case UIA_ProgressBarControlTypeId:  return "progressbar";
        case UIA_RadioButtonControlTypeId:  return "radio";
        case UIA_ScrollBarControlTypeId:    return "scrollbar";
        case UIA_SliderControlTypeId:       return "slider";
        case UIA_SpinnerControlTypeId:      return "spinner";
        case UIA_StatusBarControlTypeId:    return "statusbar";
        case UIA_TabControlTypeId:          return "tab";
        case UIA_TabItemControlTypeId:      return "tabitem";
        case UIA_TextControlTypeId:         return "text";
        case UIA_ToolBarControlTypeId:      return "toolbar";
        case UIA_ToolTipControlTypeId:      return "tooltip";
        case UIA_TreeControlTypeId:         return "tree";
        case UIA_TreeItemControlTypeId:     return "treeitem";
        case UIA_CustomControlTypeId:       return "custom";
        case UIA_GroupControlTypeId:        return "group";
        case UIA_ThumbControlTypeId:        return "thumb";
        case UIA_DataGridControlTypeId:     return "datagrid";
        case UIA_DataItemControlTypeId:     return "dataitem";
        case UIA_DocumentControlTypeId:     return "document";
        case UIA_SplitButtonControlTypeId:  return "splitbutton";
        case UIA_WindowControlTypeId:       return "window";
        case UIA_PaneControlTypeId:         return "pane";
        case UIA_HeaderControlTypeId:       return "header";
        case UIA_HeaderItemControlTypeId:   return "headeritem";
        case UIA_TableControlTypeId:        return "table";
        case UIA_TitleBarControlTypeId:     return "titlebar";
        case UIA_SeparatorControlTypeId:    return "separator";
        case UIA_SemanticZoomControlTypeId: return "semanticzoom";
        case UIA_AppBarControlTypeId:       return "appbar";
        default:                            return "unknown";
    }
}

/* True if this element belongs to the agent's own UI. Used to skip the
   agent's own console window during enumeration - otherwise a caller
   that searches for "Close" can match the agent window's Close button
   and self-terminate the agent. We have to match TWO ways:
   1. The element's CurrentProcessId == our PID. Catches direct cases.
   2. The element's name matches our exe path. Catches the console
      window, which on Windows is owned by conhost.exe / Windows
      Terminal, not by us - the console *displays* our exe path as its
      title, but get_CurrentProcessId returns the host's PID.
   Both checks are cheap. The name list is built once at first call. */
static bool element_is_own_process(IUIAutomationElement *el) {
    static const DWORD own_pid = GetCurrentProcessId();
    static char own_exe_path[MAX_PATH] = "";
    static char own_exe_basename[MAX_PATH] = "";
    if (!own_exe_path[0]) {
        DWORD n = GetModuleFileNameA(NULL, own_exe_path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) own_exe_path[0] = 0;
        const char *base = strrchr(own_exe_path, '\\');
        const char *fwd  = strrchr(own_exe_path, '/');
        if (fwd > base) base = fwd;
        if (base) base++; else base = own_exe_path;
        strncpy_s(own_exe_basename, sizeof(own_exe_basename),
                  base, _TRUNCATE);
    }
    if (!el) return false;

    int pid = 0;
    if (SUCCEEDED(el->get_CurrentProcessId(&pid))
        && (DWORD)pid == own_pid) {
        return true;
    }

    /* Name-based match for console window child elements. */
    if (own_exe_path[0]) {
        BSTR name = NULL;
        bool match = false;
        if (SUCCEEDED(el->get_CurrentName(&name)) && name) {
            char buf[MAX_PATH];
            int len = WideCharToMultiByte(CP_ACP, 0, name, -1, buf,
                                          (int)sizeof(buf), NULL, NULL);
            if (len > 0) {
                /* Match either the full exe path or just the basename
                   (e.g. "remote-hands.exe"). Console title is usually
                   the full path; some tabitems carry just the basename. */
                if (_stricmp(buf, own_exe_path) == 0
                    || _stricmp(buf, own_exe_basename) == 0) {
                    match = true;
                }
            }
            SysFreeString(name);
        }
        if (match) return true;
    }
    return false;
}

/* Replace tab/newline with space so a value can sit on one tab-delimited
   row. Caller-supplied buffer; returns the buffer for chaining. */
static char *sanitize_inplace(char *s) {
    for (char *p = s; *p; p++) {
        if (*p == '\t' || *p == '\n' || *p == '\r') *p = ' ';
    }
    return s;
}

/* BSTR → UTF-8-ish ANSI char (we use ANSI/Latin-1 on the wire per
   PROTOCOL.md). Truncates to max-1 chars. */
static void bstr_to_chars(BSTR src, char *dst, size_t max) {
    if (!src) { dst[0] = 0; return; }
    int len = WideCharToMultiByte(CP_ACP, 0, src, -1, dst, (int)max, NULL, NULL);
    if (len <= 0) dst[0] = 0;
    else dst[(size_t)len < max ? (size_t)len - 1 : max - 1] = 0;
    sanitize_inplace(dst);
}

/* Build the comma-separated flag string for a row. */
static void build_flags(IUIAutomationElement *el, char *out, size_t max) {
    out[0] = 0;
    BOOL b = FALSE;
    auto append = [&](const char *s) {
        size_t cur = strlen(out);
        size_t need = strlen(s) + (cur ? 1 : 0);
        if (cur + need + 1 > max) return;
        if (cur) { out[cur] = ','; out[cur + 1] = 0; }
        strcat(out, s);
    };
    if (SUCCEEDED(el->get_CurrentIsEnabled(&b)) && b)        append("enabled");
    if (SUCCEEDED(el->get_CurrentHasKeyboardFocus(&b)) && b) append("focused");
    if (SUCCEEDED(el->get_CurrentIsOffscreen(&b)) && b)      append("offscreen");
    if (SUCCEEDED(el->get_CurrentIsPassword(&b)) && b)       append("password");
    /* Toggle / Selection / ExpandCollapse states via patterns — best
       effort, ignore failures. */
    ComPtr<IUIAutomationTogglePattern> toggle;
    if (SUCCEEDED(el->GetCurrentPatternAs(UIA_TogglePatternId,
            __uuidof(IUIAutomationTogglePattern), (void**)toggle.GetAddressOf()))
        && toggle) {
        ToggleState state;
        if (SUCCEEDED(toggle->get_CurrentToggleState(&state)) && state == ToggleState_On) {
            append("checked");
        }
    }
    ComPtr<IUIAutomationSelectionItemPattern> sel;
    if (SUCCEEDED(el->GetCurrentPatternAs(UIA_SelectionItemPatternId,
            __uuidof(IUIAutomationSelectionItemPattern), (void**)sel.GetAddressOf()))
        && sel) {
        BOOL selected = FALSE;
        if (SUCCEEDED(sel->get_CurrentIsSelected(&selected)) && selected) {
            append("selected");
        }
    }
    ComPtr<IUIAutomationExpandCollapsePattern> exp;
    if (SUCCEEDED(el->GetCurrentPatternAs(UIA_ExpandCollapsePatternId,
            __uuidof(IUIAutomationExpandCollapsePattern), (void**)exp.GetAddressOf()))
        && exp) {
        ExpandCollapseState state;
        if (SUCCEEDED(exp->get_CurrentExpandCollapseState(&state))
            && state == ExpandCollapseState_Expanded) {
            append("expanded");
        }
    }
}

/* Format the canonical row for an element (without trailing newline).
   Returns the number of chars written; 0 on failure. */
static int format_row(IUIAutomationElement *el, int id, char *buf, size_t max) {
    if (!el || !buf || max < 64) return 0;

    CONTROLTYPEID ct = 0;
    el->get_CurrentControlType(&ct);
    const char *role = role_name_for(ct);

    RECT r = {0, 0, 0, 0};
    el->get_CurrentBoundingRectangle(&r);
    int x = r.left, y = r.top, w = r.right - r.left, h = r.bottom - r.top;

    BSTR name_bstr = NULL;
    el->get_CurrentName(&name_bstr);
    char name[256];
    bstr_to_chars(name_bstr, name, sizeof(name));
    if (name_bstr) SysFreeString(name_bstr);

    char value[256] = "";
    ComPtr<IUIAutomationValuePattern> vp;
    if (SUCCEEDED(el->GetCurrentPatternAs(UIA_ValuePatternId,
            __uuidof(IUIAutomationValuePattern), (void**)vp.GetAddressOf()))
        && vp) {
        BSTR v = NULL;
        if (SUCCEEDED(vp->get_CurrentValue(&v)) && v) {
            bstr_to_chars(v, value, sizeof(value));
            SysFreeString(v);
        }
    }

    char flags[128];
    build_flags(el, flags, sizeof(flags));

    int n = _snprintf_s(buf, max, _TRUNCATE,
                        "%d\t%d\t%d\t%d\t%d\t%s\t%s\t%s\t%s",
                        id, x, y, w, h, role, name, value, flags);
    return (n > 0) ? n : 0;
}

static bool element_is_interesting(IUIAutomationElement *el) {
    /* Filter rule: keep elements that are (a) on-screen and (b) either
       have a non-empty name, or are a known interactable role. Drops the
       sea of structural panes that UIA puts in the raw tree. */
    if (!el) return false;
    BOOL offscreen = FALSE;
    el->get_CurrentIsOffscreen(&offscreen);
    if (offscreen) return false;

    BSTR name = NULL;
    el->get_CurrentName(&name);
    bool has_name = (name && SysStringLen(name) > 0);
    if (name) SysFreeString(name);

    CONTROLTYPEID ct = 0;
    el->get_CurrentControlType(&ct);
    bool interactable =
        ct == UIA_ButtonControlTypeId || ct == UIA_CheckBoxControlTypeId ||
        ct == UIA_ComboBoxControlTypeId || ct == UIA_EditControlTypeId ||
        ct == UIA_HyperlinkControlTypeId || ct == UIA_ListItemControlTypeId ||
        ct == UIA_MenuItemControlTypeId || ct == UIA_RadioButtonControlTypeId ||
        ct == UIA_TabItemControlTypeId || ct == UIA_TreeItemControlTypeId ||
        ct == UIA_SliderControlTypeId || ct == UIA_SpinnerControlTypeId ||
        ct == UIA_SplitButtonControlTypeId;

    return has_name || interactable;
}

/* Add an element to the per-thread map and return its new id. The map
   takes ownership of an AddRef'd reference. */
static int register_element(IUIAutomationElement *el) {
    if (!g_state || !el) return -1;
    int id = g_state->next_id++;
    el->AddRef();
    g_state->elements[id] = el;
    return id;
}

/* Lookup; returns nullptr if id is unknown. */
static IUIAutomationElement *lookup_element(int id) {
    if (!g_state) return nullptr;
    auto it = g_state->elements.find(id);
    if (it == g_state->elements.end()) return nullptr;
    return it->second;
}

/* ------------------------------------------------------------------ */
/* ELEMENTS — walk subtree, emit rows                                  */
/* ------------------------------------------------------------------ */

extern "C" int uia_elements(const int *rect_xywh,
                            unsigned char **out_buf, unsigned long *out_size) {
    if (!g_available || !g_state) return -1;

    /* Drop existing ids — ELEMENTS is regenerative. */
    for (auto &kv : g_state->elements) {
        if (kv.second) kv.second->Release();
    }
    g_state->elements.clear();
    g_state->next_id = 1;

    ComPtr<IUIAutomationElement> root;
    if (rect_xywh) {
        POINT pt{ rect_xywh[0] + rect_xywh[2] / 2, rect_xywh[1] + rect_xywh[3] / 2 };
        if (FAILED(g_uia->ElementFromPoint(pt, root.GetAddressOf())) || !root) {
            return -1;
        }
    } else {
        if (FAILED(g_uia->GetRootElement(root.GetAddressOf())) || !root) {
            return -1;
        }
    }

    /* Walk via ControlViewWalker — interactable controls only, no scaffolding. */
    ComPtr<IUIAutomationTreeWalker> walker;
    if (FAILED(g_uia->get_ControlViewWalker(walker.GetAddressOf())) || !walker) {
        return -1;
    }

    std::string out;
    out.reserve(16 * 1024);
    char row[1024];

    /* Iterative DFS to avoid recursion / stack overflow on deep trees. */
    std::vector<IUIAutomationElement*> stack;
    root->AddRef();
    stack.push_back(root.Get());

    /* Cap to prevent runaway captures pinning the agent. UIA on a busy
       desktop tops out around ~2000 visible elements; 5000 is safe slack. */
    constexpr int MAX_ELEMENTS = 5000;
    int emitted = 0;

    while (!stack.empty() && emitted < MAX_ELEMENTS) {
        IUIAutomationElement *el = stack.back();
        stack.pop_back();

        /* Skip our own process's window subtree entirely - prevents a
           caller from invoking the agent's own Close button and killing
           the agent. */
        if (element_is_own_process(el)) {
            el->Release();
            continue;
        }

        if (element_is_interesting(el)) {
            int id = register_element(el);
            if (id > 0) {
                int n = format_row(el, id, row, sizeof(row));
                if (n > 0) {
                    out.append(row, (size_t)n);
                    out.push_back('\n');
                    emitted++;
                }
            }
        }

        /* Push children. Walker's GetFirstChild returns a fresh COM ref. */
        ComPtr<IUIAutomationElement> child;
        if (SUCCEEDED(walker->GetFirstChildElement(el, child.GetAddressOf())) && child) {
            std::vector<IUIAutomationElement*> children;
            children.push_back(child.Detach());
            while (true) {
                IUIAutomationElement *prev = children.back();
                ComPtr<IUIAutomationElement> sibling;
                if (FAILED(walker->GetNextSiblingElement(prev, sibling.GetAddressOf()))
                    || !sibling) break;
                children.push_back(sibling.Detach());
            }
            /* Reverse-push so first child is processed first (LIFO stack). */
            for (auto it = children.rbegin(); it != children.rend(); ++it) {
                stack.push_back(*it);
            }
        }

        el->Release();
    }
    /* Drain remaining stack entries in case of early termination. */
    for (auto *e : stack) e->Release();

    BYTE *buf = (BYTE*)malloc(out.size() ? out.size() : 1);
    if (!buf) return -1;
    if (out.size()) memcpy(buf, out.data(), out.size());
    *out_buf = buf;
    *out_size = (unsigned long)out.size();
    return 0;
}

/* ------------------------------------------------------------------ */
/* ELEMENT_AT — hit-test                                              */
/* ------------------------------------------------------------------ */

extern "C" int uia_element_at(int x, int y, char **out_row) {
    if (!g_available || !g_state) return -1;
    POINT pt{ x, y };
    ComPtr<IUIAutomationElement> el;
    if (FAILED(g_uia->ElementFromPoint(pt, el.GetAddressOf())) || !el) {
        return -1;
    }
    /* Refuse to hand callers a handle to our own UI - they'd be one
       click away from killing us. */
    if (element_is_own_process(el.Get())) return -1;
    int id = register_element(el.Get());
    if (id < 0) return -1;

    char buf[1024];
    int n = format_row(el.Get(), id, buf, sizeof(buf));
    if (n <= 0) return -1;
    char *r = (char*)malloc((size_t)n + 1);
    if (!r) return -1;
    memcpy(r, buf, (size_t)n);
    r[n] = 0;
    *out_row = r;
    return 0;
}

/* ------------------------------------------------------------------ */
/* ELEMENT_FIND — first match by role + name substring                */
/* ------------------------------------------------------------------ */

static bool ascii_contains_ci(const char *hay, const char *needle) {
    size_t hn = strlen(hay), nn = strlen(needle);
    if (nn == 0) return true;
    if (nn > hn) return false;
    for (size_t i = 0; i + nn <= hn; i++) {
        bool ok = true;
        for (size_t j = 0; j < nn; j++) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

extern "C" int uia_element_find(const char *role_token, const char *name_substr,
                                char **out_row) {
    if (!g_available || !g_state || !role_token) return -1;

    ComPtr<IUIAutomationElement> root;
    if (FAILED(g_uia->GetRootElement(root.GetAddressOf())) || !root) return -1;
    ComPtr<IUIAutomationTreeWalker> walker;
    if (FAILED(g_uia->get_ControlViewWalker(walker.GetAddressOf())) || !walker)
        return -1;

    std::vector<IUIAutomationElement*> stack;
    root->AddRef();
    stack.push_back(root.Get());

    IUIAutomationElement *found = nullptr;
    constexpr int SCAN_LIMIT = 5000;
    int scanned = 0;
    while (!stack.empty() && !found && scanned < SCAN_LIMIT) {
        IUIAutomationElement *el = stack.back();
        stack.pop_back();
        scanned++;

        /* Skip our own process's subtree - same self-foot-shooting guard
           as ELEMENTS. */
        if (element_is_own_process(el)) {
            el->Release();
            continue;
        }

        CONTROLTYPEID ct = 0;
        el->get_CurrentControlType(&ct);
        if (strcmp(role_name_for(ct), role_token) == 0) {
            BSTR name = NULL;
            el->get_CurrentName(&name);
            char buf[256] = "";
            bstr_to_chars(name, buf, sizeof(buf));
            if (name) SysFreeString(name);
            if (!name_substr || !*name_substr || ascii_contains_ci(buf, name_substr)) {
                found = el;  /* keep ref */
                break;
            }
        }

        ComPtr<IUIAutomationElement> child;
        if (SUCCEEDED(walker->GetFirstChildElement(el, child.GetAddressOf())) && child) {
            std::vector<IUIAutomationElement*> children;
            children.push_back(child.Detach());
            while (true) {
                IUIAutomationElement *prev = children.back();
                ComPtr<IUIAutomationElement> sibling;
                if (FAILED(walker->GetNextSiblingElement(prev, sibling.GetAddressOf()))
                    || !sibling) break;
                children.push_back(sibling.Detach());
            }
            for (auto it = children.rbegin(); it != children.rend(); ++it) {
                stack.push_back(*it);
            }
        }

        if (el != found) el->Release();
    }
    for (auto *e : stack) if (e != found) e->Release();

    if (!found) return -1;

    int id = register_element(found);
    found->Release();  /* register_element AddRef'd; this drops our local ref */
    if (id < 0) return -1;

    IUIAutomationElement *el_for_row = lookup_element(id);
    char buf[1024];
    int n = format_row(el_for_row, id, buf, sizeof(buf));
    if (n <= 0) return -1;
    char *r = (char*)malloc((size_t)n + 1);
    if (!r) return -1;
    memcpy(r, buf, (size_t)n);
    r[n] = 0;
    *out_row = r;
    return 0;
}

/* ------------------------------------------------------------------ */
/* ELEMENT_INVOKE / ELEMENT_FOCUS                                     */
/* ------------------------------------------------------------------ */

extern "C" int uia_element_invoke(int id) {
    if (!g_available) return -1;
    IUIAutomationElement *el = lookup_element(id);
    if (!el) return -2;
    ComPtr<IUIAutomationInvokePattern> invoke;
    if (FAILED(el->GetCurrentPatternAs(UIA_InvokePatternId,
            __uuidof(IUIAutomationInvokePattern),
            (void**)invoke.GetAddressOf())) || !invoke) {
        return -3;
    }
    return SUCCEEDED(invoke->Invoke()) ? 0 : -1;
}

extern "C" int uia_element_focus(int id) {
    if (!g_available) return -1;
    IUIAutomationElement *el = lookup_element(id);
    if (!el) return -2;
    return SUCCEEDED(el->SetFocus()) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* ELEMENT_TREE — recursive descent with depth column                 */
/* ------------------------------------------------------------------ */

extern "C" int uia_element_tree(int id, unsigned char **out_buf,
                                unsigned long *out_size) {
    if (!g_available || !g_state) return -1;
    IUIAutomationElement *root = lookup_element(id);
    if (!root) return -2;

    ComPtr<IUIAutomationTreeWalker> walker;
    if (FAILED(g_uia->get_ControlViewWalker(walker.GetAddressOf())) || !walker)
        return -1;

    std::string out;
    out.reserve(8 * 1024);
    char row[1024];
    char prefixed[1100];

    /* DFS frame: (element, depth). element is borrowed for the root
       (already in the map) and AddRef'd for newly-discovered children
       so they survive walker traversal. */
    struct frame { IUIAutomationElement *el; int depth; bool owned; };
    std::vector<frame> stack;
    stack.push_back({root, 0, false});

    constexpr int MAX_NODES = 5000;
    int emitted = 0;

    while (!stack.empty() && emitted < MAX_NODES) {
        frame f = stack.back();
        stack.pop_back();

        /* Skip the agent's own subtree - same self-shooting guard as
           ELEMENTS / ELEMENT_FIND. */
        if (element_is_own_process(f.el)) {
            if (f.owned) f.el->Release();
            continue;
        }

        /* Emit row for f. The root is already registered (it's the id
           the caller passed in); children get new ids. */
        int row_id = id;
        if (f.depth > 0) {
            row_id = register_element(f.el);
            if (row_id < 0) { if (f.owned) f.el->Release(); continue; }
        }
        int n = format_row(f.el, row_id, row, sizeof(row));
        if (n > 0) {
            int p = _snprintf_s(prefixed, sizeof(prefixed), _TRUNCATE,
                                "%d\t%s\n", f.depth, row);
            if (p > 0) { out.append(prefixed, (size_t)p); emitted++; }
        }

        /* Push children in reverse so first child is processed first. */
        ComPtr<IUIAutomationElement> child;
        if (SUCCEEDED(walker->GetFirstChildElement(f.el, child.GetAddressOf())) && child) {
            std::vector<IUIAutomationElement*> kids;
            kids.push_back(child.Detach());
            while (true) {
                IUIAutomationElement *prev = kids.back();
                ComPtr<IUIAutomationElement> sib;
                if (FAILED(walker->GetNextSiblingElement(prev, sib.GetAddressOf()))
                    || !sib) break;
                kids.push_back(sib.Detach());
            }
            for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
                stack.push_back({*it, f.depth + 1, true});
            }
        }

        if (f.owned) f.el->Release();
    }
    for (auto &f : stack) if (f.owned) f.el->Release();

    BYTE *buf = (BYTE*)malloc(out.size() ? out.size() : 1);
    if (!buf) return -1;
    if (out.size()) memcpy(buf, out.data(), out.size());
    *out_buf = buf;
    *out_size = (unsigned long)out.size();
    return 0;
}

/* ------------------------------------------------------------------ */
/* ELEMENT_TEXT / ELEMENT_SET_TEXT                                    */
/* ------------------------------------------------------------------ */

extern "C" int uia_element_text(int id, unsigned char **out_buf,
                                unsigned long *out_size) {
    if (!g_available) return -1;
    IUIAutomationElement *el = lookup_element(id);
    if (!el) return -2;

    BSTR text = NULL;

    /* TextPattern: full document text. */
    ComPtr<IUIAutomationTextPattern> tp;
    if (SUCCEEDED(el->GetCurrentPatternAs(UIA_TextPatternId,
            __uuidof(IUIAutomationTextPattern), (void**)tp.GetAddressOf()))
        && tp) {
        ComPtr<IUIAutomationTextRange> range;
        if (SUCCEEDED(tp->get_DocumentRange(range.GetAddressOf())) && range) {
            range->GetText(-1, &text);  /* -1 = no length cap */
        }
    }

    /* ValuePattern fallback. */
    if (!text) {
        ComPtr<IUIAutomationValuePattern> vp;
        if (SUCCEEDED(el->GetCurrentPatternAs(UIA_ValuePatternId,
                __uuidof(IUIAutomationValuePattern), (void**)vp.GetAddressOf()))
            && vp) {
            vp->get_CurrentValue(&text);
        }
    }

    /* Name fallback. */
    if (!text) {
        el->get_CurrentName(&text);
    }

    if (!text) {
        /* Element exposes no text-bearing surface — return empty body
           rather than ERR. The caller can distinguish "empty content"
           from "no text" via context. */
        BYTE *empty = (BYTE*)malloc(1);
        if (!empty) return -1;
        *out_buf = empty;
        *out_size = 0;
        return 0;
    }

    /* BSTR (UTF-16) → ANSI for the wire. */
    int needed = WideCharToMultiByte(CP_ACP, 0, text, -1, NULL, 0, NULL, NULL);
    if (needed <= 0) { SysFreeString(text); return -1; }
    BYTE *buf = (BYTE*)malloc((size_t)needed);
    if (!buf) { SysFreeString(text); return -1; }
    int written = WideCharToMultiByte(CP_ACP, 0, text, -1,
                                      (LPSTR)buf, needed, NULL, NULL);
    SysFreeString(text);
    if (written <= 0) { free(buf); return -1; }
    /* Drop the trailing NUL — wire payloads are length-prefixed, no
       sentinel needed. */
    *out_buf = buf;
    *out_size = (unsigned long)(written - 1);
    return 0;
}

extern "C" int uia_element_set_text(int id, const unsigned char *bytes,
                                    unsigned long len) {
    if (!g_available) return -1;
    IUIAutomationElement *el = lookup_element(id);
    if (!el) return -2;
    ComPtr<IUIAutomationValuePattern> vp;
    if (FAILED(el->GetCurrentPatternAs(UIA_ValuePatternId,
            __uuidof(IUIAutomationValuePattern), (void**)vp.GetAddressOf()))
        || !vp) {
        return -3;
    }

    /* ANSI → UTF-16 (BSTR). The MBToWC family handles a byte buffer with
       no trailing NUL when given an explicit length. */
    int wlen = MultiByteToWideChar(CP_ACP, 0, (LPCSTR)bytes, (int)len, NULL, 0);
    if (wlen < 0) return -1;
    BSTR str = SysAllocStringLen(NULL, (UINT)wlen);
    if (!str) return -1;
    if (wlen > 0) {
        MultiByteToWideChar(CP_ACP, 0, (LPCSTR)bytes, (int)len, str, wlen);
    }
    HRESULT hr = vp->SetValue(str);
    SysFreeString(str);
    return SUCCEEDED(hr) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* ELEMENT_TOGGLE / ELEMENT_EXPAND / ELEMENT_COLLAPSE                  */
/* ------------------------------------------------------------------ */

extern "C" int uia_element_toggle(int id) {
    if (!g_available) return -1;
    IUIAutomationElement *el = lookup_element(id);
    if (!el) return -2;
    ComPtr<IUIAutomationTogglePattern> tp;
    if (FAILED(el->GetCurrentPatternAs(UIA_TogglePatternId,
            __uuidof(IUIAutomationTogglePattern), (void**)tp.GetAddressOf()))
        || !tp) {
        return -3;
    }
    return SUCCEEDED(tp->Toggle()) ? 0 : -1;
}

static int expand_collapse(int id, bool expand) {
    if (!g_available) return -1;
    IUIAutomationElement *el = lookup_element(id);
    if (!el) return -2;
    ComPtr<IUIAutomationExpandCollapsePattern> ep;
    if (FAILED(el->GetCurrentPatternAs(UIA_ExpandCollapsePatternId,
            __uuidof(IUIAutomationExpandCollapsePattern),
            (void**)ep.GetAddressOf()))
        || !ep) {
        return -3;
    }
    HRESULT hr = expand ? ep->Expand() : ep->Collapse();
    return SUCCEEDED(hr) ? 0 : -1;
}

extern "C" int uia_element_expand(int id)   { return expand_collapse(id, true); }
extern "C" int uia_element_collapse(int id) { return expand_collapse(id, false); }
