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

@Test func visionDescribeRegisteredAtUpdateTier() {
    let spec = VerbTable.specs["vision.describe"]
    #expect(spec != nil)
    #expect(spec?.tier == .update)
}

@Test func visionCalibrateRegisteredAtUpdateTier() {
    let spec = VerbTable.specs["vision.calibrate"]
    #expect(spec != nil)
    #expect(spec?.tier == .update)
}
