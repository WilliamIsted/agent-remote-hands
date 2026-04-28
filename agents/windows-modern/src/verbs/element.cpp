//   Copyright 2026 William Isted and contributors
//
//   Licensed under the Apache License, Version 2.0 (the "License");
//   you may not use this file except in compliance with the License.
//   You may obtain a copy of the License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
//   Unless required by applicable law or agreed to in writing, software
//   distributed under the License is distributed on an "AS IS" BASIS,
//   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//   See the License for the specific language governing permissions and
//   limitations under the License.

// `element.*` namespace verb handlers.
//
// Implements PROTOCOL.md §4.5 — UI Automation surface, eleven verbs:
//   element.list, element.tree, element.at, element.find,
//   element.invoke, element.toggle, element.expand, element.collapse,
//   element.focus, element.text, element.set_text.
//
// Element ids are connection-scoped sequential integers (`elt:N`) managed by
// ElementTable. The table owns one COM ref per element; this file uses
// borrowed pointers via lookup() and AddRefs through register_element().
//
// Closes: #44 (umbrella), #50, #51, #52, #53, #54.

#include "../connection.hpp"
#include "../element_table.hpp"
#include "../errors.hpp"
#include "../json.hpp"
#include "../log.hpp"
#include "../text_util.hpp"
#include "../uipi.hpp"

#include <cctype>
#include <charconv>
#include <cstdio>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <UIAutomation.h>
#include <wrl/client.h>

namespace remote_hands::element_verbs {

using Microsoft::WRL::ComPtr;

namespace {

// ---------------------------------------------------------------------------
// Argument parsing helpers

bool parse_int(std::string_view s, int& out) {
    const auto* end = s.data() + s.size();
    long v = 0;
    const auto [p, ec] = std::from_chars(s.data(), end, v, 10);
    if (ec != std::errc{} || p != end) return false;
    out = static_cast<int>(v);
    return true;
}

bool parse_region(std::string_view s, RECT& out) {
    // x,y,w,h
    int x = 0, y = 0, w = 0, h = 0;
    std::size_t cursor = 0;
    auto take = [&](int& v) {
        const std::size_t comma = s.find(',', cursor);
        const std::size_t end_pos = (comma == std::string_view::npos) ? s.size() : comma;
        const auto* p_end = s.data() + end_pos;
        long parsed = 0;
        const auto [p, ec] = std::from_chars(s.data() + cursor, p_end, parsed, 10);
        if (ec != std::errc{} || p != p_end) return false;
        v = static_cast<int>(parsed);
        cursor = end_pos + 1;
        return true;
    };
    if (!take(x) || !take(y) || !take(w) || !take(h)) return false;
    out.left   = x;
    out.top    = y;
    out.right  = x + w;
    out.bottom = y + h;
    return true;
}

// ---------------------------------------------------------------------------
// UIA → wire mapping

const char* role_token(LONG control_type) {
    switch (control_type) {
        case UIA_ButtonControlTypeId:        return "button";
        case UIA_CalendarControlTypeId:      return "calendar";
        case UIA_CheckBoxControlTypeId:      return "checkbox";
        case UIA_ComboBoxControlTypeId:      return "combobox";
        case UIA_EditControlTypeId:          return "edit";
        case UIA_HyperlinkControlTypeId:     return "link";
        case UIA_ImageControlTypeId:         return "image";
        case UIA_ListItemControlTypeId:      return "listitem";
        case UIA_ListControlTypeId:          return "list";
        case UIA_MenuControlTypeId:          return "menu";
        case UIA_MenuBarControlTypeId:       return "menubar";
        case UIA_MenuItemControlTypeId:      return "menuitem";
        case UIA_ProgressBarControlTypeId:   return "progressbar";
        case UIA_RadioButtonControlTypeId:   return "radiobutton";
        case UIA_ScrollBarControlTypeId:     return "scrollbar";
        case UIA_SliderControlTypeId:        return "slider";
        case UIA_SpinnerControlTypeId:       return "spinner";
        case UIA_StatusBarControlTypeId:     return "statusbar";
        case UIA_TabControlTypeId:           return "tab";
        case UIA_TabItemControlTypeId:       return "tabitem";
        case UIA_TextControlTypeId:          return "text";
        case UIA_ToolBarControlTypeId:       return "toolbar";
        case UIA_ToolTipControlTypeId:       return "tooltip";
        case UIA_TreeControlTypeId:          return "tree";
        case UIA_TreeItemControlTypeId:      return "treeitem";
        case UIA_CustomControlTypeId:        return "custom";
        case UIA_GroupControlTypeId:         return "group";
        case UIA_ThumbControlTypeId:         return "thumb";
        case UIA_DataGridControlTypeId:      return "datagrid";
        case UIA_DataItemControlTypeId:      return "dataitem";
        case UIA_DocumentControlTypeId:      return "document";
        case UIA_SplitButtonControlTypeId:   return "splitbutton";
        case UIA_WindowControlTypeId:        return "window";
        case UIA_PaneControlTypeId:          return "pane";
        case UIA_HeaderControlTypeId:        return "header";
        case UIA_HeaderItemControlTypeId:    return "headeritem";
        case UIA_TableControlTypeId:         return "table";
        case UIA_TitleBarControlTypeId:      return "titlebar";
        case UIA_SeparatorControlTypeId:     return "separator";
        default:                             return "unknown";
    }
}

std::string bstr_to_utf8(BSTR b) {
    if (!b) return {};
    const UINT len = SysStringLen(b);
    return text::wide_to_utf8(b, len);
}

// Reads the element's flag bits using cheap-ish properties.
std::vector<std::string> element_flags(IUIAutomationElement* elem) {
    std::vector<std::string> out;

    BOOL b = FALSE;
    if (SUCCEEDED(elem->get_CurrentIsEnabled(&b))         && b) out.emplace_back("enabled");
    if (SUCCEEDED(elem->get_CurrentHasKeyboardFocus(&b))  && b) out.emplace_back("focused");
    if (SUCCEEDED(elem->get_CurrentIsOffscreen(&b))       && b) out.emplace_back("offscreen");
    if (SUCCEEDED(elem->get_CurrentIsPassword(&b))        && b) out.emplace_back("password");

    // Pattern-derived flags. QI can fail; treat absence as flag-not-applicable.
    {
        ComPtr<IUIAutomationSelectionItemPattern> sip;
        if (SUCCEEDED(elem->GetCurrentPatternAs(
                UIA_SelectionItemPatternId, IID_PPV_ARGS(&sip))) && sip) {
            BOOL sel = FALSE;
            if (SUCCEEDED(sip->get_CurrentIsSelected(&sel)) && sel) {
                out.emplace_back("selected");
            }
        }
    }
    {
        ComPtr<IUIAutomationTogglePattern> tp;
        if (SUCCEEDED(elem->GetCurrentPatternAs(
                UIA_TogglePatternId, IID_PPV_ARGS(&tp))) && tp) {
            ToggleState st = ToggleState_Off;
            if (SUCCEEDED(tp->get_CurrentToggleState(&st)) && st == ToggleState_On) {
                out.emplace_back("checked");
            }
        }
    }
    {
        ComPtr<IUIAutomationExpandCollapsePattern> ecp;
        if (SUCCEEDED(elem->GetCurrentPatternAs(
                UIA_ExpandCollapsePatternId, IID_PPV_ARGS(&ecp))) && ecp) {
            ExpandCollapseState st = ExpandCollapseState_Collapsed;
            if (SUCCEEDED(ecp->get_CurrentExpandCollapseState(&st)) &&
                st == ExpandCollapseState_Expanded) {
                out.emplace_back("expanded");
            }
        }
    }

    return out;
}

std::string value_pattern_value(IUIAutomationElement* elem) {
    ComPtr<IUIAutomationValuePattern> vp;
    if (FAILED(elem->GetCurrentPatternAs(
            UIA_ValuePatternId, IID_PPV_ARGS(&vp))) || !vp) {
        return {};
    }
    BSTR v = nullptr;
    if (FAILED(vp->get_CurrentValue(&v)) || !v) return {};
    std::string out = bstr_to_utf8(v);
    SysFreeString(v);
    return out;
}

// Appends `{"id":"elt:N","role":"...","name":"...","value":"...",
//          "bounds":[x,y,w,h],"flags":[...]}` to `out`.
void append_element_object(std::string& out,
                           const std::string& id,
                           IUIAutomationElement* elem) {
    LONG ctype = UIA_CustomControlTypeId;
    elem->get_CurrentControlType(&ctype);

    BSTR name_bstr = nullptr;
    elem->get_CurrentName(&name_bstr);
    const std::string name = bstr_to_utf8(name_bstr);
    if (name_bstr) SysFreeString(name_bstr);

    const std::string value = value_pattern_value(elem);

    RECT rc{};
    elem->get_CurrentBoundingRectangle(&rc);

    out += '{';
    json::append_kv_string(out, "id", id);                       out += ',';
    json::append_kv_string(out, "role", role_token(ctype));      out += ',';
    json::append_kv_string(out, "name", name);                   out += ',';
    json::append_kv_string(out, "value", value);                 out += ',';
    json::append_string(out, "bounds");
    char bbuf[64];
    std::snprintf(bbuf, sizeof(bbuf), ":[%ld,%ld,%ld,%ld],",
                  rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
    out += bbuf;
    json::append_string_array(out, "flags", element_flags(elem));
    out += '}';
}

// ---------------------------------------------------------------------------
// Tree-walk plumbing

ComPtr<IUIAutomationCondition> build_visible_condition(IUIAutomation* uia) {
    // (IsControlElement = TRUE) AND (IsOffscreen = FALSE)
    VARIANT vtrue;  vtrue.vt = VT_BOOL;  vtrue.boolVal = VARIANT_TRUE;
    VARIANT vfalse; vfalse.vt = VT_BOOL; vfalse.boolVal = VARIANT_FALSE;

    ComPtr<IUIAutomationCondition> is_control;
    uia->CreatePropertyCondition(UIA_IsControlElementPropertyId, vtrue, &is_control);
    ComPtr<IUIAutomationCondition> not_offscreen;
    uia->CreatePropertyCondition(UIA_IsOffscreenPropertyId, vfalse, &not_offscreen);

    ComPtr<IUIAutomationCondition> combined;
    if (is_control && not_offscreen) {
        uia->CreateAndCondition(is_control.Get(), not_offscreen.Get(), &combined);
    }
    return combined;
}

void walk_subtree(IUIAutomationTreeWalker* walker,
                  IUIAutomationElement* parent,
                  ElementTable& table,
                  std::string& out,
                  bool& first,
                  int depth,
                  int max_depth) {
    if (depth > max_depth) return;

    ComPtr<IUIAutomationElement> child;
    walker->GetFirstChildElement(parent, &child);
    while (child) {
        const auto id = table.register_element(child.Get());

        if (!first) out += ',';
        first = false;
        out += '{';
        json::append_kv_int(out, "depth", depth);
        out += ',';
        // The element object body without the leading `{` and trailing `}`.
        // append_element_object writes a complete object — embed by stripping braces.
        std::string per_elem;
        append_element_object(per_elem, id, child.Get());
        // Drop leading '{' and trailing '}' from per_elem.
        out.append(per_elem.data() + 1, per_elem.size() - 2);
        out += '}';

        walk_subtree(walker, child.Get(), table, out, first, depth + 1, max_depth);

        ComPtr<IUIAutomationElement> sibling;
        walker->GetNextSiblingElement(child.Get(), &sibling);
        child = std::move(sibling);
    }
}

// ---------------------------------------------------------------------------
// Common preamble: get UIA singleton or fail.

IUIAutomation* require_uia(Connection& conn) {
    IUIAutomation* uia = conn.element_table().uia();
    if (!uia) {
        conn.writer().write_err(
            ErrorCode::NotSupported,
            "{\"reason\":\"IUIAutomation unavailable\"}");
    }
    return uia;
}

IUIAutomationElement* require_element(Connection& conn, std::string_view id) {
    IUIAutomationElement* elem = conn.element_table().lookup(id);
    if (!elem) {
        std::string detail = "{";
        json::append_kv_string(detail, "handle", id);
        detail += '}';
        conn.writer().write_err(ErrorCode::TargetGone, detail);
        return nullptr;
    }
    return elem;
}

}  // namespace

// ---------------------------------------------------------------------------
// element.list

void list(Connection& conn, const wire::Request& req) {
    IUIAutomation* uia = require_uia(conn);
    if (!uia) return;

    RECT region{};
    bool has_region = false;
    for (std::size_t i = 0; i < req.args.size(); ++i) {
        if (req.args[i] == "--region" && i + 1 < req.args.size()) {
            if (!parse_region(req.args[++i], region)) {
                conn.writer().write_err(
                    ErrorCode::InvalidArgs,
                    "{\"message\":\"--region must be x,y,w,h\"}");
                return;
            }
            has_region = true;
        }
    }

    ComPtr<IUIAutomationElement> root;
    if (FAILED(uia->GetRootElement(&root)) || !root) {
        conn.writer().write_err(ErrorCode::NotSupported);
        return;
    }

    auto cond = build_visible_condition(uia);
    if (!cond) {
        conn.writer().write_err(ErrorCode::NotSupported);
        return;
    }

    ComPtr<IUIAutomationElementArray> arr;
    if (FAILED(root->FindAll(TreeScope_Subtree, cond.Get(), &arr)) || !arr) {
        conn.writer().write_err(ErrorCode::NotSupported);
        return;
    }

    int count = 0;
    arr->get_Length(&count);

    std::string body = "{\"elements\":[";
    bool first = true;
    for (int i = 0; i < count; ++i) {
        ComPtr<IUIAutomationElement> elem;
        if (FAILED(arr->GetElement(i, &elem)) || !elem) continue;

        if (has_region) {
            RECT rc{};
            if (FAILED(elem->get_CurrentBoundingRectangle(&rc))) continue;
            // Reject if entirely outside region.
            if (rc.right <= region.left || rc.left >= region.right ||
                rc.bottom <= region.top || rc.top >= region.bottom) {
                continue;
            }
        }

        const auto id = conn.element_table().register_element(elem.Get());
        if (!first) body += ',';
        first = false;
        append_element_object(body, id, elem.Get());
    }
    body += "]}";
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// element.tree

void tree(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 1) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"element.tree requires <elt-id>\"}");
        return;
    }
    IUIAutomation* uia = require_uia(conn);
    if (!uia) return;
    IUIAutomationElement* root = require_element(conn, req.args[0]);
    if (!root) return;

    ComPtr<IUIAutomationTreeWalker> walker;
    uia->get_ContentViewWalker(&walker);
    if (!walker) {
        conn.writer().write_err(ErrorCode::NotSupported);
        return;
    }

    std::string body = "{\"elements\":[";
    // Include the root itself at depth 0.
    {
        const auto id = conn.element_table().register_element(root);
        body += '{';
        json::append_kv_int(body, "depth", 0);
        body += ',';
        std::string per_elem;
        append_element_object(per_elem, id, root);
        body.append(per_elem.data() + 1, per_elem.size() - 2);
        body += '}';
    }
    bool first = false;
    walk_subtree(walker.Get(), root, conn.element_table(),
                 body, first, /*depth=*/1, /*max_depth=*/12);
    body += "]}";
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// element.at

void at(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 2) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"element.at requires <x> <y>\"}");
        return;
    }
    int x = 0, y = 0;
    if (!parse_int(req.args[0], x) || !parse_int(req.args[1], y)) {
        conn.writer().write_err(ErrorCode::InvalidArgs,
                                "{\"message\":\"x and y must be integers\"}");
        return;
    }
    IUIAutomation* uia = require_uia(conn);
    if (!uia) return;

    POINT pt{x, y};
    ComPtr<IUIAutomationElement> elem;
    if (FAILED(uia->ElementFromPoint(pt, &elem)) || !elem) {
        conn.writer().write_err(ErrorCode::NotFound);
        return;
    }

    const auto id = conn.element_table().register_element(elem.Get());
    std::string body;
    append_element_object(body, id, elem.Get());
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// element.find

void find(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 2) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"element.find requires <role> <name-pattern>\"}");
        return;
    }
    IUIAutomation* uia = require_uia(conn);
    if (!uia) return;

    const std::string& role_arg = req.args[0];
    const std::string& name_arg = req.args[1];

    // Linear scan; case-insensitive substring match on Name.
    ComPtr<IUIAutomationElement> root;
    if (FAILED(uia->GetRootElement(&root)) || !root) {
        conn.writer().write_err(ErrorCode::NotSupported);
        return;
    }

    auto cond = build_visible_condition(uia);
    if (!cond) {
        conn.writer().write_err(ErrorCode::NotSupported);
        return;
    }

    ComPtr<IUIAutomationElementArray> arr;
    if (FAILED(root->FindAll(TreeScope_Subtree, cond.Get(), &arr)) || !arr) {
        conn.writer().write_err(ErrorCode::NotSupported);
        return;
    }

    auto needle = name_arg;
    for (char& c : needle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    int count = 0;
    arr->get_Length(&count);
    for (int i = 0; i < count; ++i) {
        ComPtr<IUIAutomationElement> elem;
        if (FAILED(arr->GetElement(i, &elem)) || !elem) continue;

        LONG ctype = 0;
        if (FAILED(elem->get_CurrentControlType(&ctype))) continue;
        if (role_arg != role_token(ctype)) continue;

        BSTR name_bstr = nullptr;
        elem->get_CurrentName(&name_bstr);
        std::string name = bstr_to_utf8(name_bstr);
        if (name_bstr) SysFreeString(name_bstr);

        std::string lname = name;
        for (char& c : lname) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lname.find(needle) == std::string::npos) continue;

        // Match.
        const auto id = conn.element_table().register_element(elem.Get());
        std::string body;
        append_element_object(body, id, elem.Get());
        conn.writer().write_ok(body);
        return;
    }

    // Nothing matched. Distinguish `not_found` from `uia_blind`: if the
    // foreground window is at a higher integrity level than the agent, UIA
    // likely cannot see across the barrier.
    HWND fg = GetForegroundWindow();
    const auto target_il = uipi::window_integrity(fg);
    const auto& self_il  = uipi::agent_integrity();
    if (!uipi::input_allowed(self_il, target_il)) {
        std::string detail = "{";
        json::append_kv_string(detail, "agent_il", self_il);   detail += ',';
        json::append_kv_string(detail, "target_il", target_il);
        detail += '}';
        conn.writer().write_err(ErrorCode::UiaBlind, detail);
        return;
    }
    conn.writer().write_err(ErrorCode::NotFound);
}

// ---------------------------------------------------------------------------
// element.invoke

void invoke(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 1) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"element.invoke requires <elt-id>\"}");
        return;
    }
    IUIAutomationElement* elem = require_element(conn, req.args[0]);
    if (!elem) return;

    ComPtr<IUIAutomationInvokePattern> ip;
    if (FAILED(elem->GetCurrentPatternAs(
            UIA_InvokePatternId, IID_PPV_ARGS(&ip))) || !ip) {
        conn.writer().write_err(
            ErrorCode::NotSupportedByTarget,
            "{\"pattern\":\"InvokePattern\"}");
        return;
    }

    const HRESULT hr = ip->Invoke();
    if (hr == UIA_E_ELEMENTNOTAVAILABLE) {
        conn.writer().write_err(ErrorCode::TargetGone);
        return;
    }
    if (FAILED(hr)) {
        char detail[64];
        std::snprintf(detail, sizeof(detail),
                      "{\"hresult\":\"0x%08lx\"}", hr);
        conn.writer().write_err(ErrorCode::NotSupported, detail);
        return;
    }
    conn.writer().write_ok();
}

// ---------------------------------------------------------------------------
// element.toggle

void toggle(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 1) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"element.toggle requires <elt-id>\"}");
        return;
    }
    IUIAutomationElement* elem = require_element(conn, req.args[0]);
    if (!elem) return;

    ComPtr<IUIAutomationTogglePattern> tp;
    if (FAILED(elem->GetCurrentPatternAs(
            UIA_TogglePatternId, IID_PPV_ARGS(&tp))) || !tp) {
        conn.writer().write_err(
            ErrorCode::NotSupportedByTarget,
            "{\"pattern\":\"TogglePattern\"}");
        return;
    }

    const HRESULT hr = tp->Toggle();
    if (hr == UIA_E_ELEMENTNOTAVAILABLE) {
        conn.writer().write_err(ErrorCode::TargetGone);
        return;
    }
    if (FAILED(hr)) {
        conn.writer().write_err(ErrorCode::NotSupported);
        return;
    }

    ToggleState st = ToggleState_Off;
    tp->get_CurrentToggleState(&st);
    const char* name = (st == ToggleState_On) ? "on"
                     : (st == ToggleState_Off) ? "off"
                     : "indeterminate";
    std::string body = "{";
    json::append_kv_string(body, "new_state", name);
    body += '}';
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// element.expand / element.collapse

namespace {

void do_expand_collapse(Connection& conn, const wire::Request& req, bool expand) {
    if (req.args.size() != 1) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"element.expand/collapse requires <elt-id>\"}");
        return;
    }
    IUIAutomationElement* elem = require_element(conn, req.args[0]);
    if (!elem) return;

    ComPtr<IUIAutomationExpandCollapsePattern> ecp;
    if (FAILED(elem->GetCurrentPatternAs(
            UIA_ExpandCollapsePatternId, IID_PPV_ARGS(&ecp))) || !ecp) {
        conn.writer().write_err(
            ErrorCode::NotSupportedByTarget,
            "{\"pattern\":\"ExpandCollapsePattern\"}");
        return;
    }

    const HRESULT hr = expand ? ecp->Expand() : ecp->Collapse();
    if (hr == UIA_E_ELEMENTNOTAVAILABLE) {
        conn.writer().write_err(ErrorCode::TargetGone);
        return;
    }
    if (FAILED(hr)) {
        conn.writer().write_err(ErrorCode::NotSupported);
        return;
    }

    ExpandCollapseState st = ExpandCollapseState_Collapsed;
    ecp->get_CurrentExpandCollapseState(&st);
    const char* name = (st == ExpandCollapseState_Expanded)        ? "expanded"
                     : (st == ExpandCollapseState_Collapsed)       ? "collapsed"
                     : (st == ExpandCollapseState_PartiallyExpanded)? "partially_expanded"
                     : "leaf";
    std::string body = "{";
    json::append_kv_string(body, "new_state", name);
    body += '}';
    conn.writer().write_ok(body);
}

}  // namespace

void expand(Connection& conn, const wire::Request& req) {
    do_expand_collapse(conn, req, true);
}

void collapse(Connection& conn, const wire::Request& req) {
    do_expand_collapse(conn, req, false);
}

// ---------------------------------------------------------------------------
// element.focus

void focus(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 1) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"element.focus requires <elt-id>\"}");
        return;
    }
    IUIAutomationElement* elem = require_element(conn, req.args[0]);
    if (!elem) return;

    const HRESULT hr = elem->SetFocus();
    if (hr == UIA_E_ELEMENTNOTAVAILABLE) {
        conn.writer().write_err(ErrorCode::TargetGone);
        return;
    }
    if (FAILED(hr)) {
        conn.writer().write_err(ErrorCode::NotSupported);
        return;
    }
    conn.writer().write_ok();
}

// ---------------------------------------------------------------------------
// element.text — TextPattern → ValuePattern → Name fallback

void text(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 1) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"element.text requires <elt-id>\"}");
        return;
    }
    IUIAutomationElement* elem = require_element(conn, req.args[0]);
    if (!elem) return;

    // 1. TextPattern.DocumentRange().GetText(-1)
    {
        ComPtr<IUIAutomationTextPattern> tp;
        if (SUCCEEDED(elem->GetCurrentPatternAs(
                UIA_TextPatternId, IID_PPV_ARGS(&tp))) && tp) {
            ComPtr<IUIAutomationTextRange> range;
            if (SUCCEEDED(tp->get_DocumentRange(&range)) && range) {
                BSTR txt = nullptr;
                if (SUCCEEDED(range->GetText(-1, &txt)) && txt) {
                    std::string out = bstr_to_utf8(txt);
                    SysFreeString(txt);
                    conn.writer().write_ok(out);
                    return;
                }
            }
        }
    }

    // 2. ValuePattern.CurrentValue
    {
        std::string v = value_pattern_value(elem);
        if (!v.empty()) {
            conn.writer().write_ok(v);
            return;
        }
    }

    // 3. Name
    BSTR name_bstr = nullptr;
    elem->get_CurrentName(&name_bstr);
    std::string out = bstr_to_utf8(name_bstr);
    if (name_bstr) SysFreeString(name_bstr);
    conn.writer().write_ok(out);
}

// ---------------------------------------------------------------------------
// element.set_text

void set_text(Connection& conn, const wire::Request& req) {
    if (req.args.size() != 2) {
        conn.writer().write_err(
            ErrorCode::InvalidArgs,
            "{\"message\":\"element.set_text requires <elt-id> <length>\"}");
        return;
    }
    IUIAutomationElement* elem = require_element(conn, req.args[0]);
    if (!elem) return;

    unsigned long long length = 0;
    {
        const auto* end = req.args[1].data() + req.args[1].size();
        const auto [p, ec] = std::from_chars(req.args[1].data(), end, length, 10);
        if (ec != std::errc{} || p != end) {
            conn.writer().write_err(
                ErrorCode::InvalidArgs,
                "{\"message\":\"length must be a non-negative integer\"}");
            return;
        }
    }

    auto payload = conn.reader().read_payload(static_cast<std::size_t>(length));
    const std::wstring wtext = text::utf8_to_wide(
        std::string_view{reinterpret_cast<const char*>(payload.data()),
                         payload.size()});

    ComPtr<IUIAutomationValuePattern> vp;
    if (FAILED(elem->GetCurrentPatternAs(
            UIA_ValuePatternId, IID_PPV_ARGS(&vp))) || !vp) {
        conn.writer().write_err(
            ErrorCode::NotSupportedByTarget,
            "{\"pattern\":\"ValuePattern\"}");
        return;
    }

    BOOL readonly = FALSE;
    if (SUCCEEDED(vp->get_CurrentIsReadOnly(&readonly)) && readonly) {
        conn.writer().write_err(ErrorCode::Readonly);
        return;
    }

    BSTR bs = SysAllocStringLen(wtext.data(),
                                static_cast<UINT>(wtext.size()));
    const HRESULT hr = vp->SetValue(bs);
    if (bs) SysFreeString(bs);

    if (hr == UIA_E_ELEMENTNOTAVAILABLE) {
        conn.writer().write_err(ErrorCode::TargetGone);
        return;
    }
    if (FAILED(hr)) {
        char detail[64];
        std::snprintf(detail, sizeof(detail),
                      "{\"hresult\":\"0x%08lx\"}", hr);
        conn.writer().write_err(ErrorCode::NotSupported, detail);
        return;
    }
    conn.writer().write_ok();
}

}  // namespace remote_hands::element_verbs
