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
    ]

    public static let implementedNamespaces: [String] = ["connection", "system", "screen", "clipboard", "window", "input"]
    public static let implementedVerbs: [String] = Array(specs.keys)
}

// MARK: - Verb handlers

/// Result of running a verb against a connection. The dispatcher does not
/// hold session state — that lives in `ConnectionSession` — so handlers take
/// the relevant pieces in.
public func dispatchVerb(
    _ request: WireRequest,
    currentTier: Tier
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
    } catch InputError.eventCreationFailed {
        return .err(code: "internal_error", detail: ["message": "CGEvent creation failed"])
    } catch {
        return .err(code: "internal_error", detail: ["message": "\(error)"])
    }
}
