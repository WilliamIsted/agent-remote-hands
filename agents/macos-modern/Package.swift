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
        .macOS(.v13),
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
