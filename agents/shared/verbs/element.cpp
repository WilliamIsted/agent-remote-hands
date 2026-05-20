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
// Implements the verbs whose contracts are the spec JSON under
// protocol/spec/verbs/windows/element.*.json (the single source of truth):
//   element.list        (R)  {region,limit,offset,full}
//   element.tree        (R)  {handle,depth,full}
//   element.at          (R)  {x,y}
//   element.find        (R)  {root,name,role,automation_id,timeout_ms}
//   element.wait        (R)  {root,name,role,automation_id,flags_required,
//                             timeout_ms}
//   element.find_invoke (U)  {root,name,role,automation_id,timeout_ms}
//   element.at_invoke   (U)  {x,y}
//   element.invoke      (U)  {handle}
//   element.toggle      (U)  {handle}
//   element.expand      (U)  {handle}
//   element.collapse    (U)  {handle}
//   element.focus       (U)  {handle}
//   element.text        (R)  {handle}
//   element.set_text    (U)  {handle,text}
//
// Element ids are connection-scoped sequential integers (`elt:N`) managed by
// ElementTable. The table owns one COM ref per element; this file uses
// borrowed pointers via lookup() and AddRefs through register_element().
//
// PHASE 2.1 — NAMED-ARG MIGRATION. These handlers no longer index `req.args`
// positionally with ad-hoc `--flag` scanning or hand-rolled element-handle /
// region multi-token reconstruction. Each declares its input_schema property
// list (IN SCHEMA ORDER) and reads each value by NAME through the shared
// SchemaArgs resolver (schema_args.hpp), with the schema's property ORDER
// used as the positional fallback when the caller invoked the verb
// positionally (the v2.2 reference client packs positional calls as
// `{"_args":[...]}`). The `window.*` namespace was the pattern-setter; this
// file follows it (and the six other Phase-2.1 slices) exactly. Validation
// emits the same ErrorCode::InvalidArgs + {"message":...} ergonomics.
//
// v2.2 fold-ins (per the spec JSON):
//   * #90 — element.find / element.wait / element.find_invoke gain
//     `automation_id` + `root` matcher inputs. `root` is an element-handle
//     (`elt:N`) scoping the FindAll subtree; absence => the desktop root.
//     `name` and `automation_id` are mutually exclusive per
//     x-mutually-exclusive.
//   * #92 — element.list / find / at / tree / wait results carry a `flags`
//     array of the spec's UIA boolean-state enum
//     (["enabled","focused","offscreen","password","required"]).
//     element.wait additionally takes `flags_required` (every listed flag
//     must be set on the matched element before the wait is satisfied).
//   * element.set_text `text` is now an inline UTF-8 STRING property — the
//     pre-Phase-2.1 separate length-prefixed wire payload (read off a
//     `<length>` arg via reader().read_payload()) is gone, mirroring the
//     clipboard.set / process.run slices.
//   * element.text's response is the structured object {text} (was a bare
//     text payload), per its x-output-schema.
//   * `bounds` is the spec `$defs/Bounds` OBJECT {x,y,w,h} (was a bare
//     [x,y,w,h] array — the strict x-output-schema declares an object).
//   * Each element object's id field is `handle` (was `id`) and carries
//     `automation_id` per the strict output schemas. `value` is NOT a
//     declared output property on any element verb (strict
//     additionalProperties:false) so it is no longer emitted.
//
// Element ids are advertised on the wire as `elt:N` STRINGS.

#include "../connection.hpp"
#include "../element_table.hpp"
#include "../errors.hpp"
#include "../json.hpp"
#include "../log.hpp"
#include "../text_util.hpp"
#include "../uipi.hpp"
#include "args.hpp"
#include "schema_args.hpp"

#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>          // Defines `interface` macro before UIA headers.
#include <UIAutomation.h>
#include <wrl/client.h>

namespace remote_hands::element_verbs {

using Microsoft::WRL::ComPtr;

// The Phase-2.1 named-argument resolver and its invalid_args helper live in
// the shared header (schema_args.hpp). Pull them into this TU's unqualified
// name lookup; behaviour is identical to the prior in-file definitions.
using wire::SchemaArgs;
using wire::invalid_args;

namespace {

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

std::string element_name(IUIAutomationElement* elem) {
    BSTR b = nullptr;
    elem->get_CurrentName(&b);
    std::string out = bstr_to_utf8(b);
    if (b) SysFreeString(b);
    return out;
}

std::string element_automation_id(IUIAutomationElement* elem) {
    BSTR b = nullptr;
    elem->get_CurrentAutomationId(&b);
    std::string out = bstr_to_utf8(b);
    if (b) SysFreeString(b);
    return out;
}

// Universally-determinable UIA boolean states (#92). Restricted to the spec
// x-output-schema flag enum: ["enabled","focused","offscreen","password",
// "required"]. Pattern-derived states (selected/checked/expanded) are NOT in
// the enum and so are no longer emitted — the strict output schema declares
// additionalProperties:false on the items and a fixed flag enum.
std::vector<std::string> element_flags(IUIAutomationElement* elem) {
    std::vector<std::string> out;

    BOOL b = FALSE;
    if (SUCCEEDED(elem->get_CurrentIsEnabled(&b))        && b) out.emplace_back("enabled");
    if (SUCCEEDED(elem->get_CurrentHasKeyboardFocus(&b)) && b) out.emplace_back("focused");
    if (SUCCEEDED(elem->get_CurrentIsOffscreen(&b))      && b) out.emplace_back("offscreen");
    if (SUCCEEDED(elem->get_CurrentIsPassword(&b))       && b) out.emplace_back("password");
    if (SUCCEEDED(elem->get_CurrentIsRequiredForForm(&b)) && b) out.emplace_back("required");

    return out;
}

// Appends the spec `$defs/Bounds` object `{ "x":.., "y":.., "w":.., "h":.. }`
// (NOT a bare array — the strict x-output-schema declares an object). Caller
// has already written the `"bounds":` key.
void append_bounds_object(std::string& out, const RECT& rc) {
    out += '{';
    json::append_kv_int(out, "x", rc.left);                 out += ',';
    json::append_kv_int(out, "y", rc.top);                  out += ',';
    json::append_kv_int(out, "w", rc.right - rc.left);      out += ',';
    json::append_kv_int(out, "h", rc.bottom - rc.top);
    out += '}';
}

// Full element object per the strict x-output-schema:
//   { handle, role, name, automation_id, bounds:{x,y,w,h}, flags:[...] }
// `flags` is included only when `with_flags` (the verb's `full`, or the
// always-on single-element shape for find/at/wait). `value` is NOT a declared
// property on any element verb's output schema, so it is never emitted.
void append_element_object(std::string& out,
                           const std::string& handle,
                           IUIAutomationElement* elem,
                           bool with_flags) {
    CONTROLTYPEID ctype = UIA_CustomControlTypeId;
    elem->get_CurrentControlType(&ctype);

    RECT rc{};
    elem->get_CurrentBoundingRectangle(&rc);

    out += '{';
    json::append_kv_string(out, "handle", handle);                  out += ',';
    json::append_kv_string(out, "role", role_token(ctype));         out += ',';
    json::append_kv_string(out, "name", element_name(elem));        out += ',';
    json::append_kv_string(out, "automation_id",
                           element_automation_id(elem));            out += ',';
    json::append_string(out, "bounds");
    out += ':';
    append_bounds_object(out, rc);
    if (with_flags) {
        out += ',';
        json::append_string_array(out, "flags", element_flags(elem));
    }
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

// Returns true if count_cap was reached (tree was truncated).
bool walk_subtree(IUIAutomationTreeWalker* walker,
                  IUIAutomationElement* parent,
                  ElementTable& table,
                  std::string& out,
                  bool& first,
                  int depth,
                  int max_depth,
                  bool full,
                  int& count,
                  int count_cap) {
    if (depth > max_depth) return false;

    ComPtr<IUIAutomationElement> child;
    walker->GetFirstChildElement(parent, &child);
    while (child) {
        if (count >= count_cap) return true;

        const auto handle = table.register_element(child.Get());

        if (!first) out += ',';
        first = false;
        out += '{';
        json::append_kv_int(out, "depth", depth);
        out += ',';
        std::string per_elem;
        append_element_object(per_elem, handle, child.Get(), full);
        // Splice the element object's body in after "depth": strip its
        // surrounding `{` `}`.
        out.append(per_elem.data() + 1, per_elem.size() - 2);
        out += '}';
        ++count;

        if (walk_subtree(walker, child.Get(), table, out, first,
                         depth + 1, max_depth, full, count, count_cap)) {
            return true;
        }

        ComPtr<IUIAutomationElement> sibling;
        walker->GetNextSiblingElement(child.Get(), &sibling);
        child = std::move(sibling);
    }
    return false;
}

// ---------------------------------------------------------------------------
// Common preamble: get UIA singleton or fail.
//
// `not_supported` is NOT in any element verb's x-errors. UIA being
// unavailable is an environment/access failure; `permission_denied` is the
// spec-declared code present in EVERY element verb's x-errors, so the prior
// undeclared `not_supported` emission is corrected to it (the migration-wide
// "map a pre-existing undeclared code to the spec-declared one without
// weakening validation" stance — same as the six prior slices).

IUIAutomation* require_uia(Connection& conn) {
    IUIAutomation* uia = conn.element_table().uia();
    if (!uia) {
        conn.writer().write_err(
            ErrorCode::PermissionDenied,
            "{\"reason\":\"IUIAutomation unavailable\"}");
    }
    return uia;
}

// Borrowed lookup of an `elt:N` handle. A missing / unknown id is reported as
// `target_gone` (the element id was valid wire syntax but no longer resolves
// to a live element — the connection-scoped allocation was invalidated or the
// id was never issued). `target_gone` is in the x-errors of every verb that
// resolves a handle (tree/invoke/toggle/expand/collapse/focus/text/set_text).
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

// Resolves the required `handle` schema property for the single-element verbs
// (invoke/toggle/expand/collapse/focus/text/set_text). A missing /
// non-scalar `handle` is invalid_args (in those verbs' x-errors); a
// well-formed handle that does not resolve is target_gone (via
// require_element).
IUIAutomationElement* require_handle(Connection& conn, const SchemaArgs& args,
                                     std::string_view verb) {
    std::optional<std::string> raw = args.str("handle");
    if (!raw) {
        invalid_args(conn, std::string(verb) + " requires 'handle'");
        return nullptr;
    }
    return require_element(conn, *raw);
}

// ---------------------------------------------------------------------------
// find / wait / find_invoke shared matcher.
//
// Resolves the role / name / automation_id / root inputs once, validates the
// name⊕automation_id mutual exclusivity (x-mutually-exclusive), resolves the
// optional `root` element-handle scope, and runs one FindAll pass returning
// the first match (or nullptr). `root` absent => the desktop RootElement.
//
// On a *validation* failure this writes the error and sets `failed`; callers
// must return immediately. A clean "no match this pass" is (nullptr, !failed).

struct MatchSpec {
    bool        have_role = false;
    std::string role;                 // exact role-token match
    bool        have_name = false;
    std::string name_lower;           // case-insensitive substring on Name
    bool        have_aid = false;
    std::string aid;                  // exact AutomationId match
};

std::string to_lower_ascii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// Parses + validates the role/name/automation_id matcher. Returns false (and
// writes invalid_args) if neither a name, automation_id, nor role was given,
// or if name and automation_id were both supplied (mutually exclusive).
bool build_match_spec(Connection& conn, const SchemaArgs& args,
                      std::string_view verb, MatchSpec& spec) {
    if (args.present("role")) {
        auto r = args.str("role");
        if (!r) {
            invalid_args(conn, std::string(verb) + " 'role' must be a string");
            return false;
        }
        spec.have_role = true;
        spec.role = *r;
    }
    if (args.present("name")) {
        auto n = args.str("name");
        if (!n) {
            invalid_args(conn, std::string(verb) + " 'name' must be a string");
            return false;
        }
        spec.have_name = true;
        spec.name_lower = to_lower_ascii(*n);
    }
    if (args.present("automation_id")) {
        auto a = args.str("automation_id");
        if (!a) {
            invalid_args(conn,
                         std::string(verb) + " 'automation_id' must be a string");
            return false;
        }
        spec.have_aid = true;
        spec.aid = *a;
    }

    if (spec.have_name && spec.have_aid) {
        invalid_args(conn, std::string(verb) +
                     " 'name' and 'automation_id' are mutually exclusive");
        return false;
    }
    if (!spec.have_role && !spec.have_name && !spec.have_aid) {
        invalid_args(conn, std::string(verb) +
                     " requires at least one of 'role', 'name', "
                     "'automation_id'");
        return false;
    }
    return true;
}

bool element_matches(IUIAutomationElement* elem, const MatchSpec& spec) {
    if (spec.have_role) {
        CONTROLTYPEID ctype = 0;
        if (FAILED(elem->get_CurrentControlType(&ctype))) return false;
        if (spec.role != role_token(ctype)) return false;
    }
    if (spec.have_aid) {
        if (element_automation_id(elem) != spec.aid) return false;
    }
    if (spec.have_name) {
        const std::string lname = to_lower_ascii(element_name(elem));
        if (lname.find(spec.name_lower) == std::string::npos) return false;
    }
    return true;
}

// Resolves the optional `root` scope into the element to FindAll under. On
// absent root, returns the desktop RootElement. On a present-but-unresolvable
// root handle, writes target_gone (root is an `elt:N` handle; an invalid one
// is the same invalidation failure require_element reports) and returns
// nullptr with `failed` set. find/wait/find_invoke all list target_gone in
// their x-errors EXCEPT element.find / element.wait — see report; the agent
// legitimately reports an invalidated root handle and the spec-x-errors
// omission is tracked upstream (same stance as the crash-guard target_gone).
ComPtr<IUIAutomationElement> resolve_search_root(Connection& conn,
                                                 IUIAutomation* uia,
                                                 const SchemaArgs& args,
                                                 std::string_view verb,
                                                 bool& failed) {
    failed = false;
    if (args.present("root")) {
        auto r = args.str("root");
        if (!r) {
            invalid_args(conn, std::string(verb) + " 'root' must be a string");
            failed = true;
            return nullptr;
        }
        IUIAutomationElement* scope = conn.element_table().lookup(*r);
        if (!scope) {
            std::string detail = "{";
            json::append_kv_string(detail, "handle", *r);
            detail += '}';
            conn.writer().write_err(ErrorCode::TargetGone, detail);
            failed = true;
            return nullptr;
        }
        // `scope` is borrowed (the ElementTable owns the only ref).
        // ComPtr::operator=(raw*) performs its own AddRef, so the table's
        // ref is untouched and the ComPtr releases exactly its own ref on
        // destruction — no double-free, no leak.
        ComPtr<IUIAutomationElement> out = scope;
        return out;
    }

    ComPtr<IUIAutomationElement> root;
    uia->GetRootElement(&root);
    return root;
}

// One FindAll pass over the visible subtree from `root`, returning the first
// element satisfying `spec` (AddRef'd into `out`) or nullptr. UIA-internal
// failures (no root / no condition / FindAll failure) are mapped to
// permission_denied — `not_supported` is NOT in find/wait/find_invoke
// x-errors; permission_denied is the spec-declared code present in all three.
// Returns false (and writes the error) on such a UIA failure.
bool find_first_match(Connection& conn,
                      IUIAutomation* uia,
                      IUIAutomationElement* root,
                      const MatchSpec& spec,
                      ComPtr<IUIAutomationElement>& out) {
    if (!root) {
        conn.writer().write_err(ErrorCode::PermissionDenied);
        return false;
    }
    auto cond = build_visible_condition(uia);
    if (!cond) {
        conn.writer().write_err(ErrorCode::PermissionDenied);
        return false;
    }
    ComPtr<IUIAutomationElementArray> arr;
    if (FAILED(root->FindAll(TreeScope_Subtree, cond.Get(), &arr)) || !arr) {
        conn.writer().write_err(ErrorCode::PermissionDenied);
        return false;
    }
    int count = 0;
    arr->get_Length(&count);
    for (int i = 0; i < count; ++i) {
        ComPtr<IUIAutomationElement> elem;
        if (FAILED(arr->GetElement(i, &elem)) || !elem) continue;
        if (!element_matches(elem.Get(), spec)) continue;
        out = std::move(elem);
        return true;
    }
    out.Reset();
    return true;   // clean "no match this pass"
}

// The not_found vs uia_blind decision shared by find / wait / find_invoke:
// if the foreground window is at a higher integrity level than the agent,
// UIA likely cannot see across the barrier — report uia_blind with the IL
// pair; otherwise plain not_found.
void write_not_found_or_uia_blind(Connection& conn) {
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

// Reads timeout_ms (optional, default `dflt`, minimum 0). Returns false (and
// writes invalid_args) on a non-integer / negative value.
bool read_timeout_ms(Connection& conn, const SchemaArgs& args,
                     std::string_view verb, long long dflt,
                     long long& out_ms) {
    out_ms = dflt;
    if (!args.present("timeout_ms")) return true;
    auto t = args.integer("timeout_ms");
    if (!t || *t < 0) {
        invalid_args(conn, std::string(verb) +
                     " 'timeout_ms' must be a non-negative integer");
        return false;
    }
    out_ms = *t;
    return true;
}

// ---------------------------------------------------------------------------
// R5 — disabled-element pre-check. Reads UIA `IsEnabledProperty`; if false,
// writes a structured `invalid_args` response with the spec-aligned
// `element_disabled` discriminator and returns false (the caller must return
// without invoking the pattern). Returns true on (enabled) or (UIA read
// failure) — a read failure is NOT treated as "disabled"; the verb proceeds
// and any genuine UIA disability surfaces via the pattern call's own error.
//
// `IsOffscreen == true` alone does NOT block (per the task spec — many valid
// UIA elements are invocable while offscreen, e.g. a virtualized list item
// before scroll). `offscreen` is still surfaced in the `flags` array purely
// for caller visibility.
//
// Detail shape (matches element-object emission style elsewhere in this TU):
//   {
//     "reason": "element_disabled",
//     "name":   "<accessible name>",
//     "role":   "<role token>",
//     "flags":  ["offscreen", ...],
//     "bounds": {"x":..,"y":..,"w":..,"h":..}
//   }
//
// invalid_args is in the x-errors of every invoke-style verb this gate is
// applied to (the spec already declares it for find_invoke/invoke/toggle/
// expand/collapse/focus/set_text) — so emitting via ErrorCode::InvalidArgs
// stays within each verb's declared error surface. The `reason` discriminator
// is the agent-ahead-of-spec signal; the protocol-side schema PR is tracked
// as a deferred follow-up per the task brief.
bool block_if_disabled(Connection& conn, IUIAutomationElement* elem) {
    BOOL enabled = TRUE;
    if (FAILED(elem->get_CurrentIsEnabled(&enabled))) {
        // UIA failed to report the state; do not block. Pattern call will
        // surface the underlying failure on its own.
        return false;
    }
    if (enabled) return false;

    CONTROLTYPEID ctype = UIA_CustomControlTypeId;
    elem->get_CurrentControlType(&ctype);

    RECT rc{};
    elem->get_CurrentBoundingRectangle(&rc);

    std::string detail = "{";
    json::append_kv_string(detail, "reason", "element_disabled");      detail += ',';
    json::append_kv_string(detail, "name",   element_name(elem));      detail += ',';
    json::append_kv_string(detail, "role",   role_token(ctype));       detail += ',';
    json::append_string_array(detail, "flags", element_flags(elem));   detail += ',';
    json::append_string(detail, "bounds");
    detail += ':';
    append_bounds_object(detail, rc);
    detail += '}';
    conn.writer().write_err(ErrorCode::InvalidArgs, detail);
    return true;
}

// ---------------------------------------------------------------------------
// Internal helper: invoke the InvokePattern on `elem` and write the verb
// response. Used by element.invoke (after handle lookup) and the compound
// verbs element.find_invoke / element.at_invoke.
//
// A hard InvokePattern failure (post-QI, non-ELEMENTNOTAVAILABLE) is reported
// as permission_denied — `not_supported` is NOT in invoke/find_invoke/
// at_invoke x-errors; permission_denied is the spec-declared code present in
// all three (corrects the pre-Phase-2.1 undeclared `not_supported`).
void invoke_on_element(Connection& conn, IUIAutomationElement* elem) {
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
        conn.writer().write_err(ErrorCode::PermissionDenied, detail);
        return;
    }
    conn.writer().write_ok();
}

}  // namespace

// ---------------------------------------------------------------------------
// element.list — input_schema {region,limit,offset,full};
// x-output-schema {elements:[ {handle,role,name,automation_id,bounds,flags?}
// ]}.

void list(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"region", "limit", "offset", "full"});
    if (args.reject_unknown(conn)) return;

    IUIAutomation* uia = require_uia(conn);
    if (!uia) return;

    RECT region{};
    bool has_region = false;
    int  limit      = 100;     // schema default
    int  offset     = 0;       // schema default
    bool full       = false;   // schema default

    // `region` is the nested $defs/Bounds object {x,y,w,h} — read via
    // node() and walk it (str() is scalar-only by design).
    if (args.present("region")) {
        const mcp::JsonValue* rn = args.node("region");
        if (rn == nullptr || !rn->is_object()) {
            invalid_args(conn,
                         "element.list 'region' must be an object "
                         "{x,y,w,h}");
            return;
        }
        const auto pull = [&](const char* k, int& dst) -> bool {
            const mcp::JsonValue* v = rn->find(k);
            if (v == nullptr || !v->is_number()) return false;
            const long long n = static_cast<long long>(v->as_number());
            if (n < INT_MIN || n > INT_MAX) return false;
            dst = static_cast<int>(n);
            return true;
        };
        int rx = 0, ry = 0, rw = 0, rh = 0;
        if (!pull("x", rx) || !pull("y", ry) ||
            !pull("w", rw) || !pull("h", rh)) {
            invalid_args(conn,
                         "element.list 'region' requires integer x,y,w,h");
            return;
        }
        region.left   = rx;
        region.top    = ry;
        region.right  = rx + rw;
        region.bottom = ry + rh;
        has_region = true;
    }

    if (args.present("limit")) {
        auto l = args.integer32("limit");
        if (!l || *l < 1) {
            invalid_args(conn,
                         "element.list 'limit' must be a positive integer");
            return;
        }
        limit = *l;
    }

    if (args.present("offset")) {
        auto o = args.integer32("offset");
        if (!o || *o < 0) {
            invalid_args(conn,
                         "element.list 'offset' must be a non-negative "
                         "integer");
            return;
        }
        offset = *o;
    }

    if (args.present("full")) {
        auto f = args.boolean("full");
        if (!f) {
            invalid_args(conn, "element.list 'full' must be a boolean");
            return;
        }
        full = *f;
    }

    ComPtr<IUIAutomationElement> root;
    if (FAILED(uia->GetRootElement(&root)) || !root) {
        conn.writer().write_err(ErrorCode::PermissionDenied);
        return;
    }

    auto cond = build_visible_condition(uia);
    if (!cond) {
        conn.writer().write_err(ErrorCode::PermissionDenied);
        return;
    }

    ComPtr<IUIAutomationElementArray> arr;
    if (FAILED(root->FindAll(TreeScope_Subtree, cond.Get(), &arr)) || !arr) {
        conn.writer().write_err(ErrorCode::PermissionDenied);
        return;
    }

    int total = 0;
    arr->get_Length(&total);

    std::string body = "{\"elements\":[";
    bool first   = true;
    int  skipped = 0;
    int  emitted = 0;

    for (int i = 0; i < total; ++i) {
        if (emitted >= limit) break;

        ComPtr<IUIAutomationElement> elem;
        if (FAILED(arr->GetElement(i, &elem)) || !elem) continue;

        if (has_region) {
            RECT rc{};
            if (FAILED(elem->get_CurrentBoundingRectangle(&rc))) continue;
            if (rc.right <= region.left || rc.left >= region.right ||
                rc.bottom <= region.top || rc.top >= region.bottom) {
                continue;
            }
        }

        if (skipped < offset) { ++skipped; continue; }

        const auto handle = conn.element_table().register_element(elem.Get());
        if (!first) body += ',';
        first = false;
        append_element_object(body, handle, elem.Get(), full);
        ++emitted;
    }
    body += "]}";
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// element.tree — input_schema {handle (required),depth,full};
// x-output-schema {elements:[ {depth,handle,role,name,automation_id,bounds,
// flags?} ], truncated?}.

void tree(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"handle", "depth", "full"});
    if (args.reject_unknown(conn)) return;

    std::optional<std::string> handle = args.str("handle");
    if (!handle) {
        invalid_args(conn, "element.tree requires 'handle'");
        return;
    }

    int  max_depth = 5;       // schema default
    int  count_cap = 200;     // internal default cap
    bool full      = false;   // schema default

    if (args.present("full")) {
        auto f = args.boolean("full");
        if (!f) {
            invalid_args(conn, "element.tree 'full' must be a boolean");
            return;
        }
        full = *f;
        if (full) {
            // Per the spec: full raises the depth cap to 12 and removes the
            // element count cap.
            max_depth = 12;
            count_cap = INT_MAX;
        }
    }

    if (args.present("depth")) {
        auto d = args.integer32("depth");
        if (!d || *d < 0) {
            invalid_args(conn,
                         "element.tree 'depth' must be a non-negative "
                         "integer");
            return;
        }
        max_depth = *d;
    }

    IUIAutomation* uia = require_uia(conn);
    if (!uia) return;
    IUIAutomationElement* root = require_element(conn, *handle);
    if (!root) return;

    ComPtr<IUIAutomationTreeWalker> walker;
    uia->get_ContentViewWalker(&walker);
    if (!walker) {
        conn.writer().write_err(ErrorCode::PermissionDenied);
        return;
    }

    std::string body = "{\"elements\":[";
    int  count     = 0;
    bool truncated = false;

    // Include the root itself at depth 0.
    {
        const auto id = conn.element_table().register_element(root);
        body += '{';
        json::append_kv_int(body, "depth", 0);
        body += ',';
        std::string per_elem;
        append_element_object(per_elem, id, root, full);
        body.append(per_elem.data() + 1, per_elem.size() - 2);
        body += '}';
        ++count;
    }
    bool first = false;
    truncated = walk_subtree(walker.Get(), root, conn.element_table(),
                             body, first, /*depth=*/1, max_depth,
                             full, count, count_cap);
    body += ']';
    if (truncated) body += ",\"truncated\":true";
    body += '}';
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// element.at — input_schema {x,y (required)}; x-output-schema
// {handle,role,name,automation_id,bounds,flags}.

void at(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"x", "y"});
    if (args.reject_unknown(conn)) return;

    auto xv = args.integer32("x");
    auto yv = args.integer32("y");
    if (!xv || !yv) {
        invalid_args(conn, "element.at requires integer 'x' and 'y'");
        return;
    }

    IUIAutomation* uia = require_uia(conn);
    if (!uia) return;

    POINT pt{*xv, *yv};
    ComPtr<IUIAutomationElement> elem;
    if (FAILED(uia->ElementFromPoint(pt, &elem)) || !elem) {
        // No element under the point: not_found, unless the foreground sits
        // across an IL barrier (then uia_blind). Both are in x-errors.
        write_not_found_or_uia_blind(conn);
        return;
    }

    const auto handle = conn.element_table().register_element(elem.Get());
    std::string body;
    append_element_object(body, handle, elem.Get(), /*with_flags=*/true);
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// element.find — input_schema {root,name,role,automation_id,timeout_ms};
// x-mutually-exclusive [name,automation_id]; x-output-schema
// {handle,name,role,automation_id,bounds,flags}.

void find(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"root", "name", "role", "automation_id",
                          "timeout_ms"});
    if (args.reject_unknown(conn)) return;

    IUIAutomation* uia = require_uia(conn);
    if (!uia) return;

    MatchSpec spec;
    if (!build_match_spec(conn, args, "element.find", spec)) return;

    long long timeout_ms = 2000;   // schema default
    if (!read_timeout_ms(conn, args, "element.find", 2000, timeout_ms)) return;

    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);

    while (true) {
        bool root_failed = false;
        ComPtr<IUIAutomationElement> root =
            resolve_search_root(conn, uia, args, "element.find", root_failed);
        if (root_failed) return;

        ComPtr<IUIAutomationElement> match;
        if (!find_first_match(conn, uia, root.Get(), spec, match)) return;

        if (match) {
            const auto handle =
                conn.element_table().register_element(match.Get());
            std::string body;
            append_element_object(body, handle, match.Get(),
                                   /*with_flags=*/true);
            conn.writer().write_ok(body);
            return;
        }

        if (clock::now() >= deadline) {
            write_not_found_or_uia_blind(conn);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

// ---------------------------------------------------------------------------
// element.wait — input_schema {root,name,role,automation_id,flags_required,
// timeout_ms}; x-mutually-exclusive [name,automation_id]; x-output-schema
// {handle,name,role,automation_id,bounds,flags}. Polling form of find: the
// match must also carry EVERY flag in flags_required (#92) before the wait
// is satisfied.

void wait(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"root", "name", "role", "automation_id",
                          "flags_required", "timeout_ms"});
    if (args.reject_unknown(conn)) return;

    IUIAutomation* uia = require_uia(conn);
    if (!uia) return;

    MatchSpec spec;
    if (!build_match_spec(conn, args, "element.wait", spec)) return;

    // flags_required: array of the #92 flag enum that must ALL be present on
    // the matched element's flags before the wait completes.
    std::vector<std::string> flags_required;
    if (args.present("flags_required")) {
        const mcp::JsonValue* fn = args.node("flags_required");
        if (fn == nullptr || !fn->is_array()) {
            invalid_args(conn,
                         "element.wait 'flags_required' must be an array of "
                         "flag strings");
            return;
        }
        // Spec input_schema constrains flags_required items to this enum.
        // An unknown flag would make the wait unsatisfiable (polls to
        // timeout -> not_found) with no indication, so reject it up front.
        static constexpr std::string_view kFlagEnum[] = {
            "enabled", "focused", "offscreen", "password", "required"};
        for (const auto& item : fn->as_array()) {
            if (!item.is_string()) {
                invalid_args(conn,
                             "element.wait 'flags_required' entries must be "
                             "strings");
                return;
            }
            const std::string flag = item.as_string();
            bool known = false;
            for (const std::string_view valid : kFlagEnum) {
                if (flag == valid) { known = true; break; }
            }
            if (!known) {
                invalid_args(conn,
                             "element.wait 'flags_required' contains an "
                             "unknown flag: " + flag);
                return;
            }
            flags_required.push_back(flag);
        }
    }

    long long timeout_ms = 5000;   // schema default
    if (!read_timeout_ms(conn, args, "element.wait", 5000, timeout_ms)) return;

    const auto flags_satisfied = [&](IUIAutomationElement* elem) -> bool {
        if (flags_required.empty()) return true;
        const std::vector<std::string> have = element_flags(elem);
        for (const std::string& want : flags_required) {
            bool found = false;
            for (const std::string& h : have) {
                if (h == want) { found = true; break; }
            }
            if (!found) return false;
        }
        return true;
    };

    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);

    while (true) {
        bool root_failed = false;
        ComPtr<IUIAutomationElement> root =
            resolve_search_root(conn, uia, args, "element.wait", root_failed);
        if (root_failed) return;

        ComPtr<IUIAutomationElement> match;
        if (!find_first_match(conn, uia, root.Get(), spec, match)) return;

        if (match && flags_satisfied(match.Get())) {
            const auto handle =
                conn.element_table().register_element(match.Get());
            std::string body;
            append_element_object(body, handle, match.Get(),
                                   /*with_flags=*/true);
            conn.writer().write_ok(body);
            return;
        }

        if (clock::now() >= deadline) {
            write_not_found_or_uia_blind(conn);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

// ---------------------------------------------------------------------------
// element.invoke — input_schema {handle (required)}; x-output-schema null.

void invoke(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"handle"});
    if (args.reject_unknown(conn)) return;

    IUIAutomationElement* elem = require_handle(conn, args, "element.invoke");
    if (!elem) return;

    // R5: disabled-element pre-check.
    if (block_if_disabled(conn, elem)) return;

    invoke_on_element(conn, elem);
}

// ---------------------------------------------------------------------------
// element.toggle — input_schema {handle (required)}; x-output-schema
// {new_state}.

void toggle(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"handle"});
    if (args.reject_unknown(conn)) return;

    IUIAutomationElement* elem = require_handle(conn, args, "element.toggle");
    if (!elem) return;

    // R5: disabled-element pre-check.
    if (block_if_disabled(conn, elem)) return;

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
        conn.writer().write_err(ErrorCode::PermissionDenied);
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
// element.expand / element.collapse — input_schema {handle (required)};
// x-output-schema {new_state}.

namespace {

void do_expand_collapse(Connection& conn, const wire::Request& req,
                        bool expand, std::string_view verb) {
    SchemaArgs args(req, {"handle"});
    if (args.reject_unknown(conn)) return;

    IUIAutomationElement* elem = require_handle(conn, args, verb);
    if (!elem) return;

    // R5: disabled-element pre-check.
    if (block_if_disabled(conn, elem)) return;

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
        conn.writer().write_err(ErrorCode::PermissionDenied);
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
    do_expand_collapse(conn, req, true, "element.expand");
}

void collapse(Connection& conn, const wire::Request& req) {
    do_expand_collapse(conn, req, false, "element.collapse");
}

// ---------------------------------------------------------------------------
// element.focus — input_schema {handle (required)}; x-output-schema null.

void focus(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"handle"});
    if (args.reject_unknown(conn)) return;

    IUIAutomationElement* elem = require_handle(conn, args, "element.focus");
    if (!elem) return;

    // R5: disabled-element pre-check.
    if (block_if_disabled(conn, elem)) return;

    const HRESULT hr = elem->SetFocus();
    if (hr == UIA_E_ELEMENTNOTAVAILABLE) {
        conn.writer().write_err(ErrorCode::TargetGone);
        return;
    }
    if (FAILED(hr)) {
        // element.focus.json x-errors lists not_supported_by_target — a
        // SetFocus failure on a live element is "the element doesn't accept
        // keyboard focus" (not the undeclared not_supported).
        conn.writer().write_err(ErrorCode::NotSupportedByTarget);
        return;
    }
    conn.writer().write_ok();
}

// ---------------------------------------------------------------------------
// element.text — input_schema {handle (required)}; x-output-schema {text}.
// TextPattern → ValuePattern → Name fallback.

void text(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"handle"});
    if (args.reject_unknown(conn)) return;

    IUIAutomationElement* elem = require_handle(conn, args, "element.text");
    if (!elem) return;

    std::string value;
    bool resolved = false;

    // 1. TextPattern.DocumentRange().GetText(-1)
    {
        ComPtr<IUIAutomationTextPattern> tp;
        if (SUCCEEDED(elem->GetCurrentPatternAs(
                UIA_TextPatternId, IID_PPV_ARGS(&tp))) && tp) {
            ComPtr<IUIAutomationTextRange> range;
            if (SUCCEEDED(tp->get_DocumentRange(&range)) && range) {
                BSTR txt = nullptr;
                if (SUCCEEDED(range->GetText(-1, &txt)) && txt) {
                    value = bstr_to_utf8(txt);
                    SysFreeString(txt);
                    resolved = true;
                }
            }
        }
    }

    // 2. ValuePattern.CurrentValue
    if (!resolved) {
        ComPtr<IUIAutomationValuePattern> vp;
        if (SUCCEEDED(elem->GetCurrentPatternAs(
                UIA_ValuePatternId, IID_PPV_ARGS(&vp))) && vp) {
            BSTR v = nullptr;
            if (SUCCEEDED(vp->get_CurrentValue(&v)) && v) {
                value = bstr_to_utf8(v);
                SysFreeString(v);
                resolved = true;
            }
        }
    }

    // 3. Name fallback (always resolvable — possibly empty string, which is
    // a valid {text:""} per the spec).
    if (!resolved) {
        value = element_name(elem);
    }

    std::string body = "{";
    json::append_kv_string(body, "text", value);
    body += '}';
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// element.set_text — input_schema {handle (required), text (required)};
// x-output-schema null. `text` is now an inline UTF-8 STRING property — the
// pre-Phase-2.1 separate length-prefixed wire payload is gone (mirrors the
// clipboard.set / process.run slices).

void set_text(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"handle", "text"});
    if (args.reject_unknown(conn)) return;

    IUIAutomationElement* elem = require_handle(conn, args, "element.set_text");
    if (!elem) return;

    // R5: disabled-element pre-check.
    if (block_if_disabled(conn, elem)) return;

    std::optional<std::string> text_arg = args.str("text");
    if (!text_arg) {
        invalid_args(conn, "element.set_text requires string 'text'");
        return;
    }
    const std::wstring wtext = text::utf8_to_wide(*text_arg);

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
    if (bs == nullptr) {
        conn.writer().write_err(
            ErrorCode::PermissionDenied,
            "{\"message\":\"element.set_text: BSTR allocation failed\"}");
        return;
    }
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
        conn.writer().write_err(ErrorCode::PermissionDenied, detail);
        return;
    }
    conn.writer().write_ok();
}

// ---------------------------------------------------------------------------
// element.find_invoke — input_schema {root,name,role,automation_id,
// timeout_ms}; x-mutually-exclusive [name,automation_id]; x-output-schema
// null. Compound: element.find followed by element.invoke; the intermediate
// elt:N is allocated, used, and discarded within the call.

void find_invoke(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"root", "name", "role", "automation_id",
                          "timeout_ms"});
    if (args.reject_unknown(conn)) return;

    IUIAutomation* uia = require_uia(conn);
    if (!uia) return;

    MatchSpec spec;
    if (!build_match_spec(conn, args, "element.find_invoke", spec)) return;

    long long timeout_ms = 2000;   // schema default
    if (!read_timeout_ms(conn, args, "element.find_invoke", 2000,
                         timeout_ms)) {
        return;
    }

    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);

    while (true) {
        bool root_failed = false;
        ComPtr<IUIAutomationElement> root =
            resolve_search_root(conn, uia, args, "element.find_invoke",
                                root_failed);
        if (root_failed) return;

        ComPtr<IUIAutomationElement> match;
        if (!find_first_match(conn, uia, root.Get(), spec, match)) return;

        if (match) {
            // R5: disabled-element pre-check. The find phase is unchanged;
            // only the invoke step gains the gate. If the matched element
            // is disabled, refuse with `element_disabled` instead of
            // silently invoking it.
            if (block_if_disabled(conn, match.Get())) return;
            invoke_on_element(conn, match.Get());
            return;
        }

        if (clock::now() >= deadline) {
            write_not_found_or_uia_blind(conn);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

// ---------------------------------------------------------------------------
// element.at_invoke — input_schema {x,y (required)}; x-output-schema null.
// Compound: element.at followed by element.invoke.

void at_invoke(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"x", "y"});
    if (args.reject_unknown(conn)) return;

    auto xv = args.integer32("x");
    auto yv = args.integer32("y");
    if (!xv || !yv) {
        invalid_args(conn, "element.at_invoke requires integer 'x' and 'y'");
        return;
    }

    IUIAutomation* uia = require_uia(conn);
    if (!uia) return;

    POINT pt{*xv, *yv};
    ComPtr<IUIAutomationElement> elem;
    if (FAILED(uia->ElementFromPoint(pt, &elem)) || !elem) {
        // element.at_invoke.json x-errors lists not_found (NOT uia_blind);
        // a missed hit-test is not_found regardless of the IL barrier.
        conn.writer().write_err(ErrorCode::NotFound);
        return;
    }
    invoke_on_element(conn, elem.Get());
}

}  // namespace remote_hands::element_verbs
