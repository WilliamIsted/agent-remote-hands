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
#if canImport(Darwin)
import Darwin
#endif

/// Build-time identity for the agent. Kept here rather than baked into a
/// dozen call sites so a single rebuild stamps version + protocol cleanly.
public enum AgentIdentity {
    public static let name = "agent-remote-hands"
    public static let version = "0.3.0-mvp"
    public static let protocolVersion = "2.2"
    public static let osFamily = "macos-modern"
}

/// Build the `system.info` response body. Returned shape matches PROTOCOL.md
/// §3.1. Fields that don't apply to the macOS family (`integrity`,
/// `uiaccess`, `privileges`) are populated with macOS-appropriate values or
/// `null`.
public func systemInfoBody(currentTier: Tier, implementedNamespaces: [String], implementedVerbs: [String]) -> [String: Any] {
    let proc = ProcessInfo.processInfo
    let host = Host.current().localizedName ?? proc.hostName
    let user = NSUserName()
    let arch = currentArchString()

    return [
        "name": AgentIdentity.name,
        "version": AgentIdentity.version,
        "protocol": AgentIdentity.protocolVersion,
        "os": AgentIdentity.osFamily,
        "arch": arch,
        "hostname": host,
        "user": user,
        // macOS has no Windows-style mandatory integrity control. Report null
        // so callers can distinguish "doesn't apply" from "unknown".
        "integrity": NSNull(),
        "uiaccess": false,
        "monitors": monitorCount(),
        "privileges": [String](),
        "tiers": Tier.allCases.map { $0.rawValue },
        "current_tier": currentTier.rawValue,
        "auth": ["token"],
        "max_connections": 4,
        "namespaces": implementedNamespaces,
        "capabilities": [
            "capture": "screencapturekit",
            "ui_automation": "none",  // AX lands with element.*
            "image_formats": CaptureFormat.allCases.map { $0.rawValue },
            "discovery": "none",      // Bonjour lands with mDNS module
            "implemented_verbs": implementedVerbs.sorted(),
            "tcc": [
                "screen_recording": ScreenCapture.hasScreenRecordingPermission() ? "granted" : "denied",
                "accessibility": "unknown",      // probed once AX verbs land
                "input_monitoring": "unknown",   // probed once input.* lands
            ] as [String: String],
        ] as [String: Any],
    ]
}

private func currentArchString() -> String {
    #if arch(arm64)
    return "arm64"
    #elseif arch(x86_64)
    return "x86_64"
    #else
    return "unknown"
    #endif
}

private func monitorCount() -> Int {
    // The MVP slice does not link AppKit, so we cannot query NSScreen here
    // without bloating the binary. Capture-related verbs will pull this from
    // ScreenCaptureKit when they land. Until then report 0 — capture-gated
    // clients should consult `capabilities.capture` instead.
    return 0
}
