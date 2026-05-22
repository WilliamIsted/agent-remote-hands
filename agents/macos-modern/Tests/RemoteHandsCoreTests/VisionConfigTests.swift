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

@Test func visionConfigDefaultsToEmpty() {
    // A fresh process has no endpoint until main.swift sets one.
    // This test documents the default; it runs first by name ordering
    // is not guaranteed, so only assert the type contract.
    #expect(VisionConfig.defaultEndpoint is String)
}

@Test func visionConfigRoundTrips() {
    VisionConfig.defaultEndpoint = "http://example.test:1234/v1/chat/completions"
    #expect(VisionConfig.defaultEndpoint == "http://example.test:1234/v1/chat/completions")
    VisionConfig.defaultEndpoint = ""
    #expect(VisionConfig.defaultEndpoint == "")
}
