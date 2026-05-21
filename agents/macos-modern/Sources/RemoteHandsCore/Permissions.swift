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
import ApplicationServices
import IOKit
import IOKit.hid

/// Startup TCC consent prompting.
///
/// Every per-verb TCC check in the agent uses the *non-prompting* API
/// variant (`CGPreflightScreenCaptureAccess`,
/// `AXIsProcessTrustedWithOptions(nil)`, `IOHIDCheckAccess`) so verbs
/// return clean `permission_denied` / `ax_disabled` wire errors instead
/// of blocking on a modal dialog. That leaves first-run setup
/// undiscoverable — an operator learns the requirement only via wire
/// errors.
///
/// `requestAll` is called once at startup and uses the *requesting* API
/// variants so macOS shows the consent dialogs. macOS renders a dialog
/// only when the permission is undetermined, so the prompts appear at
/// most once — on genuine first run.
public enum Permissions {

    /// Trigger the macOS consent dialogs for the three TCC categories the
    /// agent uses: Screen Recording, Accessibility, Input Monitoring.
    ///
    /// Non-blocking: each call shows its dialog asynchronously and returns
    /// the current grant status immediately. The agent always continues
    /// startup regardless of outcome.
    public static func requestAll(logger: (String) -> Void) {
        logger("requesting TCC permissions (Screen Recording, Accessibility, Input Monitoring)")

        let screenRecording = CGRequestScreenCaptureAccess()
        logger("  Screen Recording: \(statusLine(screenRecording))")

        let axOptions = [kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: true] as CFDictionary
        let accessibility = AXIsProcessTrustedWithOptions(axOptions)
        logger("  Accessibility: \(statusLine(accessibility))")

        let inputMonitoring = IOHIDRequestAccess(kIOHIDRequestTypePostEvent)
        logger("  Input Monitoring: \(statusLine(inputMonitoring))")
    }

    /// Format a grant boolean for the startup log. A `false` is ambiguous
    /// — it can mean "a dialog is showing and the user has not yet
    /// decided" or "the user previously denied" (the API cannot
    /// distinguish the two) — so the message states both possibilities.
    private static func statusLine(_ granted: Bool) -> String {
        granted
            ? "granted"
            : "not granted — a prompt may be showing; if previously denied, grant in System Settings → Privacy & Security and restart the agent"
    }
}
