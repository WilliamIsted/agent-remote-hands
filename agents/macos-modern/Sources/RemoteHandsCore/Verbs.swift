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

/// The outcome of a verb handler. The connection layer turns this into bytes
/// on the wire via `formatOK` / `formatERR` and follows it with a state
/// transition (close, tier change, …) where the verb dictates one.
public enum VerbOutcome: Sendable {
    case ok(payload: Data)
    case err(code: String, detail: [String: String])
    /// Verb-level side effect on the connection. `.closeAfterReply` causes
    /// the connection to send the reply and then close the socket.
    case okThenClose(payload: Data)
    /// Tier transition. Sent as part of `connection.tier_raise` /
    /// `connection.tier_drop` responses.
    case okWithTierChange(newTier: Tier, payload: Data)
}

/// The information the dispatcher needs about each verb: required tier,
/// payload consumption, and whether it's valid pre-hello.
public struct VerbSpec: Sendable {
    public let tier: Tier
    /// True for verbs callable before `connection.hello` (only hello and
    /// close qualify). All other verbs require state == connected.
    public let preHelloOK: Bool
    /// True for verbs whose grammar includes a final `<length>` argument
    /// followed by exactly that many opaque payload bytes on the wire
    /// (PROTOCOL.md §1.2). The framing layer reads the payload before the
    /// verb handler is invoked; the handler receives it in
    /// `WireRequest.payload`.
    public let consumesPayload: Bool

    public init(tier: Tier, preHelloOK: Bool = false, consumesPayload: Bool = false) {
        self.tier = tier
        self.preHelloOK = preHelloOK
        self.consumesPayload = consumesPayload
    }
}

/// Convenience: convert `[String: String]` detail to `[String: Any]` for the
/// formatter (the formatter takes Any so it can also carry numerics).
private func detailAny(_ d: [String: String]) -> [String: Any] {
    var out: [String: Any] = [:]
    for (k, v) in d { out[k] = v }
    return out
}

// MARK: - Verb registry

/// The single source of truth for which verbs the MVP advertises. New verbs
/// land here first; the dispatcher routes anything else to `not_supported`.
public enum VerbTable {
    public static let specs: [String: VerbSpec] = [
        // Connection lifecycle (CRUDX-exempt — tier doesn't apply).
        "connection.hello":      VerbSpec(tier: .read, preHelloOK: true),
        "connection.close":      VerbSpec(tier: .read, preHelloOK: true),
        "connection.reset":      VerbSpec(tier: .read, preHelloOK: false),
        "connection.tier_raise": VerbSpec(tier: .read, preHelloOK: false),
        "connection.tier_drop":  VerbSpec(tier: .read, preHelloOK: false),

        // System discovery — all read-tier.
        "system.info":           VerbSpec(tier: .read, preHelloOK: false),
        "system.health":         VerbSpec(tier: .read, preHelloOK: false),
        "system.capabilities":   VerbSpec(tier: .read, preHelloOK: false),
        "system.verbs":          VerbSpec(tier: .read, preHelloOK: false),

        // Screen capture — read-tier.
        "screen.capture":        VerbSpec(tier: .read, preHelloOK: false),

        // Clipboard — get is read, set is update and consumes a payload.
        "clipboard.get":         VerbSpec(tier: .read,   preHelloOK: false),
        "clipboard.set":         VerbSpec(tier: .update, preHelloOK: false, consumesPayload: true),

        // Window — list/find/state are read; focus/close/move are update.
        "window.list":           VerbSpec(tier: .read,   preHelloOK: false),
        "window.find":           VerbSpec(tier: .read,   preHelloOK: false),
        "window.state":          VerbSpec(tier: .read,   preHelloOK: false),
        "window.focus":          VerbSpec(tier: .update, preHelloOK: false),
        "window.close":          VerbSpec(tier: .update, preHelloOK: false),
        "window.move":           VerbSpec(tier: .update, preHelloOK: false),

        // Mouse input — all update-tier.
        "input.mouse.click":     VerbSpec(tier: .update, preHelloOK: false),
        "input.mouse.move":      VerbSpec(tier: .update, preHelloOK: false),
        "input.mouse.scroll":    VerbSpec(tier: .update, preHelloOK: false),
        "input.mouse.drag":      VerbSpec(tier: .update, preHelloOK: false),
        "input.mouse.press":     VerbSpec(tier: .update, preHelloOK: false),
        "input.mouse.release":   VerbSpec(tier: .update, preHelloOK: false),

        // Keyboard input — all update-tier; type consumes a payload.
        "input.keyboard.key":       VerbSpec(tier: .update, preHelloOK: false),
        "input.keyboard.key_down":  VerbSpec(tier: .update, preHelloOK: false),
        "input.keyboard.key_up":    VerbSpec(tier: .update, preHelloOK: false),
        "input.keyboard.type":      VerbSpec(tier: .update, preHelloOK: false, consumesPayload: true),

        // Shared input verbs.
        "input.position":           VerbSpec(tier: .read,   preHelloOK: false),
        // Windows-only message verbs — Apple Events would be the equivalent
        // affordance but the wire shape is incompatible. Stubs ensure the
        // wire surface includes them but always returns not_supported_by_target.
        "input.send_message":       VerbSpec(tier: .update, preHelloOK: false),
        "input.post_message":       VerbSpec(tier: .update, preHelloOK: false),

        // Process.
        "process.list":             VerbSpec(tier: .read,   preHelloOK: false),
        "process.start":            VerbSpec(tier: .create, preHelloOK: false, consumesPayload: true),
        "process.shell":            VerbSpec(tier: .create, preHelloOK: false),
        "process.kill":             VerbSpec(tier: .delete, preHelloOK: false),
        "process.wait":             VerbSpec(tier: .read,   preHelloOK: false),

        // File + directory.
        "file.create":              VerbSpec(tier: .create, preHelloOK: false),
        "file.read":                VerbSpec(tier: .read,   preHelloOK: false),
        "file.write":               VerbSpec(tier: .update, preHelloOK: false, consumesPayload: true),
        "file.write_at":            VerbSpec(tier: .update, preHelloOK: false, consumesPayload: true),
        "file.delete":              VerbSpec(tier: .delete, preHelloOK: false),
        "file.rename":              VerbSpec(tier: .update, preHelloOK: false),
        "file.stat":                VerbSpec(tier: .read,   preHelloOK: false),
        "file.exists":              VerbSpec(tier: .read,   preHelloOK: false),
        "file.wait":                VerbSpec(tier: .read,   preHelloOK: false),
        "file.download":            VerbSpec(tier: .update, preHelloOK: false),
        "directory.list":           VerbSpec(tier: .read,   preHelloOK: false),
        "directory.stat":           VerbSpec(tier: .read,   preHelloOK: false),
        "directory.exists":         VerbSpec(tier: .read,   preHelloOK: false),
        "directory.create":         VerbSpec(tier: .create, preHelloOK: false),
        "directory.rename":         VerbSpec(tier: .update, preHelloOK: false),
        "directory.remove":         VerbSpec(tier: .delete, preHelloOK: false),

        // Element (AX) — 7 read + 7 update.
        "element.list":             VerbSpec(tier: .read,   preHelloOK: false),
        "element.tree":             VerbSpec(tier: .read,   preHelloOK: false),
        "element.at":               VerbSpec(tier: .read,   preHelloOK: false),
        "element.find":             VerbSpec(tier: .read,   preHelloOK: false),
        "element.wait":             VerbSpec(tier: .read,   preHelloOK: false),
        "element.text":             VerbSpec(tier: .read,   preHelloOK: false),
        "element.invoke":           VerbSpec(tier: .update, preHelloOK: false),
        "element.toggle":           VerbSpec(tier: .update, preHelloOK: false),
        "element.expand":           VerbSpec(tier: .update, preHelloOK: false),
        "element.collapse":         VerbSpec(tier: .update, preHelloOK: false),
        "element.focus":            VerbSpec(tier: .update, preHelloOK: false),
        "element.set_text":         VerbSpec(tier: .update, preHelloOK: false, consumesPayload: true),
        "element.find_invoke":      VerbSpec(tier: .update, preHelloOK: false),
        "element.at_invoke":        VerbSpec(tier: .update, preHelloOK: false),
    ]

    public static let implementedNamespaces: [String] = ["connection", "system", "screen", "clipboard", "window", "input", "element", "file", "directory", "process"]
    public static let implementedVerbs: [String] = Array(specs.keys)
}

// MARK: - Verb handlers

/// Result of running a verb against a connection. The dispatcher does not
/// hold session state — that lives in `ConnectionSession` — so handlers take
/// the relevant pieces in.
public func dispatchVerb(
    _ request: WireRequest,
    currentTier: Tier,
    elementTable: ElementTable
) -> VerbOutcome {
    guard let spec = VerbTable.specs[request.verb] else {
        return .err(code: "not_supported_by_target", detail: ["verb": request.verb])
    }
    // Tier gate. Connection-namespace verbs are CRUDX-exempt and skip this.
    if !request.verb.hasPrefix("connection.") {
        if !currentTier.subsumes(spec.tier) {
            return .err(
                code: "tier_required",
                detail: ["required": spec.tier.rawValue, "current": currentTier.rawValue]
            )
        }
    }

    switch request.verb {
    case "connection.hello":   return handleHello(request)
    case "connection.close":   return handleClose(request)
    case "connection.reset":   return handleReset(request)
    case "connection.tier_raise": return handleTierRaise(request, currentTier: currentTier)
    case "connection.tier_drop":  return handleTierDrop(request, currentTier: currentTier)
    case "system.info":        return handleSystemInfo(currentTier: currentTier)
    case "system.health":      return handleSystemHealth()
    case "system.capabilities": return handleSystemCapabilities()
    case "system.verbs":       return handleSystemVerbs()
    case "screen.capture":     return handleScreenCapture(request)
    case "clipboard.get":      return handleClipboardGet()
    case "clipboard.set":      return handleClipboardSet(request)
    case "window.list":        return handleWindowList(request)
    case "window.find":        return handleWindowFind(request)
    case "window.focus":       return handleWindowFocus(request)
    case "window.close":       return handleWindowClose(request)
    case "window.move":        return handleWindowMove(request)
    case "window.state":       return handleWindowState(request)
    case "input.mouse.click":  return handleMouseClick(request)
    case "input.mouse.move":   return handleMouseMove(request)
    case "input.mouse.scroll": return handleMouseScroll(request)
    case "input.mouse.drag":   return handleMouseDrag(request)
    case "input.mouse.press":  return handleMousePress(request)
    case "input.mouse.release":return handleMouseRelease(request)
    case "input.keyboard.key":      return handleKeyTap(request)
    case "input.keyboard.key_down": return handleKeyDown(request)
    case "input.keyboard.key_up":   return handleKeyUp(request)
    case "input.keyboard.type":     return handleKeyType(request)
    case "input.position":          return handleInputPosition()
    case "input.send_message", "input.post_message":
        return .err(code: "not_supported_by_target", detail: ["verb": request.verb])
    case "process.list":         return handleProcessList(request)
    case "process.start":        return handleProcessStart(request)
    case "process.shell":        return handleProcessShell(request)
    case "process.kill":         return handleProcessKill(request)
    case "process.wait":         return handleProcessWait(request)
    case "file.create":          return handleFileCreate(request)
    case "file.read":            return handleFileRead(request)
    case "file.write":           return handleFileWrite(request)
    case "file.write_at":        return handleFileWriteAt(request)
    case "file.delete":          return handleFileDelete(request)
    case "file.rename":          return handleFileRename(request)
    case "file.stat":            return handleFileStat(request)
    case "file.exists":          return handleFileExists(request)
    case "file.wait":            return handleFileWait(request)
    case "file.download":        return handleFileDownload(request)
    case "directory.list":       return handleDirList(request)
    case "directory.stat":       return handleDirStat(request)
    case "directory.exists":     return handleDirExists(request)
    case "directory.create":     return handleDirCreate(request)
    case "directory.rename":     return handleDirRename(request)
    case "directory.remove":     return handleDirRemove(request)
    case "element.list":         return handleElementList(request, table: elementTable)
    case "element.tree":         return handleElementTree(request, table: elementTable)
    case "element.at":           return handleElementAt(request, table: elementTable)
    case "element.find":         return handleElementFind(request, table: elementTable)
    case "element.wait":         return handleElementWait(request, table: elementTable)
    case "element.text":         return handleElementText(request, table: elementTable)
    case "element.invoke":       return handleElementInvoke(request, table: elementTable)
    case "element.toggle":       return handleElementToggle(request, table: elementTable)
    case "element.expand":       return handleElementExpand(request, table: elementTable)
    case "element.collapse":     return handleElementCollapse(request, table: elementTable)
    case "element.focus":        return handleElementFocus(request, table: elementTable)
    case "element.set_text":     return handleElementSetText(request, table: elementTable)
    case "element.find_invoke":  return handleElementFindInvoke(request, table: elementTable)
    case "element.at_invoke":    return handleElementAtInvoke(request, table: elementTable)
    default:
        // Unreachable — VerbTable.specs guard covers everything above.
        return .err(code: "internal_error", detail: ["verb": request.verb])
    }
}

// MARK: connection.*

private func handleHello(_ request: WireRequest) -> VerbOutcome {
    // connection.hello <client-name> <protocol-major>
    guard request.args.count >= 2 else {
        return .err(code: "invalid_args", detail: ["message": "expected <client-name> <protocol-version>"])
    }
    let claimed = request.args[1]
    // Accept "2", "2.0", "2.1", "2.2" — major-version check only.
    let major = claimed.split(separator: ".").first.map(String.init) ?? claimed
    guard major == "2" else {
        return .err(code: "protocol_mismatch", detail: ["agent": "2", "client": claimed])
    }
    return .ok(payload: Data())
}

private func handleClose(_ request: WireRequest) -> VerbOutcome {
    return .okThenClose(payload: Data())
}

private func handleReset(_ request: WireRequest) -> VerbOutcome {
    // The framing layer's "reset" is handled at the connection level when it
    // sees this verb — there's no per-verb state to clear in the MVP.
    return .ok(payload: Data())
}

private func handleTierRaise(_ request: WireRequest, currentTier: Tier) -> VerbOutcome {
    // Token file generation is not yet implemented in the MVP slice. Without
    // a token file there is no way to elevate; return not_supported_by_target
    // until the Token module lands.
    return .err(
        code: "not_supported_by_target",
        detail: ["message": "token file not yet implemented in MVP slice"]
    )
}

private func handleTierDrop(_ request: WireRequest, currentTier: Tier) -> VerbOutcome {
    guard let target = request.args.first.flatMap(Tier.init(rawValue:)) else {
        return .err(code: "invalid_args", detail: ["message": "missing or unknown tier"])
    }
    if target.rank > currentTier.rank {
        return .err(code: "invalid_args", detail: ["message": "tier_drop cannot raise"])
    }
    let body: [String: String] = ["new_tier": target.rawValue]
    let data = (try? JSONSerialization.data(withJSONObject: body, options: [.sortedKeys])) ?? Data()
    return .okWithTierChange(newTier: target, payload: data)
}

// MARK: system.*

private func handleSystemInfo(currentTier: Tier) -> VerbOutcome {
    let body = systemInfoBody(
        currentTier: currentTier,
        implementedNamespaces: VerbTable.implementedNamespaces,
        implementedVerbs: VerbTable.implementedVerbs
    )
    guard let data = try? JSONSerialization.data(withJSONObject: body, options: [.sortedKeys]) else {
        return .err(code: "internal_error", detail: ["message": "system.info JSON encoding failed"])
    }
    return .ok(payload: data)
}

private func handleSystemHealth() -> VerbOutcome {
    return .ok(payload: Data())
}

private func handleSystemCapabilities() -> VerbOutcome {
    var map: [String: [String: String]] = [:]
    for (name, spec) in VerbTable.specs {
        map[name] = ["tier": spec.tier.rawValue]
    }
    let data = (try? JSONSerialization.data(withJSONObject: map, options: [.sortedKeys])) ?? Data()
    return .ok(payload: data)
}

private func handleSystemVerbs() -> VerbOutcome {
    // Minimal shape: array of {name, tier}. The full v2.2 shape includes arg
    // schemas — those land when verbs grow real argument grammars.
    let verbs = VerbTable.specs.map { (name, spec) -> [String: String] in
        ["name": name, "tier": spec.tier.rawValue]
    }
    let body: [String: Any] = ["verbs": verbs.sorted { ($0["name"] ?? "") < ($1["name"] ?? "") }]
    let data = (try? JSONSerialization.data(withJSONObject: body, options: [.sortedKeys])) ?? Data()
    return .ok(payload: data)
}

// MARK: screen.*

private func handleScreenCapture(_ request: WireRequest) -> VerbOutcome {
    let args = ParsedArgs(request.args)

    // --format <png|jpeg|heic|bmp|webp>. Default: png. webp is in the
    // protocol enum for forward compatibility but not implemented here.
    let formatStr = args.flags["format"] ?? "png"
    if formatStr == "webp" {
        return .err(code: "unsupported_format", detail: ["format": formatStr])
    }
    guard let format = CaptureFormat(rawValue: formatStr) else {
        return .err(code: "unsupported_format", detail: ["format": formatStr])
    }

    // --quality 1..100. Default 75. Lossless formats (png, bmp) ignore it.
    let quality: Int
    if let raw = args.flags["quality"] {
        guard let q = Int(raw), (1...100).contains(q) else {
            return .err(code: "invalid_args", detail: ["message": "quality must be an integer 1-100"])
        }
        quality = q
    } else {
        quality = 75
    }

    // Selectors not yet implemented in this slice. Reject explicitly so a
    // caller passing --region doesn't get a misleadingly-correct full-screen
    // capture back.
    for sel in ["region", "window", "monitor"] {
        if args.flags[sel] != nil {
            return .err(
                code: "invalid_args",
                detail: ["message": "--\(sel) not yet implemented in MVP slice; full-screen capture only"]
            )
        }
    }

    do {
        let bytes = try ScreenCapture.captureFullScreen(format: format, quality: quality)
        return .ok(payload: bytes)
    } catch CaptureError.permissionDenied {
        return .err(code: "permission_denied", detail: [
            "category": "screen_recording",
            "hint": "Grant in System Settings → Privacy & Security → Screen Recording, then restart the agent",
        ])
    } catch CaptureError.unsupportedFormat(let f) {
        return .err(code: "unsupported_format", detail: ["format": f])
    } catch CaptureError.captureFailed(let msg) {
        return .err(code: "capture_failed", detail: ["message": msg])
    } catch {
        return .err(code: "capture_failed", detail: ["message": "\(error)"])
    }
}

// MARK: clipboard.*

private func handleClipboardGet() -> VerbOutcome {
    do {
        let bytes = try Clipboard.getText()
        return .ok(payload: bytes)
    } catch ClipboardError.empty {
        return .err(code: "clipboard_empty", detail: [:])
    } catch ClipboardError.formatUnavailable {
        return .err(code: "clipboard_format_unavailable", detail: [:])
    } catch {
        return .err(code: "internal_error", detail: ["message": "\(error)"])
    }
}

private func handleClipboardSet(_ request: WireRequest) -> VerbOutcome {
    do {
        try Clipboard.setText(request.payload)
        return .ok(payload: Data())
    } catch {
        return .err(code: "internal_error", detail: ["message": "\(error)"])
    }
}

// MARK: window.*

private func handleWindowList(_ request: WireRequest) -> VerbOutcome {
    let args = ParsedArgs(request.args)
    let filter = args.flags["filter"]
    let includeAll = args.flags["all"] != nil
    let windows = Window.list(filter: filter, includeAll: includeAll)
    let body: [String: Any] = ["windows": windows.map { $0.jsonObject }]
    guard let data = try? JSONSerialization.data(withJSONObject: body, options: [.sortedKeys]) else {
        return .err(code: "internal_error", detail: ["message": "window.list JSON encoding failed"])
    }
    return .ok(payload: data)
}

private func handleWindowFind(_ request: WireRequest) -> VerbOutcome {
    guard let pattern = request.args.first else {
        return .err(code: "invalid_args", detail: ["message": "window.find requires a title pattern"])
    }
    do {
        let w = try Window.find(pattern: pattern)
        let data = (try? JSONSerialization.data(withJSONObject: w.jsonObject, options: [.sortedKeys])) ?? Data()
        return .ok(payload: data)
    } catch WindowError.notFound {
        return .err(code: "not_found", detail: ["message": "no window matched pattern \"\(pattern)\""])
    } catch {
        return .err(code: "internal_error", detail: ["message": "\(error)"])
    }
}

private func handleWindowFocus(_ request: WireRequest) -> VerbOutcome {
    guard let id = request.args.first else {
        return .err(code: "invalid_args", detail: ["message": "window.focus requires <id>"])
    }
    return windowActionResult { try Window.focus(idStr: id) }
}

private func handleWindowClose(_ request: WireRequest) -> VerbOutcome {
    guard let id = request.args.first else {
        return .err(code: "invalid_args", detail: ["message": "window.close requires <id>"])
    }
    return windowActionResult { try Window.close(idStr: id) }
}

private func handleWindowMove(_ request: WireRequest) -> VerbOutcome {
    // Grammar: window.move <id> <x> <y> <w> <h>
    guard request.args.count >= 5 else {
        return .err(code: "invalid_args", detail: ["message": "window.move requires <id> <x> <y> <w> <h>"])
    }
    let id = request.args[0]
    guard let x = Int(request.args[1]),
          let y = Int(request.args[2]),
          let w = Int(request.args[3]),
          let h = Int(request.args[4]) else {
        return .err(code: "invalid_args", detail: ["message": "x/y/w/h must be integers"])
    }
    let rect = CGRect(x: x, y: y, width: w, height: h)
    return windowActionResult { try Window.move(idStr: id, to: rect) }
}

private func handleWindowState(_ request: WireRequest) -> VerbOutcome {
    guard let id = request.args.first else {
        return .err(code: "invalid_args", detail: ["message": "window.state requires <id>"])
    }
    do {
        let s = try Window.state(idStr: id)
        let data = (try? JSONSerialization.data(withJSONObject: ["state": s], options: [.sortedKeys])) ?? Data()
        return .ok(payload: data)
    } catch let e as WindowError {
        return windowErrorOutcome(e)
    } catch {
        return .err(code: "internal_error", detail: ["message": "\(error)"])
    }
}

private func windowActionResult(_ op: () throws -> Void) -> VerbOutcome {
    do {
        try op()
        return .ok(payload: Data())
    } catch let e as WindowError {
        return windowErrorOutcome(e)
    } catch {
        return .err(code: "internal_error", detail: ["message": "\(error)"])
    }
}

private func windowErrorOutcome(_ e: WindowError) -> VerbOutcome {
    switch e {
    case .invalidId(let s):
        return .err(code: "invalid_args", detail: ["message": "expected mac:<n>, got \"\(s)\""])
    case .notFound:
        return .err(code: "not_found", detail: [:])
    case .axDisabled:
        return .err(code: "permission_denied", detail: [
            "category": "accessibility",
            "hint": "Grant in System Settings → Privacy & Security → Accessibility, then restart the agent",
        ])
    case .axError(let m):
        return .err(code: "ax_error", detail: ["message": m])
    case .readonly(let m):
        return .err(code: "readonly", detail: ["message": m])
    }
}

// MARK: input.mouse.*

private func parsePoint(_ args: [String]) -> CGPoint? {
    guard args.count >= 2, let x = Double(args[0]), let y = Double(args[1]) else { return nil }
    return CGPoint(x: x, y: y)
}

private func parseButton(_ flags: [String: String]) throws -> MouseButton {
    let raw = flags["button"] ?? "left"
    return try MouseButton(parsing: raw)
}

private func handleMouseClick(_ request: WireRequest) -> VerbOutcome {
    let parsed = ParsedArgs(request.args)
    guard let pt = parsePoint(parsed.positionals) else {
        return .err(code: "invalid_args", detail: ["message": "expected <x> <y>"])
    }
    let clicks = parsed.intFlag("clicks") ?? 1
    return inputResult {
        let button = try parseButton(parsed.flags)
        try Input.click(at: pt, button: button, clicks: clicks)
    }
}

private func handleMouseMove(_ request: WireRequest) -> VerbOutcome {
    guard let pt = parsePoint(ParsedArgs(request.args).positionals) else {
        return .err(code: "invalid_args", detail: ["message": "expected <x> <y>"])
    }
    return inputResult { try Input.move(to: pt) }
}

private func handleMouseScroll(_ request: WireRequest) -> VerbOutcome {
    let parsed = ParsedArgs(request.args)
    guard parsed.positionals.count >= 3,
          let x = Double(parsed.positionals[0]),
          let y = Double(parsed.positionals[1]),
          let notches = Int(parsed.positionals[2]) else {
        return .err(code: "invalid_args", detail: ["message": "expected <x> <y> <notches>"])
    }
    return inputResult { try Input.scroll(at: CGPoint(x: x, y: y), notches: notches) }
}

private func handleMouseDrag(_ request: WireRequest) -> VerbOutcome {
    let parsed = ParsedArgs(request.args)
    guard parsed.positionals.count >= 4,
          let x1 = Double(parsed.positionals[0]),
          let y1 = Double(parsed.positionals[1]),
          let x2 = Double(parsed.positionals[2]),
          let y2 = Double(parsed.positionals[3]) else {
        return .err(code: "invalid_args", detail: ["message": "expected <x1> <y1> <x2> <y2>"])
    }
    return inputResult {
        let button = try parseButton(parsed.flags)
        try Input.drag(from: CGPoint(x: x1, y: y1), to: CGPoint(x: x2, y: y2), button: button)
    }
}

private func handleMousePress(_ request: WireRequest) -> VerbOutcome {
    let parsed = ParsedArgs(request.args)
    guard let pt = parsePoint(parsed.positionals) else {
        return .err(code: "invalid_args", detail: ["message": "expected <x> <y>"])
    }
    return inputResult {
        let button = try parseButton(parsed.flags)
        try Input.press(at: pt, button: button)
    }
}

private func handleMouseRelease(_ request: WireRequest) -> VerbOutcome {
    let parsed = ParsedArgs(request.args)
    guard let pt = parsePoint(parsed.positionals) else {
        return .err(code: "invalid_args", detail: ["message": "expected <x> <y>"])
    }
    return inputResult {
        let button = try parseButton(parsed.flags)
        try Input.release(at: pt, button: button)
    }
}

private func inputResult(_ op: () throws -> Void) -> VerbOutcome {
    do {
        try op()
        return .ok(payload: Data())
    } catch InputError.permissionDenied {
        return .err(code: "permission_denied", detail: [
            "category": "input_monitoring",
            "hint": "Grant in System Settings → Privacy & Security → Input Monitoring, then restart the agent",
        ])
    } catch InputError.unknownButton(let b) {
        return .err(code: "invalid_args", detail: ["message": "unknown button \"\(b)\"; expected left/right/middle"])
    } catch InputError.unknownKey(let k) {
        return .err(code: "invalid_args", detail: ["message": "unknown key \"\(k)\"; see Input.swift KeyName table"])
    } catch InputError.unknownModifier(let m) {
        return .err(code: "invalid_args", detail: ["message": "unknown modifier \"\(m)\"; expected cmd/shift/opt/ctrl/fn"])
    } catch InputError.eventCreationFailed {
        return .err(code: "internal_error", detail: ["message": "CGEvent creation failed"])
    } catch {
        return .err(code: "internal_error", detail: ["message": "\(error)"])
    }
}

// MARK: input.keyboard.*

private func parseModifiers(_ flags: [String: String]) throws -> CGEventFlags {
    guard let raw = flags["modifiers"] else { return [] }
    return try Modifier.combinedFlags(from: raw)
}

private func handleKeyTap(_ request: WireRequest) -> VerbOutcome {
    let parsed = ParsedArgs(request.args)
    guard let name = parsed.positionals.first else {
        return .err(code: "invalid_args", detail: ["message": "input.keyboard.key requires <name>"])
    }
    return inputResult {
        let mods = try parseModifiers(parsed.flags)
        try Input.keyTap(named: name, modifiers: mods)
    }
}

private func handleKeyDown(_ request: WireRequest) -> VerbOutcome {
    let parsed = ParsedArgs(request.args)
    guard let name = parsed.positionals.first else {
        return .err(code: "invalid_args", detail: ["message": "input.keyboard.key_down requires <name>"])
    }
    return inputResult {
        let mods = try parseModifiers(parsed.flags)
        try Input.keyDown(named: name, modifiers: mods)
    }
}

private func handleKeyUp(_ request: WireRequest) -> VerbOutcome {
    let parsed = ParsedArgs(request.args)
    guard let name = parsed.positionals.first else {
        return .err(code: "invalid_args", detail: ["message": "input.keyboard.key_up requires <name>"])
    }
    return inputResult {
        let mods = try parseModifiers(parsed.flags)
        try Input.keyUp(named: name, modifiers: mods)
    }
}

private func handleKeyType(_ request: WireRequest) -> VerbOutcome {
    guard let text = String(data: request.payload, encoding: .utf8) else {
        return .err(code: "invalid_args", detail: ["message": "input.keyboard.type payload must be UTF-8"])
    }
    return inputResult { try Input.typeText(text) }
}

// MARK: input.position

private func handleInputPosition() -> VerbOutcome {
    let p = Input.cursorPosition()
    let body: [String: Any] = ["x": Int(p.x.rounded()), "y": Int(p.y.rounded())]
    let data = (try? JSONSerialization.data(withJSONObject: body, options: [.sortedKeys])) ?? Data()
    return .ok(payload: data)
}

// MARK: process.*

private func procResult<T>(_ op: () throws -> T, encode: (T) -> Data) -> VerbOutcome {
    do {
        let v = try op()
        return .ok(payload: encode(v))
    } catch let e as ProcError {
        return procErrorOutcome(e)
    } catch {
        return .err(code: "internal_error", detail: ["message": "\(error)"])
    }
}

private func procErrorOutcome(_ e: ProcError) -> VerbOutcome {
    switch e {
    case .notFound:         return .err(code: "not_found", detail: [:])
    case .permissionDenied: return .err(code: "permission_denied", detail: [:])
    case .timeout:          return .err(code: "timeout", detail: [:])
    case .spawnFailed(let m): return .err(code: "spawn_failed", detail: ["message": m])
    case .io(let m):        return .err(code: "io_error", detail: ["message": m])
    }
}

private func handleProcessList(_ r: WireRequest) -> VerbOutcome {
    let parsed = ParsedArgs(r.args)
    let filter = parsed.flags["filter"]
    let entries = ProcOps.list(filter: filter)
    let body: [String: Any] = ["processes": entries.map { $0.jsonObject }]
    let data = (try? JSONSerialization.data(withJSONObject: body, options: [.sortedKeys])) ?? Data()
    return .ok(payload: data)
}

private func handleProcessStart(_ r: WireRequest) -> VerbOutcome {
    // Grammar: process.start <executable> [args...] [--stdin <length>]
    // For MVP the executable is positional and remaining positionals are
    // argv. --stdin presence + non-zero length triggers payload-read; the
    // stdin payload is in r.payload.
    let parsed = ParsedArgs(r.args)
    guard let exe = parsed.positionals.first else {
        return .err(code: "invalid_args", detail: ["message": "process.start requires <executable>"])
    }
    let argv = Array(parsed.positionals.dropFirst())
    let stdinPayload: Data? = r.payload.isEmpty ? nil : r.payload
    return procResult({
        let pid = try ProcOps.start(executable: exe, args: argv, stdinPayload: stdinPayload)
        return pid
    }, encode: { pid in
        let body: [String: Any] = ["pid": Int(pid)]
        return (try? JSONSerialization.data(withJSONObject: body, options: [.sortedKeys])) ?? Data()
    })
}

private func handleProcessShell(_ r: WireRequest) -> VerbOutcome {
    // Grammar: process.shell <command-string>  (whole tail is the command;
    // for now we join positionals with spaces — caller should quote the
    // command if it has spaces, per wire-format §1.2.5).
    let parsed = ParsedArgs(r.args)
    let command = parsed.positionals.joined(separator: " ")
    guard !command.isEmpty else {
        return .err(code: "invalid_args", detail: ["message": "process.shell requires a command"])
    }
    return procResult({ try ProcOps.shell(command) }, encode: { result in
        let body: [String: Any] = [
            "exit_code": Int(result.exitCode),
            "stdout": String(data: result.stdout, encoding: .utf8) ?? "",
            "stderr": String(data: result.stderr, encoding: .utf8) ?? "",
        ]
        return (try? JSONSerialization.data(withJSONObject: body, options: [.sortedKeys])) ?? Data()
    })
}

private func handleProcessKill(_ r: WireRequest) -> VerbOutcome {
    let parsed = ParsedArgs(r.args)
    guard let pidStr = parsed.positionals.first, let pid = Int32(pidStr) else {
        return .err(code: "invalid_args", detail: ["message": "process.kill requires <pid>"])
    }
    let force = parsed.flags["force"] != nil
    return procResult({ try ProcOps.kill(pid: pid, force: force); return () }, encode: { _ in Data() })
}

private func handleProcessWait(_ r: WireRequest) -> VerbOutcome {
    let parsed = ParsedArgs(r.args)
    guard let pidStr = parsed.positionals.first, let pid = Int32(pidStr) else {
        return .err(code: "invalid_args", detail: ["message": "process.wait requires <pid>"])
    }
    let now = Int(Date().timeIntervalSince1970 * 1000)
    let deadline = parsed.intFlag("deadline") ?? (now + 30_000)
    return procResult({ try ProcOps.waitFor(pid: pid, deadlineMs: deadline) }, encode: { status in
        let body: [String: Any] = ["exit_code": Int(status)]
        return (try? JSONSerialization.data(withJSONObject: body, options: [.sortedKeys])) ?? Data()
    })
}

// MARK: file.* + directory.*

private func fsResult<T>(_ op: () throws -> T, encode: (T) -> Data) -> VerbOutcome {
    do {
        let v = try op()
        return .ok(payload: encode(v))
    } catch let e as FSError {
        return fsErrorOutcome(e)
    } catch {
        return .err(code: "internal_error", detail: ["message": "\(error)"])
    }
}

private func fsErrorOutcome(_ e: FSError) -> VerbOutcome {
    switch e {
    case .notFound:         return .err(code: "not_found", detail: [:])
    case .alreadyExists:    return .err(code: "already_exists", detail: [:])
    case .notADirectory:    return .err(code: "not_a_directory", detail: [:])
    case .notEmpty:         return .err(code: "not_empty", detail: [:])
    case .permissionDenied: return .err(code: "permission_denied", detail: [:])
    case .crossDevice:      return .err(code: "cross_device", detail: ["hint": "use --cross-fs to enable copy-then-remove fallback"])
    case .timeout:          return .err(code: "timeout", detail: [:])
    case .io(let m):        return .err(code: "io_error", detail: ["message": m])
    }
}

private func handleFileCreate(_ r: WireRequest) -> VerbOutcome {
    guard let path = r.args.first else {
        return .err(code: "invalid_args", detail: ["message": "file.create requires <path>"])
    }
    return fsResult({ try FileSystem.createFile(path); return () }, encode: { _ in Data() })
}

private func handleFileRead(_ r: WireRequest) -> VerbOutcome {
    guard let path = r.args.first else {
        return .err(code: "invalid_args", detail: ["message": "file.read requires <path>"])
    }
    return fsResult({ try FileSystem.readFile(path) }, encode: { $0 })
}

private func handleFileWrite(_ r: WireRequest) -> VerbOutcome {
    // Grammar: file.write <path> <length>
    guard r.args.count >= 2 else {
        return .err(code: "invalid_args", detail: ["message": "file.write requires <path> <length>"])
    }
    let path = r.args[0]
    return fsResult({ try FileSystem.writeFile(path, payload: r.payload); return () }, encode: { _ in Data() })
}

private func handleFileWriteAt(_ r: WireRequest) -> VerbOutcome {
    // Grammar: file.write_at <path> <offset> <length> [--truncate]
    let parsed = ParsedArgs(r.args)
    guard parsed.positionals.count >= 3,
          let offset = Int64(parsed.positionals[1]) else {
        return .err(code: "invalid_args", detail: ["message": "file.write_at requires <path> <offset> <length>"])
    }
    let truncate = parsed.flags["truncate"] != nil
    return fsResult({
        try FileSystem.writeAt(parsed.positionals[0], offset: offset, payload: r.payload, truncate: truncate)
        return ()
    }, encode: { _ in Data() })
}

private func handleFileDelete(_ r: WireRequest) -> VerbOutcome {
    guard let path = r.args.first else {
        return .err(code: "invalid_args", detail: ["message": "file.delete requires <path>"])
    }
    return fsResult({ try FileSystem.deleteFile(path); return () }, encode: { _ in Data() })
}

private func handleFileRename(_ r: WireRequest) -> VerbOutcome {
    let parsed = ParsedArgs(r.args)
    guard parsed.positionals.count >= 2 else {
        return .err(code: "invalid_args", detail: ["message": "file.rename requires <src> <dst>"])
    }
    let overwrite = parsed.flags["overwrite"] != nil
    let crossfs = parsed.flags["cross-fs"] != nil
    return fsResult({
        try FileSystem.rename(src: parsed.positionals[0], dst: parsed.positionals[1],
                              overwrite: overwrite, allowCrossFS: crossfs)
        return ()
    }, encode: { _ in Data() })
}

private func handleFileStat(_ r: WireRequest) -> VerbOutcome {
    guard let path = r.args.first else {
        return .err(code: "invalid_args", detail: ["message": "file.stat requires <path>"])
    }
    return fsResult({ try FileSystem.stat(path) }, encode: { stat in
        (try? JSONSerialization.data(withJSONObject: stat.jsonObject, options: [.sortedKeys])) ?? Data()
    })
}

private func handleFileExists(_ r: WireRequest) -> VerbOutcome {
    guard let path = r.args.first else {
        return .err(code: "invalid_args", detail: ["message": "file.exists requires <path>"])
    }
    let (exists, type) = FileSystem.exists(path)
    var body: [String: Any] = ["exists": exists]
    if let t = type { body["type"] = t.rawValue }
    let data = (try? JSONSerialization.data(withJSONObject: body, options: [.sortedKeys])) ?? Data()
    return .ok(payload: data)
}

private func handleFileWait(_ r: WireRequest) -> VerbOutcome {
    let parsed = ParsedArgs(r.args)
    guard let glob = parsed.positionals.first else {
        return .err(code: "invalid_args", detail: ["message": "file.wait requires <glob>"])
    }
    let interval = parsed.intFlag("interval") ?? 200
    let now = Int(Date().timeIntervalSince1970 * 1000)
    let deadline = parsed.intFlag("deadline") ?? (now + 10_000)
    return fsResult({ try FileSystem.waitForPath(glob, intervalMs: interval, deadlineMs: deadline) }, encode: { stat in
        (try? JSONSerialization.data(withJSONObject: stat.jsonObject, options: [.sortedKeys])) ?? Data()
    })
}

private func handleFileDownload(_ r: WireRequest) -> VerbOutcome {
    // Grammar: file.download <url> <destination>
    guard r.args.count >= 2 else {
        return .err(code: "invalid_args", detail: ["message": "file.download requires <url> <destination>"])
    }
    return fsResult({ try FileSystem.download(url: r.args[0], destination: r.args[1]) }, encode: { stat in
        (try? JSONSerialization.data(withJSONObject: stat.jsonObject, options: [.sortedKeys])) ?? Data()
    })
}

private func handleDirList(_ r: WireRequest) -> VerbOutcome {
    guard let path = r.args.first else {
        return .err(code: "invalid_args", detail: ["message": "directory.list requires <path>"])
    }
    return fsResult({ try FileSystem.listDirectory(path) }, encode: { entries in
        let body: [String: Any] = ["entries": entries.map { $0.jsonObject }]
        return (try? JSONSerialization.data(withJSONObject: body, options: [.sortedKeys])) ?? Data()
    })
}

private func handleDirStat(_ r: WireRequest) -> VerbOutcome {
    guard let path = r.args.first else {
        return .err(code: "invalid_args", detail: ["message": "directory.stat requires <path>"])
    }
    return fsResult({ try FileSystem.directoryStat(path) }, encode: { tuple in
        let body: [String: Any] = ["entries": tuple.count, "mtime": tuple.mtime]
        return (try? JSONSerialization.data(withJSONObject: body, options: [.sortedKeys])) ?? Data()
    })
}

private func handleDirExists(_ r: WireRequest) -> VerbOutcome {
    guard let path = r.args.first else {
        return .err(code: "invalid_args", detail: ["message": "directory.exists requires <path>"])
    }
    let (exists, type) = FileSystem.exists(path)
    let isDir = exists && type == .directory
    let body: [String: Any] = ["exists": isDir]
    let data = (try? JSONSerialization.data(withJSONObject: body, options: [.sortedKeys])) ?? Data()
    return .ok(payload: data)
}

private func handleDirCreate(_ r: WireRequest) -> VerbOutcome {
    let parsed = ParsedArgs(r.args)
    guard let path = parsed.positionals.first else {
        return .err(code: "invalid_args", detail: ["message": "directory.create requires <path>"])
    }
    let parents = parsed.flags["parents"] != nil
    return fsResult({ try FileSystem.createDirectory(path, withParents: parents); return () }, encode: { _ in Data() })
}

private func handleDirRename(_ r: WireRequest) -> VerbOutcome {
    let parsed = ParsedArgs(r.args)
    guard parsed.positionals.count >= 2 else {
        return .err(code: "invalid_args", detail: ["message": "directory.rename requires <src> <dst>"])
    }
    let overwrite = parsed.flags["overwrite"] != nil
    let crossfs = parsed.flags["cross-fs"] != nil
    return fsResult({
        try FileSystem.rename(src: parsed.positionals[0], dst: parsed.positionals[1],
                              overwrite: overwrite, allowCrossFS: crossfs)
        return ()
    }, encode: { _ in Data() })
}

private func handleDirRemove(_ r: WireRequest) -> VerbOutcome {
    let parsed = ParsedArgs(r.args)
    guard let path = parsed.positionals.first else {
        return .err(code: "invalid_args", detail: ["message": "directory.remove requires <path>"])
    }
    let recursive = parsed.flags["recursive"] != nil
    return fsResult({ try FileSystem.removeDirectory(path, recursive: recursive); return () }, encode: { _ in Data() })
}

// MARK: element.*

private func elementResult<T>(_ op: () throws -> T, encode: (T) -> Data) -> VerbOutcome {
    do {
        let r = try op()
        return .ok(payload: encode(r))
    } catch let e as ElementError {
        return elementErrorOutcome(e)
    } catch {
        return .err(code: "internal_error", detail: ["message": "\(error)"])
    }
}

private func elementErrorOutcome(_ e: ElementError) -> VerbOutcome {
    switch e {
    case .axDisabled:
        return .err(code: "ax_disabled", detail: [
            "category": "accessibility",
            "hint": "Grant in System Settings → Privacy & Security → Accessibility, then restart the agent",
        ])
    case .invalidId(let s):
        return .err(code: "invalid_args", detail: ["message": "expected elt:<n>, got \"\(s)\""])
    case .notFound:        return .err(code: "not_found", detail: [:])
    case .noMatch:         return .err(code: "not_found", detail: [:])
    case .readonly(let a): return .err(code: "readonly", detail: ["attribute": a])
    case .actionUnsupported:
        return .err(code: "not_supported_by_target", detail: ["message": "element has no such action / attribute is unsupported"])
    case .timeout:         return .err(code: "timeout", detail: [:])
    case .axError(let m):  return .err(code: "ax_error", detail: ["message": m])
    }
}

private func encodeSnapshots(_ snaps: [Element.Snapshot]) -> Data {
    let body: [String: Any] = ["elements": snaps.map { $0.jsonObject }]
    return (try? JSONSerialization.data(withJSONObject: body, options: [.sortedKeys])) ?? Data()
}

private func encodeSnapshot(_ snap: Element.Snapshot) -> Data {
    return (try? JSONSerialization.data(withJSONObject: snap.jsonObject, options: [.sortedKeys])) ?? Data()
}

private func handleElementList(_ request: WireRequest, table: ElementTable) -> VerbOutcome {
    let parsed = ParsedArgs(request.args)
    var region: CGRect? = nil
    if let raw = parsed.flags["region"] {
        let parts = raw.split(separator: ",").compactMap { Double($0) }
        if parts.count == 4 {
            region = CGRect(x: parts[0], y: parts[1], width: parts[2], height: parts[3])
        }
    }
    return elementResult({ try Element.list(table: table, region: region) }, encode: encodeSnapshots)
}

private func handleElementTree(_ request: WireRequest, table: ElementTable) -> VerbOutcome {
    let parsed = ParsedArgs(request.args)
    guard let id = parsed.positionals.first else {
        return .err(code: "invalid_args", detail: ["message": "element.tree requires <elt:N>"])
    }
    let maxDepth = parsed.intFlag("max-depth") ?? 16
    return elementResult({ try Element.tree(table: table, idStr: id, maxDepth: maxDepth) }, encode: encodeSnapshots)
}

private func handleElementAt(_ request: WireRequest, table: ElementTable) -> VerbOutcome {
    let parsed = ParsedArgs(request.args)
    guard let pt = parsePoint(parsed.positionals) else {
        return .err(code: "invalid_args", detail: ["message": "element.at requires <x> <y>"])
    }
    return elementResult({ try Element.elementAt(table: table, point: pt) }, encode: encodeSnapshot)
}

private func handleElementFind(_ request: WireRequest, table: ElementTable) -> VerbOutcome {
    let parsed = ParsedArgs(request.args)
    guard parsed.positionals.count >= 2 else {
        return .err(code: "invalid_args", detail: ["message": "element.find requires <role> <name-pattern>"])
    }
    return elementResult({
        try Element.find(table: table, role: parsed.positionals[0], pattern: parsed.positionals[1])
    }, encode: encodeSnapshot)
}

private func handleElementWait(_ request: WireRequest, table: ElementTable) -> VerbOutcome {
    let parsed = ParsedArgs(request.args)
    guard parsed.positionals.count >= 2 else {
        return .err(code: "invalid_args", detail: ["message": "element.wait requires <role> <name-pattern>"])
    }
    let interval = parsed.intFlag("interval") ?? 100
    let now = Int(Date().timeIntervalSince1970 * 1000)
    let deadline = parsed.intFlag("deadline") ?? (now + 10_000)
    return elementResult({
        try Element.wait(table: table, role: parsed.positionals[0], pattern: parsed.positionals[1],
                         intervalMs: interval, deadlineMs: deadline)
    }, encode: encodeSnapshot)
}

private func handleElementText(_ request: WireRequest, table: ElementTable) -> VerbOutcome {
    guard let id = request.args.first else {
        return .err(code: "invalid_args", detail: ["message": "element.text requires <elt:N>"])
    }
    return elementResult({ Data(try Element.text(table: table, idStr: id).utf8) }, encode: { $0 })
}

private func handleElementInvoke(_ request: WireRequest, table: ElementTable) -> VerbOutcome {
    guard let id = request.args.first else {
        return .err(code: "invalid_args", detail: ["message": "element.invoke requires <elt:N>"])
    }
    return elementResult({ try Element.invoke(table: table, idStr: id); return () }, encode: { _ in Data() })
}

private func handleElementToggle(_ request: WireRequest, table: ElementTable) -> VerbOutcome {
    guard let id = request.args.first else {
        return .err(code: "invalid_args", detail: ["message": "element.toggle requires <elt:N>"])
    }
    return elementResult({ try Element.toggle(table: table, idStr: id) }, encode: { state in
        (try? JSONSerialization.data(withJSONObject: ["state": state], options: [.sortedKeys])) ?? Data()
    })
}

private func handleElementExpand(_ request: WireRequest, table: ElementTable) -> VerbOutcome {
    guard let id = request.args.first else {
        return .err(code: "invalid_args", detail: ["message": "element.expand requires <elt:N>"])
    }
    return elementResult({ try Element.expand(table: table, idStr: id); return () }, encode: { _ in Data() })
}

private func handleElementCollapse(_ request: WireRequest, table: ElementTable) -> VerbOutcome {
    guard let id = request.args.first else {
        return .err(code: "invalid_args", detail: ["message": "element.collapse requires <elt:N>"])
    }
    return elementResult({ try Element.collapse(table: table, idStr: id); return () }, encode: { _ in Data() })
}

private func handleElementFocus(_ request: WireRequest, table: ElementTable) -> VerbOutcome {
    guard let id = request.args.first else {
        return .err(code: "invalid_args", detail: ["message": "element.focus requires <elt:N>"])
    }
    return elementResult({ try Element.focus(table: table, idStr: id); return () }, encode: { _ in Data() })
}

private func handleElementSetText(_ request: WireRequest, table: ElementTable) -> VerbOutcome {
    guard let id = request.args.first else {
        return .err(code: "invalid_args", detail: ["message": "element.set_text requires <elt:N> <length>"])
    }
    guard let text = String(data: request.payload, encoding: .utf8) else {
        return .err(code: "invalid_args", detail: ["message": "element.set_text payload must be UTF-8"])
    }
    return elementResult({ try Element.setText(table: table, idStr: id, text: text); return () }, encode: { _ in Data() })
}

private func handleElementFindInvoke(_ request: WireRequest, table: ElementTable) -> VerbOutcome {
    let parsed = ParsedArgs(request.args)
    guard parsed.positionals.count >= 2 else {
        return .err(code: "invalid_args", detail: ["message": "element.find_invoke requires <role> <name-pattern>"])
    }
    return elementResult({
        let snap = try Element.find(table: table, role: parsed.positionals[0], pattern: parsed.positionals[1])
        try Element.invoke(table: table, idStr: snap.id)
        return snap
    }, encode: encodeSnapshot)
}

private func handleElementAtInvoke(_ request: WireRequest, table: ElementTable) -> VerbOutcome {
    let parsed = ParsedArgs(request.args)
    guard let pt = parsePoint(parsed.positionals) else {
        return .err(code: "invalid_args", detail: ["message": "element.at_invoke requires <x> <y>"])
    }
    return elementResult({
        let snap = try Element.elementAt(table: table, point: pt)
        try Element.invoke(table: table, idStr: snap.id)
        return snap
    }, encode: encodeSnapshot)
}
