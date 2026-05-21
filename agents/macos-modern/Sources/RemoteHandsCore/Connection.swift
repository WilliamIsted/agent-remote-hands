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
public final class ConnectionSession: @unchecked Sendable {
    private let fd: Int32
    private var state: ConnectionState = .preHello
    private var tier: Tier = .read
    private let frameReader: FrameReader
    private let elementTable = ElementTable()
    private let subscriptions = SubscriptionRegistry()
    private let tokenStore: TokenStore?
    /// Serialises ALL writes to fd — OK/ERR replies from the read loop and
    /// EVENT frames from subscription queues both go through `send()` which
    /// takes this lock.
    private let sendLock = NSLock()
    private let label: String
    private let logger: (String) -> Void

    public init(fd: Int32, label: String, tokenStore: TokenStore?, logger: @escaping (String) -> Void) {
        self.fd = fd
        self.label = label
        self.tokenStore = tokenStore
        self.logger = logger
        self.frameReader = FrameReader { verb in
            VerbTable.specs[verb]?.consumesPayload ?? false
        }
    }

    /// Run the read loop until the socket closes or the peer sends
    /// `connection.close`. Blocks the current thread — call from a dispatch
    /// queue dedicated to this connection.
    public func run() {
        logger("[\(label)] connection opened, tier=\(tier.rawValue) state=preHello")
        defer {
            // Cancel all subscriptions before closing the socket — pending
            // EVENT writes from subscription queues need to find the fd
            // still valid (or at least not have it closed mid-write).
            subscriptions.cancelAll()
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
            frameReader.append(Array(readBuf[0..<n]))

            // Drain as many complete frames as the buffer holds. `nextFrame`
            // returns `.incomplete` when more bytes are needed; we stop and
            // wait for the next read.
            drainLoop: while true {
                let result = frameReader.nextFrame()
                switch result {
                case .incomplete:
                    break drainLoop
                case .headerTooLong:
                    sendErr(code: "header_too_long")
                case .parseError(let code, let detail):
                    sendErr(code: code, detail: detail)
                case .ok(let request):
                    process(request: request)
                    if state == .closed { return }
                }
            }
        }
    }

    private func process(request: WireRequest) {
        // Pre-hello gate per PROTOCOL.md §2.1.
        if state == .preHello {
            guard let spec = VerbTable.specs[request.verb], spec.preHelloOK else {
                sendErr(code: "invalid_state", detail: ["required": "hello"])
                return
            }
        }

        let context = DispatchContext(
            currentTier: tier,
            elementTable: elementTable,
            subscriptions: subscriptions,
            sendEvent: { [weak self] subID, payload in
                self?.send(formatEvent(subID: subID, payload: payload))
            },
            tokenStore: tokenStore
        )
        let outcome = dispatchVerb(request, context: context)
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
        sendLock.lock(); defer { sendLock.unlock() }
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
}
