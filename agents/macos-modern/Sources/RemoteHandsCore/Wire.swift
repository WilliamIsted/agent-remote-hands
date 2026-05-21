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

// MARK: - Tier ladder (CRUDX)

/// Strict CRUDX ladder. Holding a higher tier subsumes all lower ones.
public enum Tier: String, CaseIterable, Sendable {
    case read
    case create
    case update
    case delete
    case extraRisky = "extra_risky"

    public var rank: Int {
        switch self {
        case .read:        return 0
        case .create:      return 1
        case .update:      return 2
        case .delete:      return 3
        case .extraRisky:  return 4
        }
    }

    public func subsumes(_ other: Tier) -> Bool { rank >= other.rank }
}

// MARK: - Wire framing per PROTOCOL.md §1.2

/// Parsed wire request: a directive (the verb), positional arguments, and an
/// opaque payload. The payload is empty when the verb's grammar carries no
/// final `<length>` argument.
public struct WireRequest: Sendable {
    public let verb: String
    public let args: [String]
    public let payload: Data
}

/// Maximum permitted header line length per PROTOCOL.md §1.2 (excludes the
/// trailing `\n`).
public let wireHeaderMaxBytes = 65_535

/// Parse errors at the framing layer. Mapped to `ERR …` responses by the
/// connection-level dispatch.
public enum WireParseError: Error, Equatable {
    case unmatchedQuote
    case headerTooLong
    case emptyHeader
    case invalidLength(String)
}

/// Tokenise an ASCII header line per PROTOCOL.md §1.2.5.
///
/// - Outside any token, `"` begins a quoted token; non-space starts an
///   unquoted token; space is a separator.
/// - Inside an unquoted token, `"` is taken literally (callers MUST NOT send
///   embedded quotes, but receivers tolerate them for robustness).
/// - Inside a quoted token, the next `"` closes it; there is no escape.
///
/// The line MUST NOT contain `\r` or `\n` — callers strip the terminator
/// before passing in.
public func tokenizeWireHeader(_ line: String) throws -> [String] {
    var tokens: [String] = []
    var current = ""
    var inQuote = false
    var tokenStart = true

    for ch in line {
        if inQuote {
            if ch == "\"" {
                tokens.append(current)
                current = ""
                inQuote = false
                tokenStart = true
            } else {
                current.append(ch)
            }
        } else if tokenStart && ch == "\"" {
            inQuote = true
            tokenStart = false
        } else if ch == " " {
            if !current.isEmpty {
                tokens.append(current)
                current = ""
            }
            tokenStart = true
        } else {
            current.append(ch)
            tokenStart = false
        }
    }
    if inQuote {
        throw WireParseError.unmatchedQuote
    }
    if !current.isEmpty {
        tokens.append(current)
    }
    return tokens
}

/// Parse a tokenised header into `(verb, args, declaredPayloadLength)`.
///
/// If the verb's grammar has a final `<length>` argument, the parser cannot
/// know that by inspection — it is part of the per-verb grammar. The
/// connection layer decides whether to consume a payload by looking at the
/// verb table. This function does NOT consume any trailing payload.
public func parseWireHeader(_ line: String) throws -> (verb: String, args: [String]) {
    let tokens = try tokenizeWireHeader(line)
    guard let verb = tokens.first else {
        throw WireParseError.emptyHeader
    }
    return (verb, Array(tokens.dropFirst()))
}

// MARK: - Response formatting

/// Format a successful response. `OK <length>\n<payload>`.
public func formatOK(payload: Data = Data()) -> Data {
    var out = Data()
    let header = "OK \(payload.count)\n"
    out.append(header.data(using: .utf8)!)
    out.append(payload)
    return out
}

/// Format an error response. `ERR <code> <length>\n<detail-json>`.
public func formatERR(code: String, detail: [String: Any] = [:]) -> Data {
    var out = Data()
    let detailData: Data
    if detail.isEmpty {
        detailData = Data()
    } else {
        detailData = (try? JSONSerialization.data(withJSONObject: detail, options: [.sortedKeys])) ?? Data()
    }
    let header = "ERR \(code) \(detailData.count)\n"
    out.append(header.data(using: .utf8)!)
    out.append(detailData)
    return out
}
