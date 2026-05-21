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
import Foundation
@testable import RemoteHandsCore

private func makeReader(payloadVerbs: Set<String> = []) -> FrameReader {
    FrameReader { payloadVerbs.contains($0) }
}

private func dataFromString(_ s: String) -> Data { Data(s.utf8) }

// MARK: empty / incomplete

@Test func framerEmptyBufferReturnsIncomplete() {
    let r = makeReader()
    if case .incomplete = r.nextFrame() {} else { Issue.record("expected .incomplete") }
}

@Test func framerHeaderWithoutNewlineReturnsIncomplete() {
    let r = makeReader()
    r.append(dataFromString("system.info"))
    if case .incomplete = r.nextFrame() {} else { Issue.record("expected .incomplete") }
}

// MARK: single frames, no payload

@Test func framerSingleSimpleHeader() throws {
    let r = makeReader()
    r.append(dataFromString("system.info\n"))
    let result = r.nextFrame()
    guard case .ok(let req) = result else {
        Issue.record("expected .ok, got \(result)")
        return
    }
    #expect(req.verb == "system.info")
    #expect(req.args == [])
    #expect(req.payload.isEmpty)
}

@Test func framerHeaderWithArgs() throws {
    let r = makeReader()
    r.append(dataFromString("connection.hello smoke 2\n"))
    let result = r.nextFrame()
    guard case .ok(let req) = result else {
        Issue.record("expected .ok")
        return
    }
    #expect(req.verb == "connection.hello")
    #expect(req.args == ["smoke", "2"])
}

@Test func framerToleratesCRLF() throws {
    let r = makeReader()
    r.append(dataFromString("system.health\r\n"))
    let result = r.nextFrame()
    guard case .ok(let req) = result else {
        Issue.record("expected .ok")
        return
    }
    #expect(req.verb == "system.health")
}

@Test func framerDrainsMultipleBackToBackFrames() {
    let r = makeReader()
    r.append(dataFromString("system.info\nsystem.health\nconnection.close\n"))
    var verbs: [String] = []
    while true {
        switch r.nextFrame() {
        case .ok(let req): verbs.append(req.verb)
        case .incomplete: break
        default: Issue.record("unexpected frame result"); return
        }
        if r.bufferedByteCount == 0 { break }
    }
    #expect(verbs == ["system.info", "system.health", "connection.close"])
}

// MARK: payload-bearing frames

@Test func framerReadsFullPayload() throws {
    let r = makeReader(payloadVerbs: ["clipboard.set"])
    r.append(dataFromString("clipboard.set 5\nhello"))
    let result = r.nextFrame()
    guard case .ok(let req) = result else {
        Issue.record("expected .ok, got \(result)")
        return
    }
    #expect(req.verb == "clipboard.set")
    #expect(req.args == ["5"])
    #expect(req.payload == Data("hello".utf8))
}

@Test func framerWaitsForPartialPayload() {
    let r = makeReader(payloadVerbs: ["clipboard.set"])
    r.append(dataFromString("clipboard.set 5\nhe"))
    // Only 2 of 5 payload bytes arrived — must defer.
    if case .incomplete = r.nextFrame() {} else { Issue.record("expected .incomplete (partial payload)") }

    // Buffer must NOT have been consumed.
    r.append(dataFromString("llo"))
    let second = r.nextFrame()
    guard case .ok(let req) = second else {
        Issue.record("expected .ok after completing payload, got \(second)")
        return
    }
    #expect(req.payload == Data("hello".utf8))
}

@Test func framerHandlesZeroLengthPayload() throws {
    let r = makeReader(payloadVerbs: ["clipboard.set"])
    r.append(dataFromString("clipboard.set 0\n"))
    let result = r.nextFrame()
    guard case .ok(let req) = result else {
        Issue.record("expected .ok")
        return
    }
    #expect(req.payload.isEmpty)
}

@Test func framerBinaryPayloadSurvivesRoundTrip() throws {
    let r = makeReader(payloadVerbs: ["file.write"])
    var bytes = Data("file.write /tmp/x 8\n".utf8)
    bytes.append(contentsOf: [0x00, 0xFF, 0x01, 0xFE, 0x7F, 0x80, 0x0A, 0x0D])
    r.append(bytes)
    let result = r.nextFrame()
    guard case .ok(let req) = result else {
        Issue.record("expected .ok")
        return
    }
    #expect(req.payload == Data([0x00, 0xFF, 0x01, 0xFE, 0x7F, 0x80, 0x0A, 0x0D]))
}

@Test func framerRejectsNonNumericLength() {
    let r = makeReader(payloadVerbs: ["clipboard.set"])
    r.append(dataFromString("clipboard.set notanumber\n"))
    let result = r.nextFrame()
    guard case .parseError(let code, _) = result else {
        Issue.record("expected .parseError, got \(result)")
        return
    }
    #expect(code == "invalid_args")
}

@Test func framerRejectsNegativeLength() {
    let r = makeReader(payloadVerbs: ["clipboard.set"])
    r.append(dataFromString("clipboard.set -1\n"))
    let result = r.nextFrame()
    guard case .parseError(let code, _) = result else {
        Issue.record("expected .parseError"); return
    }
    #expect(code == "invalid_args")
}

// MARK: bytewise arrival

@Test func framerHandlesBytewiseArrival() throws {
    let r = makeReader(payloadVerbs: ["clipboard.set"])
    let full = dataFromString("clipboard.set 5\nhello")
    var emittedVerb: String?
    var emittedPayload: Data?
    for byte in full {
        r.append([byte])
        switch r.nextFrame() {
        case .ok(let req):
            emittedVerb = req.verb
            emittedPayload = req.payload
        case .incomplete:
            continue
        default:
            Issue.record("unexpected frame result during bytewise feed")
            return
        }
    }
    #expect(emittedVerb == "clipboard.set")
    #expect(emittedPayload == Data("hello".utf8))
}

// MARK: bad headers

@Test func framerEmptyHeaderLineReportsError() {
    let r = makeReader()
    r.append(dataFromString("\n"))
    if case .parseError(let code, _) = r.nextFrame() {
        #expect(code == "invalid_args")
    } else {
        Issue.record("expected .parseError on empty header")
    }
}

@Test func framerOversizedHeader() {
    let r = makeReader()
    // wireHeaderMaxBytes is 65 535; build one byte past it followed by \n.
    var huge = String(repeating: "a", count: wireHeaderMaxBytes + 1)
    huge.append("\n")
    r.append(dataFromString(huge))
    if case .headerTooLong = r.nextFrame() {} else {
        Issue.record("expected .headerTooLong")
    }
}
