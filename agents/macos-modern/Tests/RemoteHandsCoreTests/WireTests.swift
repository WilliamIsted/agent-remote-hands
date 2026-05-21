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

// MARK: tokenizer

@Test func tokenizerSplitsSimpleHeader() throws {
    let tokens = try tokenizeWireHeader("connection.hello smoke-test 2")
    #expect(tokens == ["connection.hello", "smoke-test", "2"])
}

@Test func tokenizerHandlesQuotedToken() throws {
    let tokens = try tokenizeWireHeader(#"directory.create "C:\Program Files\demo dir""#)
    #expect(tokens == ["directory.create", #"C:\Program Files\demo dir"#])
}

@Test func tokenizerHandlesEmptyQuotedToken() throws {
    let tokens = try tokenizeWireHeader(#"window.find """#)
    #expect(tokens == ["window.find", ""])
}

@Test func tokenizerThrowsOnUnmatchedQuote() {
    #expect(throws: WireParseError.unmatchedQuote) {
        try tokenizeWireHeader(#"window.find "unterminated"#)
    }
}

@Test func tokenizerTreatsBackslashLiterally() throws {
    // PROTOCOL.md §1.2.5: backslashes inside an unquoted token are literal —
    // matters for Windows-style paths.
    let tokens = try tokenizeWireHeader(#"directory.create C:\Temp\demo"#)
    #expect(tokens == ["directory.create", #"C:\Temp\demo"#])
}

@Test func tokenizerHandlesEmptyInput() throws {
    let tokens = try tokenizeWireHeader("")
    #expect(tokens == [])
}

// MARK: header parser

@Test func parseHeaderExtractsVerbAndArgs() throws {
    let (verb, args) = try parseWireHeader("system.info")
    #expect(verb == "system.info")
    #expect(args == [])
}

@Test func parseHeaderWithArgs() throws {
    let (verb, args) = try parseWireHeader("connection.hello smoke 2")
    #expect(verb == "connection.hello")
    #expect(args == ["smoke", "2"])
}

// MARK: response formatting

@Test func formatOKEmpty() {
    #expect(formatOK() == Data("OK 0\n".utf8))
}

@Test func formatOKWithPayload() {
    let body = Data(#"{"ok":true}"#.utf8)
    let out = formatOK(payload: body)
    #expect(out == Data("OK 11\n{\"ok\":true}".utf8))
}

@Test func formatERREmpty() {
    #expect(formatERR(code: "not_found") == Data("ERR not_found 0\n".utf8))
}

@Test func formatERRWithDetail() {
    let out = formatERR(code: "tier_required", detail: ["required": "update", "current": "read"])
    let s = String(decoding: out, as: UTF8.self)
    #expect(s.hasPrefix("ERR tier_required "))
    #expect(s.contains("\"current\":\"read\""))
    #expect(s.contains("\"required\":\"update\""))
}

// MARK: tier ladder

@Test func tierSubsumption() {
    #expect(Tier.update.subsumes(.read))
    #expect(Tier.update.subsumes(.create))
    #expect(Tier.update.subsumes(.update))
    #expect(!Tier.read.subsumes(.update))
    #expect(Tier.extraRisky.subsumes(.delete))
}
