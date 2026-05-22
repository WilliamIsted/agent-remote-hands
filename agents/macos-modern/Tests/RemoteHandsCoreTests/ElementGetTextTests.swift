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

@Test func sliceTextWholeStringWhenUnderLimit() {
    let r = Element.sliceText("hello", offset: 0, maxLength: 100)
    #expect(r.text == "hello")
    #expect(r.length == 5)
    #expect(r.truncated == false)
    #expect(r.offset == 0)
}

@Test func sliceTextTruncatesAtMaxLength() {
    let r = Element.sliceText("hello world", offset: 0, maxLength: 5)
    #expect(r.text == "hello")
    #expect(r.length == 11)
    #expect(r.truncated == true)
    #expect(r.offset == 0)
}

@Test func sliceTextHonoursOffset() {
    let r = Element.sliceText("hello world", offset: 6, maxLength: 100)
    #expect(r.text == "world")
    #expect(r.length == 11)
    #expect(r.truncated == false)
    #expect(r.offset == 6)
}

@Test func sliceTextOffsetPastEndClampsToEmpty() {
    let r = Element.sliceText("hello", offset: 99, maxLength: 100)
    #expect(r.text == "")
    #expect(r.length == 5)
    #expect(r.truncated == false)
    #expect(r.offset == 5)
}

@Test func sliceTextOffsetPlusLimitMidString() {
    let r = Element.sliceText("0123456789", offset: 3, maxLength: 4)
    #expect(r.text == "3456")
    #expect(r.length == 10)
    #expect(r.truncated == true)
    #expect(r.offset == 3)
}
