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

/// Build the `system.info` response body. Field names match the post-rc.2
/// schema used by the v2.2 conformance suite — `family`/`agent`/
/// `agent_protocol`/`os_name`+`os_version`/`cpu_arch`/`screens`/`framings`
/// — rather than the older PROTOCOL.md §3.1 names (`os`/`name`/`protocol`/
/// `arch`/`monitors`).
public func systemInfoBody(currentTier: Tier, implementedNamespaces: [String], implementedVerbs: [String]) -> [String: Any] {
    let proc = ProcessInfo.processInfo
    let host = Host.current().localizedName ?? proc.hostName
    let user = NSUserName()

    return [
        "family": AgentIdentity.osFamily,
        "agent": AgentIdentity.name,
        "agent_version": AgentIdentity.version,
        "agent_protocol": AgentIdentity.protocolVersion,
        "os_name": "macOS",
        "os_version": proc.operatingSystemVersionString,
        "cpu_arch": currentArchString(),
        "hostname": host,
        "user": user,
        // macOS has no Windows-style mandatory integrity control. The
        // conformance enum permits "none".
        "integrity": "none",
        "uiaccess": false,
        "screens": screens(),
        "privileges": [String](),
        "tiers": Tier.allCases.map { $0.rawValue },
        "current_tier": currentTier.rawValue,
        "auth": ["token"],
        "max_connections": 4,
        "namespaces": implementedNamespaces,
        // Supported framings post-hello. WebSocket isn't yet implemented.
        "framings": ["mcp"],
        "capabilities": [
            "capture": "screencapturekit",
            "ui_automation": "ax",
            "image_formats": CaptureFormat.allCases.map { $0.rawValue },
            "discovery": "bonjour",
            "implemented_verbs": implementedVerbs.sorted(),
            "tcc": [
                "screen_recording": ScreenCapture.hasScreenRecordingPermission() ? "granted" : "denied",
                "accessibility": Window.hasAccessibilityPermission() ? "granted" : "denied",
                "input_monitoring": Input.hasInputMonitoringPermission() ? "granted" : "denied",
            ] as [String: String],
        ] as [String: Any],
    ]
}

private func currentArchString() -> String {
    #if arch(arm64)
    return "arm64"
    #elseif arch(x86_64)
    return "x64"
    #else
    return "unknown"
    #endif
}

private func screens() -> [[String: Any]] {
    return NSScreen.screens.enumerated().map { idx, screen in
        let f = screen.frame
        return [
            "index": idx,
            "bounds": [
                "x": Int(f.origin.x),
                "y": Int(f.origin.y),
                "w": Int(f.size.width),
                "h": Int(f.size.height),
            ] as [String: Int],
            "scale": Double(screen.backingScaleFactor),
        ]
    }
}
