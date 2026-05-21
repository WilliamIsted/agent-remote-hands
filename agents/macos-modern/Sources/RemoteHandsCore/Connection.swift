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
#if canImport(Darwin)
import Darwin
#endif

/// Per-connection state per PROTOCOL.md §2.1.
public enum ConnectionState: Sendable {
    case preHello
    case connected
    case closed
}

/// One connection — owns the socket, drives the read loop, and routes verbs
/// through `dispatchVerb`. Designed to run on a single dispatch queue so
/// state mutations need no locking.
public final class ConnectionSession {
    private let fd: Int32
    private var state: ConnectionState = .preHello
    private var tier: Tier = .read
    private var buffer = Data()
    private let label: String
    private let logger: (String) -> Void

    public init(fd: Int32, label: String, logger: @escaping (String) -> Void) {
        self.fd = fd
        self.label = label
        self.logger = logger
    }

    /// Run the read loop until the socket closes or the peer sends
    /// `connection.close`. Blocks the current thread — call from a dispatch
    /// queue dedicated to this connection.
    public func run() {
        logger("[\(label)] connection opened, tier=\(tier.rawValue) state=preHello")
        defer {
            close(fd)
            state = .closed
            logger("[\(label)] connection closed")
        }

        var readBuf = [UInt8](repeating: 0, count: 8192)
        while state != .closed {
            let n = readBuf.withUnsafeMutableBufferPointer { ptr in
                Darwin.read(fd, ptr.baseAddress, ptr.count)
            }
            if n <= 0 {
                // 0 = peer EOF; <0 = error. Either way: end.
                return
            }
            buffer.append(contentsOf: readBuf[0..<n])

            while tryProcessFrame() {
                if state == .closed { return }
            }
        }
    }

    /// Try to extract one frame from the buffer. Returns true if a frame was
    /// processed (caller should loop), false if more bytes are needed.
    private func tryProcessFrame() -> Bool {
        // Find the terminating \n.
        guard let nlIdx = buffer.firstIndex(of: 0x0A) else { return false }

        if nlIdx > wireHeaderMaxBytes {
            sendErr(code: "header_too_long")
            // Drop buffer up through the \n so we resync.
            buffer.removeSubrange(0...nlIdx)
            return true
        }

        // Slice off the header (without \n, and without optional trailing \r).
        var headerEnd = nlIdx
        if headerEnd > 0 && buffer[headerEnd - 1] == 0x0D { headerEnd -= 1 }
        let headerBytes = buffer[0..<headerEnd]
        let lineStr = String(decoding: headerBytes, as: UTF8.self)

        let verb: String
        let args: [String]
        do {
            (verb, args) = try parseWireHeader(lineStr)
        } catch let err as WireParseError {
            sendErr(code: errorCode(for: err))
            buffer.removeSubrange(0...nlIdx)
            return true
        } catch {
            sendErr(code: "invalid_args")
            buffer.removeSubrange(0...nlIdx)
            return true
        }

        // Decide whether to consume a trailing payload. The MVP slice has no
        // verbs with a payload argument; `clipboard.set` / `file.write` etc.
        // will set their last-arg-is-length convention when they land.
        let payload = Data()  // no MVP verb consumes a payload yet

        // Consume the framed bytes from the buffer.
        buffer.removeSubrange(0...nlIdx)

        let request = WireRequest(verb: verb, args: args, payload: payload)
        process(request: request)
        return true
    }

    private func process(request: WireRequest) {
        // Pre-hello gate per PROTOCOL.md §2.1.
        if state == .preHello {
            guard let spec = VerbTable.specs[request.verb], spec.preHelloOK else {
                sendErr(code: "invalid_state", detail: ["required": "hello"])
                return
            }
        }

        let outcome = dispatchVerb(request, currentTier: tier)
        switch outcome {
        case .ok(let payload):
            sendOK(payload: payload)
            // Hello transitions to connected on success.
            if request.verb == "connection.hello" && state == .preHello {
                state = .connected
                logger("[\(label)] hello accepted, state=connected tier=\(tier.rawValue)")
            }

        case .okThenClose(let payload):
            sendOK(payload: payload)
            state = .closed

        case .okWithTierChange(let newTier, let payload):
            sendOK(payload: payload)
            tier = newTier
            logger("[\(label)] tier changed to \(newTier.rawValue)")

        case .err(let code, let detail):
            sendErr(code: code, detail: detail)
        }
    }

    private func sendOK(payload: Data) {
        send(formatOK(payload: payload))
    }

    private func sendErr(code: String, detail: [String: String] = [:]) {
        var anyDetail: [String: Any] = [:]
        for (k, v) in detail { anyDetail[k] = v }
        send(formatERR(code: code, detail: anyDetail))
    }

    private func send(_ data: Data) {
        data.withUnsafeBytes { raw in
            guard let base = raw.baseAddress else { return }
            var remaining = data.count
            var p = base
            while remaining > 0 {
                let n = Darwin.write(fd, p, remaining)
                if n <= 0 {
                    // Peer dropped or error — caller will detect on next read.
                    return
                }
                p = p.advanced(by: n)
                remaining -= n
            }
        }
    }

    private func errorCode(for err: WireParseError) -> String {
        switch err {
        case .unmatchedQuote:   return "invalid_args"
        case .headerTooLong:    return "header_too_long"
        case .emptyHeader:      return "invalid_args"
        case .invalidLength:    return "invalid_args"
        }
    }
}
