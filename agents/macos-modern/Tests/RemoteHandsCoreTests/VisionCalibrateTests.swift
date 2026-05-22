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
import Testing
@testable import RemoteHandsCore

@Test func detectImageFormatRecognisesPNG() {
    let png = Data([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00])
    #expect(VisionLLM.detectImageFormat(png) == "png")
}

@Test func detectImageFormatRecognisesJPEG() {
    let jpeg = Data([0xFF, 0xD8, 0xFF, 0xE0, 0x00])
    #expect(VisionLLM.detectImageFormat(jpeg) == "jpeg")
}

@Test func detectImageFormatRecognisesBMP() {
    let bmp = Data([0x42, 0x4D, 0x00, 0x00])
    #expect(VisionLLM.detectImageFormat(bmp) == "bmp")
}

@Test func detectImageFormatRejectsGarbage() {
    let garbage = Data(repeating: 0x5A, count: 32)
    #expect(VisionLLM.detectImageFormat(garbage) == nil)
}

@Test func detectImageFormatRejectsTooShort() {
    #expect(VisionLLM.detectImageFormat(Data([0x42])) == nil)
}
