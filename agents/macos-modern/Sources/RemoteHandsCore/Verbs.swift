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

/// The information the dispatcher needs about each verb: required tier and
/// whether it's valid only before / only after hello.
public struct VerbSpec: Sendable {
    public let tier: Tier
    /// True for verbs callable before `connection.hello` (only hello and
    /// close qualify). All other verbs require state == connected.
    public let preHelloOK: Bool
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
    ]

    public static let implementedNamespaces: [String] = ["connection", "system", "screen"]
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
