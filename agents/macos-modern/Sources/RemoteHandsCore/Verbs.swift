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
        // Windows-only message verbs — intentionally OMITTED from
        // VerbTable.specs (and therefore from system.capabilities).
        // Dispatch fall-through returns not_supported_by_target.

        // Vision.
        "vision.ocr":               VerbSpec(tier: .read,        preHelloOK: false, consumesPayload: true),

        // Registry (Windows-only) and watch.registry / input.{send,post}_message
        // are intentionally OMITTED from VerbTable.specs so they're absent
        // from system.capabilities — per PROTOCOL.md §3.2 ("a verb absent
        // from this map is not implemented"). The dispatch fall-through
        // still returns not_supported_by_target for callers that try them.

        // Watch — all read-tier (subscriptions are observational). watch.registry
        // omitted; Windows-only and absent from capabilities.
        "watch.region":             VerbSpec(tier: .read,        preHelloOK: false),
        "watch.window":             VerbSpec(tier: .read,        preHelloOK: false),
        "watch.process":            VerbSpec(tier: .read,        preHelloOK: false),
        "watch.element":            VerbSpec(tier: .read,        preHelloOK: false),
        "watch.file":               VerbSpec(tier: .read,        preHelloOK: false),
        "watch.cancel":             VerbSpec(tier: .read,        preHelloOK: false),

        // System power.
        "system.power.blockers":    VerbSpec(tier: .read,        preHelloOK: false),
        "system.power.lock":        VerbSpec(tier: .extraRisky,  preHelloOK: false),
        "system.power.cancel":      VerbSpec(tier: .extraRisky,  preHelloOK: false),
        "system.power.shutdown":    VerbSpec(tier: .extraRisky,  preHelloOK: false),
        "system.power.reboot":      VerbSpec(tier: .extraRisky,  preHelloOK: false),
        "system.power.logoff":      VerbSpec(tier: .extraRisky,  preHelloOK: false),
        "system.power.hibernate":   VerbSpec(tier: .extraRisky,  preHelloOK: false),
        "system.power.sleep":       VerbSpec(tier: .extraRisky,  preHelloOK: false),

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
        "file.download":            VerbSpec(tier: .create, preHelloOK: false),
        "directory.list":           VerbSpec(tier: .read,   preHelloOK: false),
        "directory.stat":           VerbSpec(tier: .read,   preHelloOK: false),
        "directory.exists":         VerbSpec(tier: .read,   preHelloOK: false),
        "directory.create":         VerbSpec(tier: .create, preHelloOK: false),
        "directory.rename":         VerbSpec(tier: .update, preHelloOK: false),
        "directory.remove":         VerbSpec(tier: .delete, preHelloOK: false),
        // directory.delete is the post-rc.2 canonical name for the same verb;
        // both ship and route to the same handler.
        "directory.delete":         VerbSpec(tier: .delete, preHelloOK: false),

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

    public static let implementedNamespaces: [String] = ["connection", "system", "screen", "clipboard", "window", "input", "element", "file", "directory", "process", "vision", "watch"]
    public static let implementedVerbs: [String] = Array(specs.keys)
}

// MARK: - Verb handlers

/// Result of running a verb against a connection. The dispatcher does not
/// hold session state — that lives in `ConnectionSession` — so handlers take
/// the relevant pieces in.
public func dispatchVerb(
    _ request: WireRequest,
    context: DispatchContext
) -> VerbOutcome {
    let currentTier = context.currentTier
    let elementTable = context.elementTable
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
    case "connection.tier_raise": return handleTierRaise(request, currentTier: currentTier, tokenStore: context.tokenStore)
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
    case "input.mouse.click":  return handleMouseClick(request, table: elementTable)
    case "input.mouse.move":   return handleMouseMove(request)
    case "input.mouse.scroll": return handleMouseScroll(request)
    case "input.mouse.drag":   return handleMouseDrag(request)
    case "input.mouse.press":  return handleMousePress(request)
    case "input.mouse.release":return handleMouseRelease(request)
    case "input.keyboard.key":      return handleKeyTap(request)
    case "input.keyboard.key_down": return handleKeyDown(request)
    case "input.keyboard.key_up":   return handleKeyUp(request)
    case "input.keyboard.type":     return handleKeyType(request)
    case "input.position":          return handleInputPosition(request)
    case "input.send_message", "input.post_message":
        return .err(code: "not_supported_by_target", detail: ["verb": request.verb])
    case "vision.ocr":            return handleVisionOCR(request)
    case "watch.region":          return handleWatchRegion(request, context: context)
    case "watch.window":          return handleWatchWindow(request, context: context)
    case "watch.process":         return handleWatchProcess(request, context: context)
    case "watch.element":         return handleWatchElement(request, context: context)
    case "watch.file":            return handleWatchFile(request, context: context)
    case "watch.registry",
         "registry.key.read", "registry.key.delete",
         "registry.value.read", "registry.value.create",
         "registry.value.update", "registry.value.delete":
        return .err(code: "not_supported_by_target", detail: [
            "verb": request.verb,
            "reason": "Windows registry has no macOS equivalent",
        ])
    case "watch.cancel":          return handleWatchCancel(request, context: context)
    case "system.power.blockers": return handlePowerBlockers()
    case "system.power.lock":     return powerResult { try Power.lock() }
    case "system.power.shutdown": return powerResult { try Power.shutdown() }
    case "system.power.reboot":   return powerResult { try Power.reboot() }
    case "system.power.logoff":   return powerResult { try Power.logoff() }
    case "system.power.hibernate":return powerResult { try Power.hibernate() }
    case "system.power.sleep":    return powerResult { try Power.sleep() }
    case "system.power.cancel":
        // No --delay support implemented yet, so nothing is ever pending.
        return .err(code: "not_found", detail: ["message": "no pending shutdown"])
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
    case "directory.remove",
         "directory.delete":     return handleDirRemove(request)
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

private func handleTierRaise(_ request: WireRequest, currentTier: Tier, tokenStore: TokenStore?) -> VerbOutcome {
    // Accept both `<tier> <token>` positional form (ARH bootstrap clients)
    // and `--tier <tier> --token <token>` flag form (the conformance
    // WireClient + MCP tools/call shape).
    let parsed = ParsedArgs(request.args)
    let tierStr = parsed.flags["tier"] ?? parsed.positionals.first
    let tokenStr = parsed.flags["token"] ?? (parsed.positionals.count >= 2 ? parsed.positionals[1] : nil)
    guard let tierStr = tierStr, let tokenStr = tokenStr else {
        return .err(code: "invalid_args", detail: ["message": "expected <tier> <token> or --tier T --token X"])
    }
    guard let target = Tier(rawValue: tierStr) else {
        return .err(code: "invalid_args", detail: ["message": "unknown tier \"\(tierStr)\""])
    }
    guard let store = tokenStore else {
        return .err(code: "not_supported_by_target", detail: ["message": "agent started without token store"])
    }
    if !store.matches(tokenStr) {
        return .err(code: "auth_invalid", detail: ["message": "token mismatch"])
    }
    // Token valid — grant requested tier. Per PROTOCOL.md §2.3 the new tier
    // can be at, above, or below the current tier (this verb is the
    // elevation primitive; tier_drop is the un-token'd downgrade).
    let body: [String: String] = ["new_tier": target.rawValue]
    let data = (try? JSONSerialization.data(withJSONObject: body, options: [.sortedKeys])) ?? Data()
    return .okWithTierChange(newTier: target, payload: data)
}

private func handleTierDrop(_ request: WireRequest, currentTier: Tier) -> VerbOutcome {
    guard let target = ParsedArgs(request.args).arg("tier").flatMap(Tier.init(rawValue:)) else {
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
    // Post-rc.2 shape: `{verbs: {<name>: <strict-tool-def>, ...}}` — a
    // MAP keyed by verb name, where each value is a strict tool
    // definition with name + description + input_schema. x-tier and
    // x-namespace are advertised as extensions.
    var verbMap: [String: [String: Any]] = [:]
    for (name, spec) in VerbTable.specs {
        verbMap[name] = [
            "name": name,
            "description": "Agent verb \(name)",
            "input_schema": [
                "type": "object",
                "properties": [String: Any]() as [String: Any],
                "additionalProperties": true,
            ] as [String: Any],
            "x-tier": spec.tier.rawValue,
            "x-namespace": name.split(separator: ".").first.map(String.init) ?? "",
        ]
    }
    let body: [String: Any] = ["verbs": verbMap]
    let data = (try? JSONSerialization.data(withJSONObject: body, options: [.sortedKeys])) ?? Data()
    return .ok(payload: data)
}

// MARK: screen.*

private func handleScreenCapture(_ request: WireRequest) -> VerbOutcome {
    let args = ParsedArgs(request.args)

    // Reject unknown flags rather than silently ignoring them — a typo
    // such as `--cursor` for `--no-cursor` must fail loudly.
    let knownFlags: Set<String> = [
        "format", "quality", "region", "window", "monitor", "no-cursor", "encoding",
    ]
    if let unknown = args.flags.keys.first(where: { !knownFlags.contains($0) }) {
        return .err(code: "invalid_args", detail: ["unknown_flag": "--\(unknown)"])
    }

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

    // The `binary` encoding (Shape-B side-channel — a raw blob trailing the
    // JSON frame) is a windows-modern-only path. This family returns the
    // image as an MCP image content item; reject the side-channel request.
    if let encoding = args.flags["encoding"], encoding == "binary" {
        return .err(code: "unsupported_format", detail: ["encoding": encoding])
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
        return .err(code: "empty", detail: [:])
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
    let includeAll = args.flags["all"] != nil || args.flags["visible-only"] == "false"
    let windows = Window.list(filter: filter, includeAll: includeAll)
    // v2.2 conformance expects a bare array, not {"windows": [...]}.
    let arr = windows.map { $0.jsonObject }
    guard let data = try? JSONSerialization.data(withJSONObject: arr, options: [.sortedKeys]) else {
        return .err(code: "internal_error", detail: ["message": "window.list JSON encoding failed"])
    }
    return .ok(payload: data)
}

private func handleWindowFind(_ request: WireRequest) -> VerbOutcome {
    let parsed = ParsedArgs(request.args)
    if let unknown = parsed.unknownFlag(allowed: ["pattern", "match"]) {
        return .err(code: "invalid_args", detail: ["unknown_flag": "--\(unknown)"])
    }
    // --match is a documented enum; an unknown mode is invalid_args. The
    // agent matches by case-insensitive substring regardless of the
    // requested mode — honouring prefix/exact/glob/regex is a separate
    // feature.
    if let m = parsed.flags["match"],
       !["substring", "prefix", "exact", "glob", "regex"].contains(m) {
        return .err(code: "invalid_args", detail: ["message": "unknown --match mode \"\(m)\""])
    }
    guard let pattern = parsed.arg("pattern") else {
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
    guard let id = ParsedArgs(request.args).arg("handle") else {
        return .err(code: "invalid_args", detail: ["message": "window.focus requires <handle>"])
    }
    return windowActionResult { try Window.focus(idStr: id) }
}

private func handleWindowClose(_ request: WireRequest) -> VerbOutcome {
    guard let id = ParsedArgs(request.args).arg("handle") else {
        return .err(code: "invalid_args", detail: ["message": "window.close requires <handle>"])
    }
    return windowActionResult { try Window.close(idStr: id) }
}

private func handleWindowMove(_ request: WireRequest) -> VerbOutcome {
    // Spec: handle/x/y required; w/h optional (omitted -> size preserved).
    // Legacy form: positionals <handle> <x> <y> <w> <h>.
    let parsed = ParsedArgs(request.args)
    guard let id = parsed.arg("handle", positional: 0),
          let xs = parsed.arg("x", positional: 1), let x = Int(xs),
          let ys = parsed.arg("y", positional: 2), let y = Int(ys) else {
        return .err(code: "invalid_args", detail: ["message": "window.move requires <handle> <x> <y>"])
    }
    let wFlag = parsed.arg("w", positional: 3).flatMap { Int($0) }
    let hFlag = parsed.arg("h", positional: 4).flatMap { Int($0) }
    var w = wFlag ?? 0
    var h = hFlag ?? 0
    if wFlag == nil || hFlag == nil {
        // Fill omitted width/height from the window's current bounds.
        guard let cur = try? Window.resolveWindow(idStr: id).bounds else {
            return .err(code: "not_found", detail: ["message": "no window for handle \"\(id)\""])
        }
        if wFlag == nil { w = Int(cur.size.width) }
        if hFlag == nil { h = Int(cur.size.height) }
    }
    let rect = CGRect(x: x, y: y, width: w, height: h)
    return windowActionResult { try Window.move(idStr: id, to: rect) }
}

private func handleWindowState(_ request: WireRequest) -> VerbOutcome {
    guard let id = ParsedArgs(request.args).arg("handle") else {
        return .err(code: "invalid_args", detail: ["message": "window.state requires <handle>"])
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

private func parsePoint(_ parsed: ParsedArgs) -> CGPoint? {
    // --x/--y flags (spec form) or two leading positionals (legacy form).
    if let xs = parsed.flags["x"], let ys = parsed.flags["y"],
       let x = Double(xs), let y = Double(ys) {
        return CGPoint(x: x, y: y)
    }
    let p = parsed.positionals
    guard p.count >= 2, let x = Double(p[0]), let y = Double(p[1]) else { return nil }
    return CGPoint(x: x, y: y)
}

private func parseButton(_ flags: [String: String]) throws -> MouseButton {
    let raw = flags["button"] ?? "left"
    return try MouseButton(parsing: raw)
}

/// Build the input.mouse.click OK body (R5): the synthesised flag, the
/// actual click position, the front-most window handle under the point,
/// and an AX hit-test of the element there. `target_element.flags` is
/// empty — populating element-state flags is a separate deferred feature.
private func clickResultBody(point pt: CGPoint, table: ElementTable) -> Data {
    var body: [String: Any] = [
        "synthesised": true,
        "actual_position": ["x": Int(pt.x), "y": Int(pt.y)] as [String: Int],
    ]
    if let win = Window.list().first(where: { $0.bounds.contains(pt) }) {
        body["target_handle"] = win.idString
    } else {
        body["target_handle"] = NSNull()
    }
    if let snap = try? Element.elementAt(table: table, point: pt) {
        var te: [String: Any] = [
            "name": snap.title, "role": snap.role, "flags": [String](),
        ]
        if let b = snap.bounds {
            te["bounds"] = [
                "x": Int(b.origin.x), "y": Int(b.origin.y),
                "w": Int(b.size.width), "h": Int(b.size.height),
            ] as [String: Int]
        }
        body["target_element"] = te
    } else {
        body["target_element"] = NSNull()
    }
    return (try? JSONSerialization.data(withJSONObject: body, options: [.sortedKeys])) ?? Data()
}

private func handleMouseClick(_ request: WireRequest, table: ElementTable) -> VerbOutcome {
    let parsed = ParsedArgs(request.args)
    if let bad = parsed.unknownFlag(allowed: [
        "x", "y", "button", "clicks", "double", "triple", "duration-ms", "clicks-interval-ms"
    ]) {
        return .err(code: "invalid_args", detail: ["unknown_flag": "--\(bad)"])
    }
    guard let pt = parsePoint(parsed) else {
        return .err(code: "invalid_args", detail: ["message": "expected <x> <y> or --x/--y"])
    }
    // Flag mutual-exclusion + range validation per the input.mouse.click
    // x-conditional rules. The verb dispatcher rejects ambiguous combos
    // up front rather than picking a winner.
    let hasDouble = parsed.flags["double"] != nil
    let hasTriple = parsed.flags["triple"] != nil
    let hasClicks = parsed.flags["clicks"] != nil
    let hasDuration = parsed.flags["duration-ms"] != nil
    let hasClicksInterval = parsed.flags["clicks-interval-ms"] != nil
    if hasDouble && hasTriple {
        return .err(code: "invalid_args", detail: ["message": "--double and --triple are mutually exclusive"])
    }
    if hasClicks && hasDouble {
        return .err(code: "invalid_args", detail: ["message": "--clicks and --double are mutually exclusive"])
    }
    if hasClicks && hasTriple {
        return .err(code: "invalid_args", detail: ["message": "--clicks and --triple are mutually exclusive"])
    }
    if hasDuration && (hasDouble || hasTriple || hasClicks) {
        return .err(code: "invalid_args", detail: ["message": "--duration-ms is mutually exclusive with click-count flags"])
    }
    if hasClicksInterval && !hasClicks {
        return .err(code: "invalid_args", detail: ["message": "--clicks-interval-ms requires --clicks"])
    }
    if let dur = parsed.intFlag("duration-ms"), !(0...1000).contains(dur) {
        return .err(code: "invalid_args", detail: ["message": "--duration-ms must be in [0, 1000]"])
    }
    var clicks = 1
    if let c = parsed.intFlag("clicks") {
        guard (2...10).contains(c) else {
            return .err(code: "invalid_args", detail: ["message": "--clicks must be in [2, 10]"])
        }
        clicks = c
    } else if hasDouble {
        clicks = 2
    } else if hasTriple {
        clicks = 3
    }
    return inputResultData {
        let button = try parseButton(parsed.flags)
        // Resolve what is under the point before the click can change the UI.
        let body = clickResultBody(point: pt, table: table)
        try Input.click(at: pt, button: button, clicks: clicks)
        return body
    }
}

private func handleMouseMove(_ request: WireRequest) -> VerbOutcome {
    guard let pt = parsePoint(ParsedArgs(request.args)) else {
        return .err(code: "invalid_args", detail: ["message": "expected <x> <y>"])
    }
    return inputResult { try Input.move(to: pt) }
}

private func handleMouseScroll(_ request: WireRequest) -> VerbOutcome {
    let parsed = ParsedArgs(request.args)
    guard let pt = parsePoint(parsed),
          let deltaStr = parsed.arg("delta", positional: 2),
          let notches = Int(deltaStr) else {
        return .err(code: "invalid_args", detail: ["message": "expected <x> <y> <delta>"])
    }
    return inputResult { try Input.scroll(at: pt, notches: notches) }
}

private func handleMouseDrag(_ request: WireRequest) -> VerbOutcome {
    let parsed = ParsedArgs(request.args)
    if let bad = parsed.unknownFlag(allowed: ["x", "y", "button", "steps"]) {
        return .err(code: "invalid_args", detail: ["unknown_flag": "--\(bad)"])
    }
    // Spec form: drag from the current cursor position to --x/--y. Legacy
    // form: four positionals <x1> <y1> <x2> <y2> (explicit start + end).
    let start: CGPoint
    let end: CGPoint
    let p = parsed.positionals
    if p.count >= 4, let x1 = Double(p[0]), let y1 = Double(p[1]),
       let x2 = Double(p[2]), let y2 = Double(p[3]) {
        start = CGPoint(x: x1, y: y1)
        end = CGPoint(x: x2, y: y2)
    } else if let target = parsePoint(parsed) {
        start = Input.cursorPosition()
        end = target
    } else {
        return .err(code: "invalid_args", detail: ["message": "drag requires --x/--y (or legacy <x1> <y1> <x2> <y2>)"])
    }
    let steps = parsed.intFlag("steps") ?? 10
    return inputResult {
        let button = try parseButton(parsed.flags)
        try Input.drag(from: start, to: end, button: button, steps: steps)
    }
}

private func handleMousePress(_ request: WireRequest) -> VerbOutcome {
    let parsed = ParsedArgs(request.args)
    // Spec: press at the current cursor position; --x/--y optionally
    // override the press location.
    let pt = parsePoint(parsed) ?? Input.cursorPosition()
    return inputResult {
        let button = try parseButton(parsed.flags)
        try Input.press(at: pt, button: button)
    }
}

private func handleMouseRelease(_ request: WireRequest) -> VerbOutcome {
    let parsed = ParsedArgs(request.args)
    // Spec: release at the current cursor position; --x/--y optionally
    // override the release location.
    let pt = parsePoint(parsed) ?? Input.cursorPosition()
    return inputResult {
        let button = try parseButton(parsed.flags)
        try Input.release(at: pt, button: button)
    }
}

private func inputResult(_ op: () throws -> Void) -> VerbOutcome {
    return inputResultData { try op(); return Data() }
}

/// Like `inputResult`, but the operation produces the OK payload.
private func inputResultData(_ op: () throws -> Data) -> VerbOutcome {
    do {
        return .ok(payload: try op())
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
    if let bad = parsed.unknownFlag(allowed: ["modifiers"]) {
        return .err(code: "invalid_args", detail: ["unknown_flag": "--\(bad)"])
    }
    guard let name = parsed.arg("vk") else {
        return .err(code: "invalid_args", detail: ["message": "input.keyboard.key requires <name>"])
    }
    return inputResult {
        let mods = try parseModifiers(parsed.flags)
        try Input.keyTap(named: name, modifiers: mods)
    }
}

private func handleKeyDown(_ request: WireRequest) -> VerbOutcome {
    let parsed = ParsedArgs(request.args)
    guard let name = parsed.arg("vk") else {
        return .err(code: "invalid_args", detail: ["message": "input.keyboard.key_down requires <name>"])
    }
    return inputResult {
        let mods = try parseModifiers(parsed.flags)
        try Input.keyDown(named: name, modifiers: mods)
    }
}

private func handleKeyUp(_ request: WireRequest) -> VerbOutcome {
    let parsed = ParsedArgs(request.args)
    if let bad = parsed.unknownFlag(allowed: ["modifiers"]) {
        return .err(code: "invalid_args", detail: ["unknown_flag": "--\(bad)"])
    }
    guard let name = parsed.arg("vk") else {
        return .err(code: "invalid_args", detail: ["message": "input.keyboard.key_up requires <name>"])
    }
    // key_up is idempotent — releasing a key that wasn't held (or a key
    // name the agent doesn't know about) must return OK 0, not ERR.
    // Cleanup-fail-safe path. Bad modifier strings still ERR — those are
    // a real programming error.
    do {
        let mods = try parseModifiers(parsed.flags)
        do {
            try Input.keyUp(named: name, modifiers: mods)
        } catch InputError.unknownKey {
            // Silently OK — release on an unknown key is a no-op.
            return .ok(payload: Data())
        }
        return .ok(payload: Data())
    } catch InputError.permissionDenied {
        return .err(code: "permission_denied", detail: [
            "category": "input_monitoring",
            "hint": "Grant in System Settings → Privacy & Security → Input Monitoring",
        ])
    } catch InputError.unknownModifier(let m) {
        return .err(code: "invalid_args", detail: ["message": "unknown modifier \"\(m)\""])
    } catch {
        return .err(code: "internal_error", detail: ["message": "\(error)"])
    }
}

private func handleKeyType(_ request: WireRequest) -> VerbOutcome {
    guard let text = String(data: request.payload, encoding: .utf8) else {
        return .err(code: "invalid_args", detail: ["message": "input.keyboard.type payload must be UTF-8"])
    }
    return inputResult { try Input.typeText(text) }
}

// MARK: input.position

private func handleInputPosition(_ request: WireRequest) -> VerbOutcome {
    let parsed = ParsedArgs(request.args)
    let p = Input.cursorPosition()
    var body: [String: Any] = ["x": Int(p.x.rounded()), "y": Int(p.y.rounded())]
    if parsed.flags["include-monitor"] != nil {
        // Determine which NSScreen contains the cursor. Origin matters:
        // NSEvent.mouseLocation uses bottom-left, NSScreen.frame uses
        // bottom-left too, but our `p` here is in CG coords (top-left).
        // Compare in CG space against converted screen frames.
        let primaryHeight = NSScreen.main?.frame.height ?? 0
        var idx = 0
        for (i, screen) in NSScreen.screens.enumerated() {
            let f = screen.frame
            // Convert NSScreen frame (Cocoa, lower-left origin) to CG (top-left).
            let cgY = primaryHeight - f.origin.y - f.size.height
            if p.x >= f.origin.x && p.x < f.origin.x + f.size.width &&
               p.y >= cgY && p.y < cgY + f.size.height {
                idx = i
                break
            }
        }
        body["monitor_index"] = idx
    }
    let data = (try? JSONSerialization.data(withJSONObject: body, options: [.sortedKeys])) ?? Data()
    return .ok(payload: data)
}

// MARK: watch.*

private func subscriptionResponse(_ id: String) -> VerbOutcome {
    let body: [String: Any] = ["subscription_id": id]
    let data = (try? JSONSerialization.data(withJSONObject: body, options: [.sortedKeys])) ?? Data()
    return .ok(payload: data)
}

private func handleWatchRegion(_ r: WireRequest, context: DispatchContext) -> VerbOutcome {
    let parsed = ParsedArgs(r.args)
    let interval = parsed.intFlag("interval") ?? 500
    let untilChange = parsed.flags["until-change"] != nil
    let id = context.subscriptions.nextSubID()
    let sub = RegionWatchSubscription(
        id: id, intervalMs: interval, untilChange: untilChange,
        registry: context.subscriptions, send: context.sendEvent
    )
    context.subscriptions.add(sub)
    return subscriptionResponse(id)
}

private func handleWatchWindow(_ r: WireRequest, context: DispatchContext) -> VerbOutcome {
    let parsed = ParsedArgs(r.args)
    // --title-prefix is required (post-rc.2). Without it, the subscription
    // would emit events for every app launch on the system — too noisy and
    // not what most callers want.
    guard let prefix = parsed.flags["title-prefix"], !prefix.isEmpty else {
        return .err(code: "invalid_args", detail: ["message": "watch.window requires --title-prefix"])
    }
    let id = context.subscriptions.nextSubID()
    let sub = WindowWatchSubscription(id: id, titlePrefix: prefix, send: context.sendEvent)
    context.subscriptions.add(sub)
    return subscriptionResponse(id)
}

private func handleWatchProcess(_ r: WireRequest, context: DispatchContext) -> VerbOutcome {
    guard let pidStr = ParsedArgs(r.args).arg("pid"), let pid = Int32(pidStr) else {
        return .err(code: "invalid_args", detail: ["message": "watch.process requires <pid>"])
    }
    let id = context.subscriptions.nextSubID()
    guard let sub = ProcessExitSubscription(id: id, pid: pid, send: context.sendEvent) else {
        return .err(code: "internal_error", detail: ["message": "DispatchSource.makeProcessSource failed"])
    }
    context.subscriptions.add(sub)
    return subscriptionResponse(id)
}

private func handleWatchElement(_ r: WireRequest, context: DispatchContext) -> VerbOutcome {
    guard let elt = ParsedArgs(r.args).arg("handle") else {
        return .err(code: "invalid_args", detail: ["message": "watch.element requires <elt:N>"])
    }
    do {
        let (pid, element) = try context.elementTable.lookup(id: elt)
        let id = context.subscriptions.nextSubID()
        guard let sub = ElementWatchSubscription(id: id, pid: pid, element: element, send: context.sendEvent) else {
            return .err(code: "ax_error", detail: ["message": "AXObserver setup failed (check Accessibility TCC)"])
        }
        context.subscriptions.add(sub)
        return subscriptionResponse(id)
    } catch let e as ElementError {
        return elementErrorOutcome(e)
    } catch {
        return .err(code: "internal_error", detail: ["message": "\(error)"])
    }
}

private func handleWatchFile(_ r: WireRequest, context: DispatchContext) -> VerbOutcome {
    guard let glob = ParsedArgs(r.args).arg("glob") else {
        return .err(code: "invalid_args", detail: ["message": "watch.file requires <glob>"])
    }
    let id = context.subscriptions.nextSubID()
    guard let sub = FileWatchSubscription(id: id, glob: glob, send: context.sendEvent) else {
        return .err(code: "internal_error", detail: ["message": "FSEventStreamCreate failed"])
    }
    context.subscriptions.add(sub)
    return subscriptionResponse(id)
}

private func handleWatchCancel(_ r: WireRequest, context: DispatchContext) -> VerbOutcome {
    guard let id = ParsedArgs(r.args).arg("subscription-id") else {
        return .err(code: "invalid_args", detail: ["message": "watch.cancel requires <sub:N>"])
    }
    let was = context.subscriptions.cancel(id: id)
    let body: [String: Any] = ["cancelled": was]
    let data = (try? JSONSerialization.data(withJSONObject: body, options: [.sortedKeys])) ?? Data()
    return .ok(payload: data)
}

// MARK: vision.ocr

private func handleVisionOCR(_ r: WireRequest) -> VerbOutcome {
    let parsed = ParsedArgs(r.args)
    let accurate = parsed.flags["fast"] == nil  // default accurate; --fast for speed
    let language = parsed.flags["language"]
    do {
        let source = try VisionSource.resolve(parsed, payload: r.payload)
        let result = try VisionOps.ocr(cgImage: source.cgImage, language: language, accurate: accurate)

        // Screen-space sources report boxes in screen points: scale the
        // image-pixel boxes back to points and offset by the source
        // origin. Image-space sources report image-pixel boxes unchanged.
        let pointScale = source.coordinateSpace == "screen"
            ? Double(NSScreen.main?.backingScaleFactor ?? 1.0) : 1.0
        let ox = Double(source.origin.x)
        let oy = Double(source.origin.y)

        let lines: [[String: Any]] = result.observations.map { obs in
            let bx = obs.bounds.origin.x / pointScale + ox
            let by = obs.bounds.origin.y / pointScale + oy
            let bw = obs.bounds.size.width / pointScale
            let bh = obs.bounds.size.height / pointScale
            return [
                "text": obs.text,
                "bbox": [
                    "x": Int(bx.rounded()), "y": Int(by.rounded()),
                    "w": Int(bw.rounded()), "h": Int(bh.rounded()),
                ] as [String: Int],
            ]
        }
        let body: [String: Any] = [
            "text": result.observations.map { $0.text }.joined(separator: "\n"),
            "lines": lines,
            "language_used": result.languageUsed,
            "coordinate_space": source.coordinateSpace,
            "image_size": [
                "w": Int(result.imageSize.width / pointScale),
                "h": Int(result.imageSize.height / pointScale),
            ] as [String: Int],
            "text_angle": 0.0,
        ]
        let data = (try? JSONSerialization.data(withJSONObject: body, options: [.sortedKeys])) ?? Data()
        return .ok(payload: data)
    } catch let e as VisionSourceError {
        switch e {
        case .invalidArgs(let m): return .err(code: "invalid_args", detail: ["message": m])
        case .notFound(let m):    return .err(code: "not_found", detail: ["message": m])
        }
    } catch CaptureError.permissionDenied {
        return .err(code: "permission_denied", detail: [
            "category": "screen_recording",
            "hint": "Grant in System Settings → Privacy & Security → Screen Recording, then restart the agent",
        ])
    } catch let e as CaptureError {
        return .err(code: "capture_failed", detail: ["message": "\(e)"])
    } catch VisionError.decodeFailed(let m) {
        return .err(code: "invalid_args", detail: ["message": "image decode failed: \(m)"])
    } catch VisionError.ocrFailed(let m) {
        return .err(code: "ocr_failed", detail: ["message": m])
    } catch {
        return .err(code: "internal_error", detail: ["message": "\(error)"])
    }
}

// MARK: system.power.*

private func handlePowerBlockers() -> VerbOutcome {
    // Spec output schema wraps the list in a `blockers` array.
    let blockers = Power.blockers()
    let body: [String: Any] = ["blockers": blockers.map { $0.jsonObject }]
    let data = (try? JSONSerialization.data(withJSONObject: body, options: [.sortedKeys])) ?? Data()
    return .ok(payload: data)
}

private func powerResult(_ op: () throws -> Void) -> VerbOutcome {
    do {
        try op()
        return .ok(payload: Data())
    } catch PowerError.permissionDenied {
        return .err(code: "permission_denied", detail: ["category": "apple_events"])
    } catch PowerError.notFound {
        return .err(code: "not_found", detail: [:])
    } catch PowerError.unsupported {
        return .err(code: "not_supported", detail: [:])
    } catch PowerError.powerStateChangesDisabled {
        return .err(code: "not_supported", detail: [
            "category": "power_state_changes_disabled",
            "hint": "agent refuses power-state-changing verbs by default; restart with --allow-power-state-changes to enable shutdown/reboot/logoff/sleep/hibernate. system.power.lock and system.power.blockers are always available.",
        ])
    } catch PowerError.io(let m) {
        return .err(code: "io_error", detail: ["message": m])
    } catch {
        return .err(code: "internal_error", detail: ["message": "\(error)"])
    }
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
    // --pattern is the post-rc.2 canonical name; --filter retained as alias.
    let filter = parsed.flags["pattern"] ?? parsed.flags["filter"]
    let includeCounters = parsed.flags["include-counters"] != nil
    let entries = ProcOps.list(filter: filter)
    let body: [String: Any] = ["processes": entries.map { $0.jsonObject(includeCounters: includeCounters) }]
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
    guard let pidStr = parsed.arg("pid"), let pid = Int32(pidStr) else {
        return .err(code: "invalid_args", detail: ["message": "process.kill requires <pid>"])
    }
    let force = parsed.flags["force"] != nil
    return procResult({ try ProcOps.kill(pid: pid, force: force); return () }, encode: { _ in Data() })
}

private func handleProcessWait(_ r: WireRequest) -> VerbOutcome {
    let parsed = ParsedArgs(r.args)
    guard let pidStr = parsed.arg("pid"), let pid = Int32(pidStr) else {
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
    case .protectedPath:
        return .err(code: "permission_denied", detail: [
            "category": "protected_path",
            "hint": "the agent blocks file/directory access to its own token file, keychain, and SSH keys",
        ])
    case .crossDevice:      return .err(code: "cross_device", detail: ["hint": "use --cross-fs to enable copy-then-remove fallback"])
    case .timeout:          return .err(code: "timeout", detail: [:])
    case .io(let m):        return .err(code: "io_error", detail: ["message": m])
    }
}

private func handleFileCreate(_ r: WireRequest) -> VerbOutcome {
    // Path: --path flag (spec form) or a leading positional.
    // Content: --content flag, else the wire payload.
    let parsed = ParsedArgs(r.args)
    guard let path = parsed.flags["path"] ?? parsed.positionals.first else {
        return .err(code: "invalid_args", detail: ["message": "file.create requires <path>"])
    }
    let initial: Data
    if let content = parsed.flags["content"] {
        initial = Data(content.utf8)
    } else {
        initial = r.payload
    }
    return fsResult({
        try FileSystem.createFile(path)
        if !initial.isEmpty {
            try FileSystem.writeAt(path, offset: 0, payload: initial, truncate: false)
        }
        return ["created": true] as [String: Any]
    }, encode: encodeJSON)
}

private func encodeJSON(_ obj: [String: Any]) -> Data {
    return (try? JSONSerialization.data(withJSONObject: obj, options: [.sortedKeys])) ?? Data()
}

private func handleFileRead(_ r: WireRequest) -> VerbOutcome {
    guard let path = ParsedArgs(r.args).arg("path") else {
        return .err(code: "invalid_args", detail: ["message": "file.read requires <path>"])
    }
    return fsResult({ try FileSystem.readFile(path) }, encode: { $0 })
}

private func handleFileWrite(_ r: WireRequest) -> VerbOutcome {
    // Grammar: file.write <path> <length>            (with payload)
    //          file.write <path> --content X         (content in flag form)
    // file.write is U-tier — it overwrites an existing file; it does NOT
    // create. Missing target → not_found (use file.create).
    let parsed = ParsedArgs(r.args)
    guard let path = parsed.arg("path") else {
        return .err(code: "invalid_args", detail: ["message": "file.write requires <path>"])
    }
    let payload: Data
    if let content = parsed.flags["content"] {
        payload = Data(content.utf8)
    } else {
        payload = r.payload
    }
    return fsResult({
        // Pre-flight: ensure the file exists. writeFile would create it
        // otherwise, which is file.create's job.
        let (exists, type) = FileSystem.exists(path)
        if !exists || type != .file {
            throw FSError.notFound
        }
        try FileSystem.writeFile(path, payload: payload)
        return ["written": true, "bytes": payload.count] as [String: Any]
    }, encode: encodeJSON)
}

private func handleFileWriteAt(_ r: WireRequest) -> VerbOutcome {
    // Grammar: file.write_at <path> <offset> [--truncate]; content via payload.
    let parsed = ParsedArgs(r.args)
    guard let path = parsed.arg("path"),
          let offsetStr = parsed.arg("offset", positional: 1),
          let offset = Int64(offsetStr) else {
        return .err(code: "invalid_args", detail: ["message": "file.write_at requires <path> <offset>"])
    }
    let truncate = parsed.flags["truncate"] != nil
    return fsResult({
        try FileSystem.writeAt(path, offset: offset, payload: r.payload, truncate: truncate)
        return ()
    }, encode: { _ in Data() })
}

private func handleFileDelete(_ r: WireRequest) -> VerbOutcome {
    let parsed = ParsedArgs(r.args)
    guard let path = parsed.arg("path") else {
        return .err(code: "invalid_args", detail: ["message": "file.delete requires <path>"])
    }
    let permanent = parsed.flags["permanent"] != nil
    return fsResult({ try FileSystem.deleteFile(path, permanent: permanent) }, encode: encodeDeleteOutcome)
}

private func encodeDeleteOutcome(_ outcome: FileSystem.DeleteOutcome) -> Data {
    var body: [String: Any] = ["mode": outcome.mode.rawValue]
    if let url = outcome.trashURL {
        body["trash_url"] = url.absoluteString
    }
    return (try? JSONSerialization.data(withJSONObject: body, options: [.sortedKeys])) ?? Data()
}

private func handleFileRename(_ r: WireRequest) -> VerbOutcome {
    let parsed = ParsedArgs(r.args)
    guard let src = parsed.arg("src", positional: 0),
          let dst = parsed.arg("dst", positional: 1) else {
        return .err(code: "invalid_args", detail: ["message": "file.rename requires <src> <dst>"])
    }
    let overwrite = parsed.flags["overwrite"] != nil
    let crossfs = parsed.flags["cross-fs"] != nil
    return fsResult({
        try FileSystem.rename(src: src, dst: dst,
                              overwrite: overwrite, allowCrossFS: crossfs)
        return ["renamed": true] as [String: Any]
    }, encode: encodeJSON)
}

private func handleFileStat(_ r: WireRequest) -> VerbOutcome {
    guard let path = ParsedArgs(r.args).arg("path") else {
        return .err(code: "invalid_args", detail: ["message": "file.stat requires <path>"])
    }
    return fsResult({ try FileSystem.stat(path) }, encode: { stat in
        (try? JSONSerialization.data(withJSONObject: stat.jsonObject, options: [.sortedKeys])) ?? Data()
    })
}

private func handleFileExists(_ r: WireRequest) -> VerbOutcome {
    guard let path = ParsedArgs(r.args).arg("path") else {
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
    guard let glob = parsed.arg("glob") else {
        return .err(code: "invalid_args", detail: ["message": "file.wait requires <glob>"])
    }
    let interval = parsed.intFlag("interval") ?? 200
    // `--timeout-ms` is the protocol's relative-deadline arg; `--deadline`
    // (absolute epoch-ms) wins if both are given. Mirrors element.wait.
    let timeoutMs = parsed.intFlag("timeout-ms") ?? 10_000
    let now = Int(Date().timeIntervalSince1970 * 1000)
    let deadline = parsed.intFlag("deadline") ?? (now + timeoutMs)
    return fsResult({ try FileSystem.waitForPath(glob, intervalMs: interval, deadlineMs: deadline) }, encode: { stat in
        (try? JSONSerialization.data(withJSONObject: stat.jsonObject, options: [.sortedKeys])) ?? Data()
    })
}

private func handleFileDownload(_ r: WireRequest) -> VerbOutcome {
    // URL + destination: --url / --local-path flags (spec form) or two
    // leading positionals.
    let parsed = ParsedArgs(r.args)
    guard let url = parsed.flags["url"] ?? parsed.positionals.first else {
        return .err(code: "invalid_args", detail: ["message": "file.download requires --url"])
    }
    guard let dest = parsed.flags["local-path"] ?? parsed.positionals.dropFirst().first else {
        return .err(code: "invalid_args", detail: ["message": "file.download requires --local-path"])
    }
    // Only http/https are downloadable — reject other schemes up front
    // rather than failing deep inside URLSession.
    let scheme = URL(string: url)?.scheme?.lowercased()
    guard scheme == "http" || scheme == "https" else {
        return .err(code: "invalid_args", detail: ["message": "file.download supports only http/https URLs"])
    }
    return fsResult({ try FileSystem.download(url: url, destination: dest) }, encode: { stat in
        (try? JSONSerialization.data(withJSONObject: stat.jsonObject, options: [.sortedKeys])) ?? Data()
    })
}

private func handleDirList(_ r: WireRequest) -> VerbOutcome {
    guard let path = ParsedArgs(r.args).arg("path") else {
        return .err(code: "invalid_args", detail: ["message": "directory.list requires <path>"])
    }
    return fsResult({ try FileSystem.listDirectory(path) }, encode: { entries in
        let body: [String: Any] = ["entries": entries.map { $0.jsonObject }]
        return (try? JSONSerialization.data(withJSONObject: body, options: [.sortedKeys])) ?? Data()
    })
}

private func handleDirStat(_ r: WireRequest) -> VerbOutcome {
    guard let path = ParsedArgs(r.args).arg("path") else {
        return .err(code: "invalid_args", detail: ["message": "directory.stat requires <path>"])
    }
    return fsResult({ try FileSystem.directoryStat(path) }, encode: { tuple in
        let body: [String: Any] = ["entries": tuple.count, "mtime": tuple.mtime]
        return (try? JSONSerialization.data(withJSONObject: body, options: [.sortedKeys])) ?? Data()
    })
}

private func handleDirExists(_ r: WireRequest) -> VerbOutcome {
    guard let path = ParsedArgs(r.args).arg("path") else {
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
    guard let path = parsed.arg("path") else {
        return .err(code: "invalid_args", detail: ["message": "directory.create requires <path>"])
    }
    let parents = parsed.flags["parents"] != nil
    return fsResult({
        try FileSystem.createDirectory(path, withParents: parents)
        return ["created": true] as [String: Any]
    }, encode: encodeJSON)
}

private func handleDirRename(_ r: WireRequest) -> VerbOutcome {
    let parsed = ParsedArgs(r.args)
    guard let src = parsed.arg("src", positional: 0),
          let dst = parsed.arg("dst", positional: 1) else {
        return .err(code: "invalid_args", detail: ["message": "directory.rename requires <src> <dst>"])
    }
    let overwrite = parsed.flags["overwrite"] != nil
    let crossfs = parsed.flags["cross-fs"] != nil
    return fsResult({
        try FileSystem.rename(src: src, dst: dst,
                              overwrite: overwrite, allowCrossFS: crossfs)
        return ["renamed": true] as [String: Any]
    }, encode: encodeJSON)
}

private func handleDirRemove(_ r: WireRequest) -> VerbOutcome {
    let parsed = ParsedArgs(r.args)
    guard let path = parsed.arg("path") else {
        return .err(code: "invalid_args", detail: ["message": "directory.remove requires <path>"])
    }
    let recursive = parsed.flags["recursive"] != nil
    let permanent = parsed.flags["permanent"] != nil
    return fsResult({
        try FileSystem.removeDirectory(path, recursive: recursive, permanent: permanent)
    }, encode: encodeDeleteOutcome)
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
    case .notFound:
        // Element table miss = "the elt:N you referenced is no longer
        // valid". The post-rc.2 code for that is target_gone (distinct
        // from not_found which means "AX walk yielded zero matches").
        return .err(code: "target_gone", detail: [:])
    case .noMatch:         return .err(code: "not_found", detail: [:])
    case .readonly(let a): return .err(code: "readonly", detail: ["attribute": a])
    case .actionUnsupported:
        return .err(code: "not_supported_by_target", detail: ["message": "element has no such action / attribute is unsupported"])
    case .timeout:         return .err(code: "timeout", detail: [:])
    case .axError(let m):  return .err(code: "ax_error", detail: ["message": m])
    }
}

private func encodeSnapshots(_ snaps: [Element.Snapshot]) -> Data {
    // Spec output schema wraps the snapshots in an `elements` array
    // (element.list and element.tree).
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
    let role = parsed.flags["role"]            // pass-through; nil = default interactable set
    let limit = parsed.intFlag("limit") ?? 256
    return elementResult(
        { try Element.list(table: table, region: region, role: role, maxResults: limit) },
        encode: encodeSnapshots
    )
}

private func handleElementTree(_ request: WireRequest, table: ElementTable) -> VerbOutcome {
    let parsed = ParsedArgs(request.args)
    guard let id = parsed.arg("handle") else {
        return .err(code: "invalid_args", detail: ["message": "element.tree requires <elt:N>"])
    }
    let maxDepth = parsed.intFlag("max-depth") ?? 16
    return elementResult({ try Element.tree(table: table, idStr: id, maxDepth: maxDepth) }, encode: encodeSnapshots)
}

private func handleElementAt(_ request: WireRequest, table: ElementTable) -> VerbOutcome {
    let parsed = ParsedArgs(request.args)
    guard let pt = parsePoint(parsed) else {
        return .err(code: "invalid_args", detail: ["message": "element.at requires <x> <y>"])
    }
    return elementResult({ try Element.elementAt(table: table, point: pt) }, encode: encodeSnapshot)
}

private func handleElementFind(_ request: WireRequest, table: ElementTable) -> VerbOutcome {
    let parsed = ParsedArgs(request.args)
    // Accept both `<role> <pattern>` positional form and the flag form
    // (--role X --name Y --timeout-ms Z) used by the post-rc.2 conformance.
    let role: String
    let pattern: String
    if let r = parsed.flags["role"], let n = parsed.flags["name"] {
        role = r; pattern = n
    } else if let n = parsed.flags["name"] {
        // --name only: search across any interactable role.
        role = ""; pattern = n
    } else if parsed.positionals.count >= 2 {
        role = parsed.positionals[0]; pattern = parsed.positionals[1]
    } else {
        return .err(code: "invalid_args", detail: ["message": "element.find requires <role> <name> or --name X"])
    }
    return elementResult({
        try Element.find(table: table, role: role, pattern: pattern)
    }, encode: encodeSnapshot)
}

private func handleElementWait(_ request: WireRequest, table: ElementTable) -> VerbOutcome {
    let parsed = ParsedArgs(request.args)
    // Same grammar as element.find with an added --timeout-ms.
    let role: String
    let pattern: String
    if let r = parsed.flags["role"], let n = parsed.flags["name"] {
        role = r; pattern = n
    } else if let n = parsed.flags["name"] {
        role = ""; pattern = n
    } else if parsed.positionals.count >= 2 {
        role = parsed.positionals[0]; pattern = parsed.positionals[1]
    } else {
        return .err(code: "invalid_args", detail: ["message": "element.wait requires <role> <name> or --name X"])
    }

    // --flags-required: comma-separated element states from the universal
    // state enum (#92). Validate every entry. (Gating the wait on those
    // states is a separate, not-yet-implemented feature.)
    let universalStates: Set<String> = ["enabled", "focused", "offscreen", "password", "required"]
    if let raw = parsed.flags["flags-required"] {
        for state in raw.split(separator: ",").map({ $0.trimmingCharacters(in: .whitespaces) })
        where !universalStates.contains(state) {
            return .err(code: "invalid_args", detail: [
                "message": "unknown --flags-required state \"\(state)\"; allowed: \(universalStates.sorted().joined(separator: ", "))",
            ])
        }
    }

    let interval = parsed.intFlag("interval") ?? 100
    let timeoutMs = parsed.intFlag("timeout-ms") ?? 10_000
    let now = Int(Date().timeIntervalSince1970 * 1000)
    let deadline = parsed.intFlag("deadline") ?? (now + timeoutMs)
    return elementResult({
        try Element.wait(table: table, role: role, pattern: pattern,
                         intervalMs: interval, deadlineMs: deadline)
    }, encode: encodeSnapshot)
}

private func handleElementText(_ request: WireRequest, table: ElementTable) -> VerbOutcome {
    guard let id = ParsedArgs(request.args).arg("handle") else {
        return .err(code: "invalid_args", detail: ["message": "element.text requires <elt:N>"])
    }
    return elementResult({ Data(try Element.text(table: table, idStr: id).utf8) }, encode: { $0 })
}

private func handleElementInvoke(_ request: WireRequest, table: ElementTable) -> VerbOutcome {
    guard let id = ParsedArgs(request.args).arg("handle") else {
        return .err(code: "invalid_args", detail: ["message": "element.invoke requires <elt:N>"])
    }
    return elementResult({ try Element.invoke(table: table, idStr: id); return () }, encode: { _ in Data() })
}

private func handleElementToggle(_ request: WireRequest, table: ElementTable) -> VerbOutcome {
    guard let id = ParsedArgs(request.args).arg("handle") else {
        return .err(code: "invalid_args", detail: ["message": "element.toggle requires <elt:N>"])
    }
    return elementResult({ try Element.toggle(table: table, idStr: id) }, encode: { state in
        (try? JSONSerialization.data(withJSONObject: ["state": state], options: [.sortedKeys])) ?? Data()
    })
}

private func handleElementExpand(_ request: WireRequest, table: ElementTable) -> VerbOutcome {
    guard let id = ParsedArgs(request.args).arg("handle") else {
        return .err(code: "invalid_args", detail: ["message": "element.expand requires <elt:N>"])
    }
    return elementResult({ try Element.expand(table: table, idStr: id); return () }, encode: { _ in Data() })
}

private func handleElementCollapse(_ request: WireRequest, table: ElementTable) -> VerbOutcome {
    guard let id = ParsedArgs(request.args).arg("handle") else {
        return .err(code: "invalid_args", detail: ["message": "element.collapse requires <elt:N>"])
    }
    return elementResult({ try Element.collapse(table: table, idStr: id); return () }, encode: { _ in Data() })
}

private func handleElementFocus(_ request: WireRequest, table: ElementTable) -> VerbOutcome {
    guard let id = ParsedArgs(request.args).arg("handle") else {
        return .err(code: "invalid_args", detail: ["message": "element.focus requires <elt:N>"])
    }
    return elementResult({ try Element.focus(table: table, idStr: id); return () }, encode: { _ in Data() })
}

private func handleElementSetText(_ request: WireRequest, table: ElementTable) -> VerbOutcome {
    guard let id = ParsedArgs(request.args).arg("handle") else {
        return .err(code: "invalid_args", detail: ["message": "element.set_text requires <elt:N> <length>"])
    }
    guard let text = String(data: request.payload, encoding: .utf8) else {
        return .err(code: "invalid_args", detail: ["message": "element.set_text payload must be UTF-8"])
    }
    return elementResult({ try Element.setText(table: table, idStr: id, text: text); return () }, encode: { _ in Data() })
}

private func handleElementFindInvoke(_ request: WireRequest, table: ElementTable) -> VerbOutcome {
    let parsed = ParsedArgs(request.args)
    let role: String
    let pattern: String
    if let r = parsed.flags["role"], let n = parsed.flags["name"] {
        role = r; pattern = n
    } else if let n = parsed.flags["name"] {
        role = ""; pattern = n
    } else if parsed.positionals.count >= 2 {
        role = parsed.positionals[0]; pattern = parsed.positionals[1]
    } else {
        return .err(code: "invalid_args", detail: ["message": "element.find_invoke requires <role> <name> or --name X"])
    }
    return elementResult({
        let snap = try Element.find(table: table, role: role, pattern: pattern)
        try Element.invoke(table: table, idStr: snap.id)
        return snap
    }, encode: encodeSnapshot)
}

private func handleElementAtInvoke(_ request: WireRequest, table: ElementTable) -> VerbOutcome {
    let parsed = ParsedArgs(request.args)
    guard let pt = parsePoint(parsed) else {
        return .err(code: "invalid_args", detail: ["message": "element.at_invoke requires <x> <y>"])
    }
    return elementResult({
        let snap = try Element.elementAt(table: table, point: pt)
        try Element.invoke(table: table, idStr: snap.id)
        return snap
    }, encode: encodeSnapshot)
}
