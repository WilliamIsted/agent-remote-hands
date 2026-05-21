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
import CoreGraphics
import AppKit
import IOKit
import IOKit.hid

/// Mouse button selector. Maps to the matching `CGMouseButton` +
/// `CGEventType.*Down/*Up/*Dragged` triple.
public enum MouseButton: String, Sendable {
    case left, right, middle

    public init(parsing s: String) throws {
        guard let b = MouseButton(rawValue: s) else {
            throw InputError.unknownButton(s)
        }
        self = b
    }

    public var cg: CGMouseButton {
        switch self {
        case .left:   return .left
        case .right:  return .right
        case .middle: return .center
        }
    }

    public var downType: CGEventType {
        switch self {
        case .left:   return .leftMouseDown
        case .right:  return .rightMouseDown
        case .middle: return .otherMouseDown
        }
    }

    public var upType: CGEventType {
        switch self {
        case .left:   return .leftMouseUp
        case .right:  return .rightMouseUp
        case .middle: return .otherMouseUp
        }
    }

    public var dragType: CGEventType {
        switch self {
        case .left:   return .leftMouseDragged
        case .right:  return .rightMouseDragged
        case .middle: return .otherMouseDragged
        }
    }
}

public enum InputError: Error, Equatable {
    /// Input Monitoring TCC not granted. Caller must visit System Settings
    /// → Privacy & Security → Input Monitoring.
    case permissionDenied
    case unknownButton(String)
    case eventCreationFailed
}

/// `input.*` implementations — CGEvent-based mouse + keyboard synthesis.
///
/// Every public entry point probes Input Monitoring TCC before posting; if
/// the grant is missing, `CGEventPost` silently drops events, which would
/// surface as success-with-no-effect. Probing makes the failure mode an
/// explicit `ERR permission_denied` on the wire.
public enum Input {

    // MARK: TCC

    public static func hasInputMonitoringPermission() -> Bool {
        return IOHIDCheckAccess(kIOHIDRequestTypePostEvent) == kIOHIDAccessTypeGranted
    }

    private static func probeTCC() throws {
        if !hasInputMonitoringPermission() {
            throw InputError.permissionDenied
        }
    }

    private static func source() -> CGEventSource? {
        return CGEventSource(stateID: .hidSystemState)
    }

    // MARK: mouse

    public static func click(at point: CGPoint, button: MouseButton, clicks: Int = 1) throws {
        try probeTCC()
        let src = source()
        // Move cursor first so the click registers at the right pixel even
        // if the hardware-tracked location is elsewhere.
        if let mv = CGEvent(mouseEventSource: src, mouseType: .mouseMoved,
                            mouseCursorPosition: point, mouseButton: button.cg) {
            mv.post(tap: .cghidEventTap)
        }
        for i in 1...max(1, clicks) {
            guard let down = CGEvent(mouseEventSource: src, mouseType: button.downType,
                                     mouseCursorPosition: point, mouseButton: button.cg),
                  let up = CGEvent(mouseEventSource: src, mouseType: button.upType,
                                   mouseCursorPosition: point, mouseButton: button.cg)
            else { throw InputError.eventCreationFailed }
            // mouseEventClickState carries the click count (1 = single,
            // 2 = double, 3 = triple). Apps use this to detect multi-click.
            down.setIntegerValueField(.mouseEventClickState, value: Int64(i))
            up.setIntegerValueField(.mouseEventClickState, value: Int64(i))
            down.post(tap: .cghidEventTap)
            up.post(tap: .cghidEventTap)
        }
    }

    public static func move(to point: CGPoint) throws {
        try probeTCC()
        guard let ev = CGEvent(mouseEventSource: source(), mouseType: .mouseMoved,
                               mouseCursorPosition: point, mouseButton: .left)
        else { throw InputError.eventCreationFailed }
        ev.post(tap: .cghidEventTap)
    }

    public static func scroll(at point: CGPoint, notches: Int) throws {
        try probeTCC()
        // Move first so the scroll targets the right window.
        try move(to: point)
        guard let ev = CGEvent(scrollWheelEvent2Source: source(), units: .line,
                               wheelCount: 1, wheel1: Int32(notches), wheel2: 0, wheel3: 0)
        else { throw InputError.eventCreationFailed }
        ev.post(tap: .cghidEventTap)
    }

    public static func drag(from start: CGPoint, to end: CGPoint, button: MouseButton, steps: Int = 10) throws {
        try probeTCC()
        let src = source()
        guard let down = CGEvent(mouseEventSource: src, mouseType: button.downType,
                                 mouseCursorPosition: start, mouseButton: button.cg)
        else { throw InputError.eventCreationFailed }
        down.post(tap: .cghidEventTap)
        let n = max(1, steps)
        for i in 1...n {
            let t = Double(i) / Double(n)
            let p = CGPoint(
                x: start.x + (end.x - start.x) * t,
                y: start.y + (end.y - start.y) * t
            )
            if let drag = CGEvent(mouseEventSource: src, mouseType: button.dragType,
                                  mouseCursorPosition: p, mouseButton: button.cg) {
                drag.post(tap: .cghidEventTap)
            }
        }
        guard let up = CGEvent(mouseEventSource: src, mouseType: button.upType,
                               mouseCursorPosition: end, mouseButton: button.cg)
        else { throw InputError.eventCreationFailed }
        up.post(tap: .cghidEventTap)
    }

    public static func press(at point: CGPoint, button: MouseButton) throws {
        try probeTCC()
        guard let ev = CGEvent(mouseEventSource: source(), mouseType: button.downType,
                               mouseCursorPosition: point, mouseButton: button.cg)
        else { throw InputError.eventCreationFailed }
        ev.post(tap: .cghidEventTap)
    }

    public static func release(at point: CGPoint, button: MouseButton) throws {
        try probeTCC()
        guard let ev = CGEvent(mouseEventSource: source(), mouseType: button.upType,
                               mouseCursorPosition: point, mouseButton: button.cg)
        else { throw InputError.eventCreationFailed }
        ev.post(tap: .cghidEventTap)
    }
}
