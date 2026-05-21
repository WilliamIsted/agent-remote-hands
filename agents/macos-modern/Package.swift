// swift-tools-version:5.9
//
// Copyright 2026 William Isted and contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//

import PackageDescription

let package = Package(
    name: "remote-hands",
    platforms: [
        // Deployment floor: Monterey 12.3 — the deliberate target from
        // Agent/Planning/agent-rebuild/macos-modern/floor-tradeoffs.md
        // (Big Sur 11 was considered and rejected there).
        //
        // No single API hard-requires 12.3: screen.capture uses the
        // synchronous CGDisplayCreateImage CoreGraphics path (see
        // ScreenCapture.swift), so ScreenCaptureKit does not gate the
        // floor. The hard technical floor is Swift concurrency, native at
        // macOS 12.0 (no back-deployment libraries are bundled). 12.3 is
        // retained as the conservative documented family target; the
        // string form pins the point release (.v12 maps to 12.0).
        .macOS("12.3"),
    ],
    products: [
        .library(name: "RemoteHandsCore", targets: ["RemoteHandsCore"]),
        .executable(name: "rha-mac", targets: ["rha-mac"]),
    ],
    targets: [
        .target(
            name: "RemoteHandsCore",
            path: "Sources/RemoteHandsCore"
        ),
        .executableTarget(
            name: "rha-mac",
            dependencies: ["RemoteHandsCore"],
            path: "Sources/rha-mac"
        ),
        .testTarget(
            name: "RemoteHandsCoreTests",
            dependencies: ["RemoteHandsCore"],
            path: "Tests/RemoteHandsCoreTests"
        ),
    ]
)
