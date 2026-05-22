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
import Carbon.HIToolbox

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
    /// macOS Accessibility TCC not granted — synthetic CGEvent posting
    /// requires it. Caller must enable the agent in System Settings →
    /// Privacy & Security → Accessibility, then restart the agent.
    case permissionDenied
    case unknownButton(String)
    case unknownKey(String)
    case unknownModifier(String)
    case eventCreationFailed
}

/// Mapping from human-readable key names to macOS `CGKeyCode` values
/// (Carbon `kVK_*` constants). Names are matched case-insensitively. The
/// table is deliberately small — common keys plus letters, digits, and
/// function keys. Arbitrary Unicode goes via `input.keyboard.type` which
/// bypasses the table entirely via `CGEventKeyboardSetUnicodeString`.
enum KeyName {
    static func keyCode(for name: String) -> CGKeyCode? {
        return table[name.lowercased()]
    }

    private static let table: [String: CGKeyCode] = {
        var t: [String: CGKeyCode] = [
            // letters
            "a": CGKeyCode(kVK_ANSI_A), "b": CGKeyCode(kVK_ANSI_B), "c": CGKeyCode(kVK_ANSI_C),
            "d": CGKeyCode(kVK_ANSI_D), "e": CGKeyCode(kVK_ANSI_E), "f": CGKeyCode(kVK_ANSI_F),
            "g": CGKeyCode(kVK_ANSI_G), "h": CGKeyCode(kVK_ANSI_H), "i": CGKeyCode(kVK_ANSI_I),
            "j": CGKeyCode(kVK_ANSI_J), "k": CGKeyCode(kVK_ANSI_K), "l": CGKeyCode(kVK_ANSI_L),
            "m": CGKeyCode(kVK_ANSI_M), "n": CGKeyCode(kVK_ANSI_N), "o": CGKeyCode(kVK_ANSI_O),
            "p": CGKeyCode(kVK_ANSI_P), "q": CGKeyCode(kVK_ANSI_Q), "r": CGKeyCode(kVK_ANSI_R),
            "s": CGKeyCode(kVK_ANSI_S), "t": CGKeyCode(kVK_ANSI_T), "u": CGKeyCode(kVK_ANSI_U),
            "v": CGKeyCode(kVK_ANSI_V), "w": CGKeyCode(kVK_ANSI_W), "x": CGKeyCode(kVK_ANSI_X),
            "y": CGKeyCode(kVK_ANSI_Y), "z": CGKeyCode(kVK_ANSI_Z),
            // digits
            "0": CGKeyCode(kVK_ANSI_0), "1": CGKeyCode(kVK_ANSI_1), "2": CGKeyCode(kVK_ANSI_2),
            "3": CGKeyCode(kVK_ANSI_3), "4": CGKeyCode(kVK_ANSI_4), "5": CGKeyCode(kVK_ANSI_5),
            "6": CGKeyCode(kVK_ANSI_6), "7": CGKeyCode(kVK_ANSI_7), "8": CGKeyCode(kVK_ANSI_8),
            "9": CGKeyCode(kVK_ANSI_9),
            // control
            "enter": CGKeyCode(kVK_Return), "return": CGKeyCode(kVK_Return),
            "tab": CGKeyCode(kVK_Tab), "space": CGKeyCode(kVK_Space),
            "backspace": CGKeyCode(kVK_Delete), "delete": CGKeyCode(kVK_ForwardDelete),
            "escape": CGKeyCode(kVK_Escape), "esc": CGKeyCode(kVK_Escape),
            "left": CGKeyCode(kVK_LeftArrow), "right": CGKeyCode(kVK_RightArrow),
            "up": CGKeyCode(kVK_UpArrow), "down": CGKeyCode(kVK_DownArrow),
            "home": CGKeyCode(kVK_Home), "end": CGKeyCode(kVK_End),
            "pageup": CGKeyCode(kVK_PageUp), "pagedown": CGKeyCode(kVK_PageDown),
            // function
            "f1": CGKeyCode(kVK_F1), "f2": CGKeyCode(kVK_F2), "f3": CGKeyCode(kVK_F3),
            "f4": CGKeyCode(kVK_F4), "f5": CGKeyCode(kVK_F5), "f6": CGKeyCode(kVK_F6),
            "f7": CGKeyCode(kVK_F7), "f8": CGKeyCode(kVK_F8), "f9": CGKeyCode(kVK_F9),
            "f10": CGKeyCode(kVK_F10), "f11": CGKeyCode(kVK_F11), "f12": CGKeyCode(kVK_F12),
        ]
        return t
    }()
}

/// Modifier flag mapping for `--modifiers` argument.
enum Modifier {
    static func flag(for name: String) -> CGEventFlags? {
        switch name.lowercased() {
        case "cmd", "command", "meta": return .maskCommand
        case "shift":                   return .maskShift
        case "opt", "option", "alt":    return .maskAlternate
        case "ctrl", "control":         return .maskControl
        case "fn":                      return .maskSecondaryFn
        default: return nil
        }
    }

    static func combinedFlags(from raw: String) throws -> CGEventFlags {
        var flags: CGEventFlags = []
        for token in raw.split(separator: ",") {
            let t = token.trimmingCharacters(in: .whitespaces)
            guard let f = flag(for: t) else {
                throw InputError.unknownModifier(t)
            }
            flags.insert(f)
        }
        return flags
    }
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

    // MARK: keyboard

    public static func keyTap(named name: String, modifiers: CGEventFlags = []) throws {
        try probeTCC()
        guard let code = KeyName.keyCode(for: name) else {
            throw InputError.unknownKey(name)
        }
        let src = source()
        guard let down = CGEvent(keyboardEventSource: src, virtualKey: code, keyDown: true),
              let up = CGEvent(keyboardEventSource: src, virtualKey: code, keyDown: false)
        else { throw InputError.eventCreationFailed }
        if !modifiers.isEmpty {
            down.flags = modifiers
            up.flags = modifiers
        }
        down.post(tap: .cghidEventTap)
        up.post(tap: .cghidEventTap)
    }

    public static func keyDown(named name: String, modifiers: CGEventFlags = []) throws {
        try probeTCC()
        guard let code = KeyName.keyCode(for: name) else {
            throw InputError.unknownKey(name)
        }
        guard let ev = CGEvent(keyboardEventSource: source(), virtualKey: code, keyDown: true)
        else { throw InputError.eventCreationFailed }
        if !modifiers.isEmpty { ev.flags = modifiers }
        ev.post(tap: .cghidEventTap)
    }

    public static func keyUp(named name: String, modifiers: CGEventFlags = []) throws {
        try probeTCC()
        guard let code = KeyName.keyCode(for: name) else {
            throw InputError.unknownKey(name)
        }
        guard let ev = CGEvent(keyboardEventSource: source(), virtualKey: code, keyDown: false)
        else { throw InputError.eventCreationFailed }
        if !modifiers.isEmpty { ev.flags = modifiers }
        ev.post(tap: .cghidEventTap)
    }

    /// Synthesise typing of arbitrary Unicode text. Uses
    /// `CGEventKeyboardSetUnicodeString` — bypasses the virtual-key table
    /// entirely, so emoji, accented letters, and any BMP/SMP character
    /// works out of the box. Apps that bind directly to HID (games,
    /// DirectInput-style polling) may not pick this up — same caveat as
    /// the Windows family.
    public static func typeText(_ text: String) throws {
        try probeTCC()
        let src = source()
        // One event per scalar keeps the down/up pair valid; some apps
        // require a full down-then-up cycle per character.
        for scalar in text.unicodeScalars {
            guard let down = CGEvent(keyboardEventSource: src, virtualKey: 0, keyDown: true),
                  let up = CGEvent(keyboardEventSource: src, virtualKey: 0, keyDown: false)
            else { throw InputError.eventCreationFailed }
            let u = [UniChar](String(scalar).utf16)
            u.withUnsafeBufferPointer { buf in
                if let base = buf.baseAddress {
                    down.keyboardSetUnicodeString(stringLength: buf.count, unicodeString: base)
                    up.keyboardSetUnicodeString(stringLength: buf.count, unicodeString: base)
                }
            }
            down.post(tap: .cghidEventTap)
            up.post(tap: .cghidEventTap)
        }
    }

    // MARK: position (read-tier; no TCC needed)

    /// Current cursor position in CG coordinates (origin = top-left of
    /// primary display).
    public static func cursorPosition() -> CGPoint {
        // CGEvent with no source returns one stamped with the current HID
        // state; .location is the live cursor coord.
        if let ev = CGEvent(source: nil) {
            return ev.location
        }
        // Fallback via Cocoa: NSEvent.mouseLocation is in flipped coords
        // (origin = bottom-left of primary display).
        let cocoa = NSEvent.mouseLocation
        let screenHeight = NSScreen.screens.first?.frame.height ?? 0
        return CGPoint(x: cocoa.x, y: screenHeight - cocoa.y)
    }

    // MARK: input system settings (read-tier; no TCC needed)

    /// System input settings for `system.info.capabilities.input_settings`:
    /// double-click timing, key-repeat timing, and the double-click slop
    /// rectangle.
    public static func systemSettings() -> [String: Any] {
        // Double-click interval — NSEvent exposes it directly, in seconds.
        let doubleClickMs = Int((NSEvent.doubleClickInterval * 1000).rounded())

        // Key-repeat settings live in the global defaults domain as tick
        // counts (1/60 s). They stay absent until the user moves them off
        // the System Settings default, so fall back to typical values.
        let defaults = UserDefaults.standard
        let initialRepeatTicks = defaults.object(forKey: "InitialKeyRepeat") as? Int ?? 25
        let repeatTicks = max(1, defaults.object(forKey: "KeyRepeat") as? Int ?? 6)
        let repeatDelayMs = Int((Double(initialRepeatTicks) * 1000.0 / 60.0).rounded())
        // characters/second = 60 ticks-per-second ÷ ticks-per-repeat.
        let repeatRateCps = Int((60.0 / Double(repeatTicks)).rounded())

        return [
            "double_click_time_ms": doubleClickMs,
            "keyboard_repeat_delay_ms": repeatDelayMs,
            "keyboard_repeat_rate_cps": repeatRateCps,
            // macOS exposes no public API for the double-click slop
            // rectangle; report the conventional small tolerance.
            "double_click_rect": ["w": 4, "h": 4] as [String: Int],
        ]
    }
}
