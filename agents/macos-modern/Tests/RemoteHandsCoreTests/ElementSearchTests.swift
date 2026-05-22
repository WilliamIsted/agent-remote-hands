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

@Test func searchSubstringFindsSingleHit() throws {
    let r = try ElementSearch.findMatches(
        in: "the quick brown fox", patterns: ["quick"],
        mode: .substring, caseSensitive: false, contextChars: 3, maxHitsPerPattern: 5)
    #expect(r.matches.count == 1)
    #expect(r.matches[0].patternIndex == 0)
    #expect(r.matches[0].matchStart == 4)
    #expect(r.matches[0].matchLength == 5)
    #expect(r.unmatched.isEmpty)
}

@Test func searchSubstringIsCaseInsensitiveByDefault() throws {
    let r = try ElementSearch.findMatches(
        in: "Hello WORLD", patterns: ["world"],
        mode: .substring, caseSensitive: false, contextChars: 0, maxHitsPerPattern: 5)
    #expect(r.matches.count == 1)
}

@Test func searchSubstringCaseSensitiveExcludesMismatch() throws {
    let r = try ElementSearch.findMatches(
        in: "Hello WORLD", patterns: ["world"],
        mode: .substring, caseSensitive: true, contextChars: 0, maxHitsPerPattern: 5)
    #expect(r.matches.isEmpty)
    #expect(r.unmatched == ["world"])
}

@Test func searchHonoursMaxHitsPerPattern() throws {
    let r = try ElementSearch.findMatches(
        in: "a a a a a", patterns: ["a"],
        mode: .substring, caseSensitive: false, contextChars: 0, maxHitsPerPattern: 2)
    #expect(r.matches.count == 2)
}

@Test func searchExcerptWindowsAroundMatch() throws {
    let r = try ElementSearch.findMatches(
        in: "0123456789abcXYZdef", patterns: ["XYZ"],
        mode: .substring, caseSensitive: false, contextChars: 3, maxHitsPerPattern: 5)
    #expect(r.matches.count == 1)
    #expect(r.matches[0].excerpt == "abcXYZdef")
}

@Test func searchRegexMode() throws {
    let r = try ElementSearch.findMatches(
        in: "order 4271 shipped", patterns: ["[0-9]+"],
        mode: .regex, caseSensitive: false, contextChars: 0, maxHitsPerPattern: 5)
    #expect(r.matches.count == 1)
    #expect(r.matches[0].matchLength == 4)
}

@Test func searchInvalidRegexThrows() {
    #expect(throws: ElementSearchError.invalidRegex("[unterminated")) {
        _ = try ElementSearch.findMatches(
            in: "anything", patterns: ["[unterminated"],
            mode: .regex, caseSensitive: false, contextChars: 0, maxHitsPerPattern: 5)
    }
}

@Test func searchReportsUnmatchedPatterns() throws {
    let r = try ElementSearch.findMatches(
        in: "the quick brown fox", patterns: ["quick", "zebra"],
        mode: .substring, caseSensitive: false, contextChars: 0, maxHitsPerPattern: 5)
    #expect(r.matches.count == 1)
    #expect(r.unmatched == ["zebra"])
}
