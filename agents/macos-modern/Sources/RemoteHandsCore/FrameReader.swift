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

/// A pull-mode frame reader for the v2.0/v2.1 ARH wire format.
///
/// Callers feed raw socket bytes in via `append`, then drain complete
/// frames via repeated `nextFrame()` calls until it returns `.incomplete`.
/// The reader handles:
///
/// - Locating the `\n` header terminator (tolerating optional `\r`).
/// - Enforcing the 65 535-byte header cap per PROTOCOL.md §1.2.
/// - Tokenising the header into `(verb, args)` with quote handling.
/// - Reading the trailing length-prefixed payload for verbs that consume
///   one — discovered via the `payloadLookup` closure injected at init.
/// - Returning `.incomplete` when the payload hasn't fully arrived yet,
///   *without* consuming the header — so the next read can complete the
///   frame.
///
/// State is held on the reader so connection code never has to manage the
/// buffer manually.
public final class FrameReader {
    private var buffer = Data()
    private let payloadLookup: @Sendable (String) -> Bool

    public enum FrameResult: Equatable {
        case incomplete
        case headerTooLong
        case parseError(code: String, detail: [String: String])
        case ok(WireRequest)

        public static func == (lhs: FrameResult, rhs: FrameResult) -> Bool {
            switch (lhs, rhs) {
            case (.incomplete, .incomplete), (.headerTooLong, .headerTooLong):
                return true
            case (.parseError(let ac, let ad), .parseError(let bc, let bd)):
                return ac == bc && ad == bd
            case (.ok(let a), .ok(let b)):
                return a.verb == b.verb && a.args == b.args && a.payload == b.payload
            default:
                return false
            }
        }
    }

    /// `payloadLookup(verb)` returns true if this verb consumes a
    /// length-prefixed payload (where the length is the verb's final arg).
    /// For unknown verbs, return false — the dispatcher will produce
    /// `not_supported_by_target` and the client gets a clean error without
    /// the connection going out of sync.
    public init(payloadLookup: @escaping @Sendable (String) -> Bool) {
        self.payloadLookup = payloadLookup
    }

    public func append(_ bytes: Data) {
        buffer.append(bytes)
    }

    public func append(_ bytes: [UInt8]) {
        buffer.append(contentsOf: bytes)
    }

    /// Drain the next complete frame, or report what's needed to progress.
    public func nextFrame() -> FrameResult {
        guard let nlIdx = buffer.firstIndex(of: 0x0A) else {
            return .incomplete
        }
        let nlOffset = buffer.distance(from: buffer.startIndex, to: nlIdx)
        if nlOffset > wireHeaderMaxBytes {
            buffer.removeSubrange(0...nlIdx)
            return .headerTooLong
        }

        var headerEnd = nlOffset
        if headerEnd > 0 && buffer[headerEnd - 1] == 0x0D { headerEnd -= 1 }
        let lineStr = String(decoding: buffer[0..<headerEnd], as: UTF8.self)

        let verb: String, args: [String]
        do {
            (verb, args) = try parseWireHeader(lineStr)
        } catch let err as WireParseError {
            buffer.removeSubrange(0...nlIdx)
            return .parseError(code: errorCodeForWireParse(err), detail: [:])
        } catch {
            buffer.removeSubrange(0...nlIdx)
            return .parseError(code: "invalid_args", detail: [:])
        }

        // Empty header (just a "\n") is a no-op — consume and try again.
        if verb.isEmpty {
            buffer.removeSubrange(0...nlIdx)
            return .parseError(code: "invalid_args", detail: ["message": "empty header line"])
        }

        let payloadStart = nlOffset + 1
        var payloadEnd = payloadStart
        var payload = Data()

        if payloadLookup(verb) {
            guard let lengthStr = args.last,
                  let length = Int(lengthStr),
                  length >= 0 else {
                buffer.removeSubrange(0...nlIdx)
                return .parseError(
                    code: "invalid_args",
                    detail: ["message": "verb expects non-negative integer length as final arg"]
                )
            }
            payloadEnd = payloadStart + length
            if buffer.count < payloadEnd {
                // The full payload hasn't arrived yet. Crucially we leave
                // the header bytes in the buffer so the next `nextFrame()`
                // call after more bytes arrive can resume from the same
                // header without re-parsing redundantly. Implementation
                // detail: we DO re-parse the header, but it's cheap and
                // avoids a second piece of state.
                return .incomplete
            }
            payload = buffer.subdata(in: payloadStart..<payloadEnd)
        }

        buffer.removeSubrange(0..<payloadEnd)
        return .ok(WireRequest(verb: verb, args: args, payload: payload))
    }

    /// Number of bytes currently buffered awaiting framing. Exposed for
    /// tests + diagnostics.
    public var bufferedByteCount: Int { buffer.count }
}

/// Map `WireParseError` cases to the wire-level error code the agent should
/// surface to the client.
public func errorCodeForWireParse(_ err: WireParseError) -> String {
    switch err {
    case .unmatchedQuote, .emptyHeader, .invalidLength: return "invalid_args"
    case .headerTooLong: return "header_too_long"
    }
}
