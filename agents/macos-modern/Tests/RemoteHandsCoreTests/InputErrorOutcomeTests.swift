//
// Copyright 2026 William Isted and contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//

import Testing
@testable import RemoteHandsCore

@Test func inputPermissionDeniedReportsAccessibility() {
    // Synthetic CGEvent posting on macOS is gated by Accessibility, not
    // Input Monitoring. The permission_denied error must point the
    // operator at the Accessibility pane — pointing at Input Monitoring
    // (where the agent never appears) is a dead end.
    let outcome = inputErrorOutcome(.permissionDenied)
    guard case .err(let code, let detail) = outcome else {
        Issue.record("expected .err, got \(outcome)")
        return
    }
    #expect(code == "permission_denied")
    #expect(detail["category"] == "accessibility")
    #expect(detail["hint"]?.contains("Accessibility") == true)
    #expect(detail["hint"]?.contains("Input Monitoring") != true)
    #expect(detail["hint"]?.contains("restart") == true)
}

@Test func inputUnknownButtonReportsInvalidArgs() {
    guard case .err(let code, _) = inputErrorOutcome(.unknownButton("x")) else {
        Issue.record("expected .err")
        return
    }
    #expect(code == "invalid_args")
}
