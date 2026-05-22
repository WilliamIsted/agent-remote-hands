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

public enum SearchMatchMode: String, Sendable {
    case substring
    case regex
}

public enum ElementSearchError: Error, Equatable {
    case invalidRegex(String)
}

/// One match within the searched text buffer. Offsets are NSString
/// (UTF-16) code-unit offsets into the buffer — the unit NSRegularExpression
/// and NSString range operations work in, kept consistent end to end.
public struct RawMatch: Sendable, Equatable {
    public let patternIndex: Int
    public let matchStart: Int
    public let matchLength: Int
    public let excerpt: String
}

/// Pure multi-pattern text search. Used by element.search after the AX
/// subtree's text has been flattened into a single buffer.
public enum ElementSearch {

    public static func findMatches(
        in buffer: String,
        patterns: [String],
        mode: SearchMatchMode,
        caseSensitive: Bool,
        contextChars: Int,
        maxHitsPerPattern: Int
    ) throws -> (matches: [RawMatch], unmatched: [String]) {
        let ns = buffer as NSString
        var matches: [RawMatch] = []
        var unmatched: [String] = []

        for (idx, pattern) in patterns.enumerated() {
            let ranges = try rangesFor(pattern: pattern, in: ns, mode: mode,
                                       caseSensitive: caseSensitive,
                                       cap: maxHitsPerPattern)
            if ranges.isEmpty {
                unmatched.append(pattern)
                continue
            }
            for range in ranges {
                let exStart = max(0, range.location - contextChars)
                let exEnd = min(ns.length, range.location + range.length + contextChars)
                let excerpt = ns.substring(with: NSRange(location: exStart, length: exEnd - exStart))
                matches.append(RawMatch(
                    patternIndex: idx,
                    matchStart: range.location,
                    matchLength: range.length,
                    excerpt: excerpt))
            }
        }
        return (matches, unmatched)
    }

    private static func rangesFor(
        pattern: String, in ns: NSString, mode: SearchMatchMode,
        caseSensitive: Bool, cap: Int
    ) throws -> [NSRange] {
        var out: [NSRange] = []
        switch mode {
        case .substring:
            var options: NSString.CompareOptions = []
            if !caseSensitive { options.insert(.caseInsensitive) }
            var searchFrom = 0
            while out.count < cap && searchFrom < ns.length {
                let scope = NSRange(location: searchFrom, length: ns.length - searchFrom)
                let r = ns.range(of: pattern, options: options, range: scope)
                if r.location == NSNotFound { break }
                out.append(r)
                searchFrom = r.location + max(1, r.length)
            }
        case .regex:
            var options: NSRegularExpression.Options = []
            if !caseSensitive { options.insert(.caseInsensitive) }
            let regex: NSRegularExpression
            do {
                regex = try NSRegularExpression(pattern: pattern, options: options)
            } catch {
                throw ElementSearchError.invalidRegex(pattern)
            }
            let whole = NSRange(location: 0, length: ns.length)
            for m in regex.matches(in: ns as String, options: [], range: whole) {
                if out.count >= cap { break }
                if m.range.length > 0 { out.append(m.range) }
            }
        }
        return out
    }
}
