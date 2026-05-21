//
// Copyright 2026 William Isted and contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//

import Foundation
import AppKit
import ApplicationServices

/// Information about a single on-screen window, derived from the CG window
/// server. Window IDs on the wire use the `mac:<CGWindowID>` form.
public struct WindowInfo: Sendable {
    public let id: CGWindowID
    public let title: String
    public let pid: pid_t
    public let owner: String
    public let bounds: CGRect
    public let layer: Int

    public var idString: String { "mac:\(id)" }

    /// JSON object per the v2.2 conformance schema: `handle`, `title`,
    /// `pid` (int), `bounds: {x, y, w, h}`, `owner`, `layer`. macOS uses
    /// `mac:` prefix in `handle` — the conformance suite asserts `win:`
    /// (windows-only); that assertion is documented as a known-divergence.
    public var jsonObject: [String: Any] {
        [
            "handle": idString,
            "title": title,
            "pid": Int(pid),
            "owner": owner,
            "bounds": [
                "x": Int(bounds.origin.x),
                "y": Int(bounds.origin.y),
                "w": Int(bounds.size.width),
                "h": Int(bounds.size.height),
            ] as [String: Int],
            "layer": layer,
        ]
    }
}

public enum WindowError: Error, Equatable {
    /// `mac:<n>` form expected; got something else or unparseable.
    case invalidId(String)
    /// No window with that CGWindowID currently exists, or the owning app
    /// has no AX window matching it.
    case notFound
    /// AX TCC grant is missing. Caller must visit System Settings →
    /// Privacy & Security → Accessibility.
    case axDisabled
    /// AX returned a non-success status. Detail string for logging.
    case axError(String)
    /// AX attribute is not settable on the target (e.g. some windows refuse
    /// resize).
    case readonly(String)
}

public enum Window {

    // MARK: list

    /// Enumerate on-screen windows. Off-screen, desktop-element, and dock
    /// windows are excluded — matches the protocol's "interactable
    /// windows" intent.
    public static func list(filter: String? = nil, includeAll: Bool = false) -> [WindowInfo] {
        var options: CGWindowListOption = [.excludeDesktopElements]
        if !includeAll {
            options.insert(.optionOnScreenOnly)
        }
        guard let raw = CGWindowListCopyWindowInfo(options, kCGNullWindowID) as? [[String: Any]] else {
            return []
        }
        var out: [WindowInfo] = []
        for entry in raw {
            guard let num = entry[kCGWindowNumber as String] as? UInt32 else { continue }
            let title = (entry[kCGWindowName as String] as? String) ?? ""
            // window.list defaults to interactable windows — without
            // Screen Recording TCC, titles are nil for off-process windows
            // (Sonoma+). Caller filters with --filter once `system.info`
            // tells them the TCC state.
            if let f = filter, !globMatch(pattern: f, in: title) {
                continue
            }
            let pid = pid_t(entry[kCGWindowOwnerPID as String] as? Int32 ?? 0)
            let owner = (entry[kCGWindowOwnerName as String] as? String) ?? ""
            let layer = entry[kCGWindowLayer as String] as? Int ?? 0
            // Skip the Dock, status bar, etc. unless includeAll. Layer 0 is
            // ordinary app windows; non-zero layers are usually system UI.
            if !includeAll && layer != 0 { continue }
            let bounds = (entry[kCGWindowBounds as String] as? [String: Any]).map(rect(from:)) ?? .zero
            out.append(WindowInfo(
                id: num, title: title, pid: pid,
                owner: owner, bounds: bounds, layer: layer
            ))
        }
        return out
    }

    public static func find(pattern: String) throws -> WindowInfo {
        for w in list() where globMatch(pattern: pattern, in: w.title) {
            return w
        }
        throw WindowError.notFound
    }

    // MARK: AX-driven verbs

    public static func focus(idStr: String) throws {
        let info = try resolveWindow(idStr: idStr)
        // Bring the owning app forward. NSRunningApplication does not need
        // AX TCC; AX is only needed for the raise step that follows.
        if let app = NSRunningApplication(processIdentifier: info.pid) {
            app.activate(options: [])
        }
        let ax = try axWindow(for: info)
        let err = AXUIElementPerformAction(ax, kAXRaiseAction as CFString)
        try checkAXResult(err, context: "kAXRaiseAction")
    }

    public static func close(idStr: String) throws {
        let info = try resolveWindow(idStr: idStr)
        let ax = try axWindow(for: info)
        var btn: CFTypeRef?
        let err = AXUIElementCopyAttributeValue(ax, kAXCloseButtonAttribute as CFString, &btn)
        try checkAXResult(err, context: "kAXCloseButtonAttribute")
        guard let button = btn else {
            throw WindowError.notFound
        }
        // CFTypeRef → AXUIElement (force-cast is safe: AX attribute returns
        // an AXUIElement for AXCloseButton when present).
        let pressErr = AXUIElementPerformAction(button as! AXUIElement, kAXPressAction as CFString)
        try checkAXResult(pressErr, context: "kAXPressAction on close button")
    }

    public static func move(idStr: String, to rect: CGRect) throws {
        let info = try resolveWindow(idStr: idStr)
        let ax = try axWindow(for: info)
        // AXValue wraps CGPoint/CGSize for AX attribute setting.
        var pos = rect.origin
        var size = rect.size
        guard let posValue = AXValueCreate(.cgPoint, &pos),
              let sizeValue = AXValueCreate(.cgSize, &size) else {
            throw WindowError.axError("AXValueCreate failed")
        }
        let posErr = AXUIElementSetAttributeValue(ax, kAXPositionAttribute as CFString, posValue)
        if posErr == .attributeUnsupported || posErr == .cannotComplete {
            throw WindowError.readonly("position not settable on this window")
        }
        try checkAXResult(posErr, context: "kAXPositionAttribute set")
        let sizeErr = AXUIElementSetAttributeValue(ax, kAXSizeAttribute as CFString, sizeValue)
        if sizeErr == .attributeUnsupported || sizeErr == .cannotComplete {
            throw WindowError.readonly("size not settable on this window")
        }
        try checkAXResult(sizeErr, context: "kAXSizeAttribute set")
    }

    public static func state(idStr: String) throws -> String {
        let info = try resolveWindow(idStr: idStr)
        let ax = try axWindow(for: info)
        if axBool(ax, attribute: kAXMinimizedAttribute) == true {
            return "minimised"
        }
        // Full-screen attribute is optional; absence = false.
        if let fs = axBool(ax, attribute: "AXFullScreen"), fs {
            return "fullscreen"
        }
        // Hidden = window's app is hidden (Cmd-H).
        if let app = NSRunningApplication(processIdentifier: info.pid), app.isHidden {
            return "hidden"
        }
        return "normal"
    }

    // MARK: helpers

    public static func parseID(_ s: String) throws -> CGWindowID {
        guard s.hasPrefix("mac:") else { throw WindowError.invalidId(s) }
        let n = s.dropFirst(4)
        guard let v = UInt32(n) else { throw WindowError.invalidId(s) }
        return CGWindowID(v)
    }

    /// Resolve `mac:<n>` to a current WindowInfo (uses CGWindowList).
    private static func resolveWindow(idStr: String) throws -> WindowInfo {
        let target = try parseID(idStr)
        // Use includeAll so we can resolve windows users explicitly target
        // even if they're not interactable in the default list.
        for w in list(filter: nil, includeAll: true) where w.id == target {
            return w
        }
        throw WindowError.notFound
    }

    /// Find the AXUIElement for a CGWindowInfo by walking the owning app's
    /// AX windows and matching by title.
    ///
    /// Limitation: an app with multiple windows of the same title can
    /// confuse this — Apple's recommended fix is `_AXUIElementGetWindow`
    /// (private). For MVP, first-title-match is the contract.
    private static func axWindow(for info: WindowInfo) throws -> AXUIElement {
        let app = AXUIElementCreateApplication(info.pid)
        var windowsRef: CFTypeRef?
        let err = AXUIElementCopyAttributeValue(app, kAXWindowsAttribute as CFString, &windowsRef)
        try checkAXResult(err, context: "kAXWindowsAttribute")
        guard let windows = windowsRef as? [AXUIElement] else {
            throw WindowError.notFound
        }
        for w in windows {
            if axString(w, attribute: kAXTitleAttribute) == info.title {
                return w
            }
        }
        // Fallback: when title is empty (no TCC, or untitled window), pick
        // the first window. Imperfect; works for many single-window apps.
        if info.title.isEmpty, let first = windows.first {
            return first
        }
        throw WindowError.notFound
    }

    private static func checkAXResult(_ err: AXError, context: String) throws {
        switch err {
        case .success: return
        case .apiDisabled:
            throw WindowError.axDisabled
        case .notImplemented, .attributeUnsupported, .actionUnsupported, .noValue:
            throw WindowError.notFound
        default:
            throw WindowError.axError("\(context): AXError \(err.rawValue)")
        }
    }

    private static func axBool(_ element: AXUIElement, attribute: String) -> Bool? {
        var ref: CFTypeRef?
        let err = AXUIElementCopyAttributeValue(element, attribute as CFString, &ref)
        if err != .success { return nil }
        return (ref as? Bool)
    }

    private static func axString(_ element: AXUIElement, attribute: String) -> String? {
        var ref: CFTypeRef?
        let err = AXUIElementCopyAttributeValue(element, attribute as CFString, &ref)
        if err != .success { return nil }
        return (ref as? String)
    }

    private static func rect(from dict: [String: Any]) -> CGRect {
        let x = (dict["X"] as? CGFloat) ?? 0
        let y = (dict["Y"] as? CGFloat) ?? 0
        let w = (dict["Width"] as? CGFloat) ?? 0
        let h = (dict["Height"] as? CGFloat) ?? 0
        return CGRect(x: x, y: y, width: w, height: h)
    }

    // Small-and-permissive glob: '*' matches any run of characters, '?'
    // matches any single character. Anchored at both ends so a bare
    // pattern matches the whole title. Used by window.list --filter and
    // window.find.
    public static func globMatch(pattern: String, in str: String) -> Bool {
        let p = Array(pattern)
        let s = Array(str)
        return globRec(p, 0, s, 0)
    }

    private static func globRec(_ p: [Character], _ pi: Int, _ s: [Character], _ si: Int) -> Bool {
        if pi == p.count { return si == s.count }
        if p[pi] == "*" {
            // Match zero or more of any character.
            var i = si
            while i <= s.count {
                if globRec(p, pi + 1, s, i) { return true }
                i += 1
            }
            return false
        }
        if si == s.count { return false }
        if p[pi] == "?" || p[pi].lowercased() == s[si].lowercased() {
            return globRec(p, pi + 1, s, si + 1)
        }
        return false
    }

    // MARK: TCC

    public static func hasAccessibilityPermission() -> Bool {
        return AXIsProcessTrustedWithOptions(nil)
    }
}
