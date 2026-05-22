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

@Test func rangeValueRegisteredAtReadTier() {
    let spec = VerbTable.specs["element.range_value"]
    #expect(spec != nil)
    #expect(spec?.tier == .read)
}

@Test func rangeValueLookupOfBogusHandleThrowsInvalidId() {
    let table = ElementTable()
    #expect(throws: ElementError.invalidId("not-an-elt")) {
        _ = try Element.rangeValue(table: table, idStr: "not-an-elt")
    }
}
