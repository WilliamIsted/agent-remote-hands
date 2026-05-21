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

public enum ElementError: Error, Equatable {
    /// AX TCC missing — analogue of windows-modern `uia_blind`.
    case axDisabled
    /// `elt:N` form expected; got something else.
    case invalidId(String)
    /// elt id not in this connection's table (stale or never issued).
    case notFound
    /// AX search completed without a match.
    case noMatch
    /// AX attribute is not settable on the target.
    case readonly(String)
    /// Element has no action of the requested kind.
    case actionUnsupported
    /// Poll timed out without a match.
    case timeout
    /// AX returned a non-success status we don't have a more specific case for.
    case axError(String)
}

/// Per-connection element ID table. Maps wire IDs (`elt:N`) to live
/// `AXUIElement` references + their owning pid. References are not stable
/// across target-app restarts; if a lookup fails on first use, return
/// `notFound` and let the client re-discover.
public final class ElementTable {
    private var entries: [Int: (pid: pid_t, element: AXUIElement)] = [:]
    private var nextID = 1

    public init() {}

    public func register(pid: pid_t, element: AXUIElement) -> String {
        let id = nextID
        nextID += 1
        entries[id] = (pid, element)
        return "elt:\(id)"
    }

    public func lookup(id idStr: String) throws -> (pid: pid_t, element: AXUIElement) {
        guard idStr.hasPrefix("elt:"), let n = Int(idStr.dropFirst(4)) else {
            throw ElementError.invalidId(idStr)
        }
        guard let entry = entries[n] else { throw ElementError.notFound }
        return entry
    }
}

public enum Element {

    // MARK: probe

    public static func hasAccessibilityPermission() -> Bool {
        return AXIsProcessTrustedWithOptions(nil)
    }

    // MARK: list / tree / at / find

    public struct Snapshot: Sendable {
        public let id: String
        public let role: String
        public let title: String
        public let value: String
        public let bounds: CGRect?
        public let depth: Int

        public var jsonObject: [String: Any] {
            var o: [String: Any] = ["id": id, "role": role, "title": title, "value": value, "depth": depth]
            if let b = bounds {
                o["bounds"] = ["x": Int(b.origin.x), "y": Int(b.origin.y), "w": Int(b.size.width), "h": Int(b.size.height)]
            }
            return o
        }
    }

    /// Enumerate visible interactable elements in the foreground app.
    /// `region` (optional) clips to a screen rect.
    public static func list(table: ElementTable, region: CGRect? = nil, maxResults: Int = 256) throws -> [Snapshot] {
        try probeTCC()
        guard let frontPID = NSWorkspace.shared.frontmostApplication?.processIdentifier else {
            return []
        }
        let app = AXUIElementCreateApplication(frontPID)
        var out: [Snapshot] = []
        walk(app, pid: frontPID, depth: 0, maxDepth: 16, table: table, into: &out, filter: { snap in
            interactableRoles.contains(snap.role) && (region.map { $0.intersects(snap.bounds ?? .null) } ?? true)
        }, cap: maxResults)
        return out
    }

    /// Recursive walk from a registered element id.
    public static func tree(table: ElementTable, idStr: String, maxDepth: Int = 16, maxResults: Int = 512) throws -> [Snapshot] {
        try probeTCC()
        let (pid, root) = try table.lookup(id: idStr)
        var out: [Snapshot] = []
        walk(root, pid: pid, depth: 0, maxDepth: maxDepth, table: table, into: &out, filter: { _ in true }, cap: maxResults)
        return out
    }

    /// Hit-test at screen coordinates.
    public static func elementAt(table: ElementTable, point: CGPoint) throws -> Snapshot {
        try probeTCC()
        let sys = AXUIElementCreateSystemWide()
        var ref: AXUIElement?
        let err = AXUIElementCopyElementAtPosition(sys, Float(point.x), Float(point.y), &ref)
        try mapAXError(err)
        guard let el = ref else { throw ElementError.noMatch }
        let pid = ownerPID(of: el) ?? 0
        return snapshot(of: el, pid: pid, depth: 0, table: table)
    }

    /// Find first element matching `<role> <name-pattern>`. Returns
    /// `ElementError.noMatch` if walk completes without a hit; AX errors
    /// surface as `axDisabled` / `axError`.
    public static func find(table: ElementTable, role: String, pattern: String, scope: AXUIElement? = nil) throws -> Snapshot {
        try probeTCC()
        let root: AXUIElement
        let pid: pid_t
        if let s = scope {
            root = s
            pid = ownerPID(of: s) ?? 0
        } else {
            guard let frontPID = NSWorkspace.shared.frontmostApplication?.processIdentifier else {
                throw ElementError.noMatch
            }
            root = AXUIElementCreateApplication(frontPID)
            pid = frontPID
        }
        var hit: Snapshot? = nil
        walk(root, pid: pid, depth: 0, maxDepth: 16, table: table, into: nil, filter: { snap in
            if snap.role.caseInsensitiveCompare(role) == .orderedSame {
                let target = snap.title.isEmpty ? snap.value : snap.title
                if Window.globMatch(pattern: pattern, in: target) {
                    hit = snap
                    return true  // stops the walk via the cap
                }
            }
            return false
        }, cap: 1, stopOnFirstMatch: true)
        if let h = hit { return h }
        throw ElementError.noMatch
    }

    /// Polling form of `find`. `interval` defaults to 100ms; `deadline` is
    /// absolute time in ms since unix epoch.
    public static func wait(table: ElementTable, role: String, pattern: String, intervalMs: Int = 100, deadlineMs: Int) throws -> Snapshot {
        try probeTCC()
        let stop = Double(deadlineMs) / 1000.0
        while Date().timeIntervalSince1970 < stop {
            if let s = try? find(table: table, role: role, pattern: pattern) {
                return s
            }
            Thread.sleep(forTimeInterval: Double(intervalMs) / 1000.0)
        }
        throw ElementError.timeout
    }

    public static func text(table: ElementTable, idStr: String) throws -> String {
        try probeTCC()
        let (_, el) = try table.lookup(id: idStr)
        return axString(el, attribute: kAXValueAttribute as String)
            ?? axString(el, attribute: kAXTitleAttribute as String)
            ?? ""
    }

    // MARK: actions (update-tier)

    public static func invoke(table: ElementTable, idStr: String) throws {
        try probeTCC()
        let (_, el) = try table.lookup(id: idStr)
        let err = AXUIElementPerformAction(el, kAXPressAction as CFString)
        try mapAXError(err)
    }

    public static func toggle(table: ElementTable, idStr: String) throws -> String {
        try probeTCC()
        let (_, el) = try table.lookup(id: idStr)
        // Press toggles a check/radio's value; read back to report new state.
        let err = AXUIElementPerformAction(el, kAXPressAction as CFString)
        try mapAXError(err)
        return axString(el, attribute: kAXValueAttribute as String) ?? "unknown"
    }

    public static func expand(table: ElementTable, idStr: String) throws {
        try probeTCC()
        let (_, el) = try table.lookup(id: idStr)
        let err = AXUIElementSetAttributeValue(el, kAXDisclosingAttribute as CFString, kCFBooleanTrue)
        if err == .attributeUnsupported { throw ElementError.actionUnsupported }
        try mapAXError(err)
    }

    public static func collapse(table: ElementTable, idStr: String) throws {
        try probeTCC()
        let (_, el) = try table.lookup(id: idStr)
        let err = AXUIElementSetAttributeValue(el, kAXDisclosingAttribute as CFString, kCFBooleanFalse)
        if err == .attributeUnsupported { throw ElementError.actionUnsupported }
        try mapAXError(err)
    }

    public static func focus(table: ElementTable, idStr: String) throws {
        try probeTCC()
        let (_, el) = try table.lookup(id: idStr)
        let err = AXUIElementSetAttributeValue(el, kAXFocusedAttribute as CFString, kCFBooleanTrue)
        if err == .attributeUnsupported { throw ElementError.actionUnsupported }
        try mapAXError(err)
    }

    public static func setText(table: ElementTable, idStr: String, text: String) throws {
        try probeTCC()
        let (_, el) = try table.lookup(id: idStr)
        var settable: DarwinBoolean = false
        let probe = AXUIElementIsAttributeSettable(el, kAXValueAttribute as CFString, &settable)
        if probe == .success && !settable.boolValue {
            throw ElementError.readonly("kAXValueAttribute")
        }
        let err = AXUIElementSetAttributeValue(el, kAXValueAttribute as CFString, text as CFString)
        try mapAXError(err)
    }

    // MARK: helpers

    private static func probeTCC() throws {
        if !AXIsProcessTrustedWithOptions(nil) {
            throw ElementError.axDisabled
        }
    }

    private static let interactableRoles: Set<String> = [
        "AXButton", "AXTextField", "AXTextArea", "AXLink", "AXMenuItem",
        "AXCheckBox", "AXRadioButton", "AXComboBox", "AXList", "AXTable",
        "AXPopUpButton", "AXSlider", "AXIncrementor", "AXStepper",
    ]

    private static func walk(
        _ element: AXUIElement,
        pid: pid_t,
        depth: Int,
        maxDepth: Int,
        table: ElementTable,
        into: UnsafeMutablePointer<[Snapshot]>?,
        filter: (Snapshot) -> Bool,
        cap: Int,
        stopOnFirstMatch: Bool = false
    ) {
        // UnsafeMutablePointer<Array> would force a heavier interface; we
        // use a closure-passing form for the common case via the inout
        // variant below.
        var sink: [Snapshot] = into?.pointee ?? []
        walkInner(element, pid: pid, depth: depth, maxDepth: maxDepth, table: table,
                  sink: &sink, filter: filter, cap: cap, stopOnFirstMatch: stopOnFirstMatch)
        into?.pointee = sink
    }

    private static func walk(
        _ element: AXUIElement, pid: pid_t, depth: Int, maxDepth: Int,
        table: ElementTable, into: inout [Snapshot],
        filter: (Snapshot) -> Bool, cap: Int, stopOnFirstMatch: Bool = false
    ) {
        walkInner(element, pid: pid, depth: depth, maxDepth: maxDepth,
                  table: table, sink: &into, filter: filter,
                  cap: cap, stopOnFirstMatch: stopOnFirstMatch)
    }

    private static func walkInner(
        _ element: AXUIElement, pid: pid_t, depth: Int, maxDepth: Int,
        table: ElementTable, sink: inout [Snapshot],
        filter: (Snapshot) -> Bool, cap: Int, stopOnFirstMatch: Bool
    ) {
        if sink.count >= cap { return }
        if depth > maxDepth { return }

        let snap = snapshot(of: element, pid: pid, depth: depth, table: table)
        let matched = filter(snap)
        if matched {
            sink.append(snap)
            if stopOnFirstMatch || sink.count >= cap { return }
        }

        var childrenRef: CFTypeRef?
        let err = AXUIElementCopyAttributeValue(element, kAXChildrenAttribute as CFString, &childrenRef)
        if err == .success, let children = childrenRef as? [AXUIElement] {
            for child in children {
                if sink.count >= cap { return }
                walkInner(child, pid: pid, depth: depth + 1, maxDepth: maxDepth,
                          table: table, sink: &sink, filter: filter,
                          cap: cap, stopOnFirstMatch: stopOnFirstMatch)
                if stopOnFirstMatch && !sink.isEmpty { return }
            }
        }
    }

    private static func snapshot(of element: AXUIElement, pid: pid_t, depth: Int, table: ElementTable) -> Snapshot {
        let role = axString(element, attribute: kAXRoleAttribute as String) ?? ""
        let title = axString(element, attribute: kAXTitleAttribute as String) ?? ""
        let value = axString(element, attribute: kAXValueAttribute as String) ?? ""
        let id = table.register(pid: pid, element: element)
        return Snapshot(id: id, role: role, title: title, value: value, bounds: axRect(element), depth: depth)
    }

    private static func axString(_ element: AXUIElement, attribute: String) -> String? {
        var ref: CFTypeRef?
        let err = AXUIElementCopyAttributeValue(element, attribute as CFString, &ref)
        if err != .success { return nil }
        if let s = ref as? String { return s }
        return nil
    }

    private static func axRect(_ element: AXUIElement) -> CGRect? {
        var posRef: CFTypeRef?
        var sizeRef: CFTypeRef?
        _ = AXUIElementCopyAttributeValue(element, kAXPositionAttribute as CFString, &posRef)
        _ = AXUIElementCopyAttributeValue(element, kAXSizeAttribute as CFString, &sizeRef)
        guard let pos = posRef, let size = sizeRef else { return nil }
        var p = CGPoint.zero, s = CGSize.zero
        AXValueGetValue(pos as! AXValue, .cgPoint, &p)
        AXValueGetValue(size as! AXValue, .cgSize, &s)
        return CGRect(origin: p, size: s)
    }

    private static func ownerPID(of element: AXUIElement) -> pid_t? {
        var pid: pid_t = 0
        let err = AXUIElementGetPid(element, &pid)
        if err != .success { return nil }
        return pid
    }

    private static func mapAXError(_ err: AXError) throws {
        switch err {
        case .success: return
        case .apiDisabled: throw ElementError.axDisabled
        case .actionUnsupported, .attributeUnsupported, .notImplemented:
            throw ElementError.actionUnsupported
        case .noValue, .invalidUIElement: throw ElementError.notFound
        default: throw ElementError.axError("AXError \(err.rawValue)")
        }
    }
}
