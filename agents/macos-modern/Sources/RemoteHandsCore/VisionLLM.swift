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

/// Process-wide vision-LLM configuration. Set once during bootstrap in
/// main.swift, read by the vision.describe / vision.calibrate handlers on
/// connection worker threads. Mirrors the PowerPolicy static-policy pattern.
///
/// `nonisolated(unsafe)`: the only write happens in main.swift before the
/// server accepts connections; reads happen on worker threads afterward.
public enum VisionConfig {
    nonisolated(unsafe) public static var defaultEndpoint: String = ""
}
