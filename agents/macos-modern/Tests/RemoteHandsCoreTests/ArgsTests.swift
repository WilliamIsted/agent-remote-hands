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

@Test func argsEmpty() {
    let p = ParsedArgs([])
    #expect(p.positionals == [])
    #expect(p.flags.isEmpty)
}

@Test func argsSinglePositional() {
    let p = ParsedArgs(["/path/to/file"])
    #expect(p.positionals == ["/path/to/file"])
    #expect(p.flags.isEmpty)
}

@Test func argsKeyValuePair() {
    let p = ParsedArgs(["--format", "png"])
    #expect(p.flags["format"] == "png")
    #expect(p.positionals == [])
}

@Test func argsMultipleFlags() {
    let p = ParsedArgs(["--format", "jpeg", "--quality", "90"])
    #expect(p.flags["format"] == "jpeg")
    #expect(p.flags["quality"] == "90")
}

@Test func argsEqualsForm() {
    let p = ParsedArgs(["--format=heic", "--quality=80"])
    #expect(p.flags["format"] == "heic")
    #expect(p.flags["quality"] == "80")
}

@Test func argsBooleanFlag() {
    let p = ParsedArgs(["--force"])
    #expect(p.flags["force"] == "")
}

@Test func argsBooleanThenKeyValue() {
    let p = ParsedArgs(["--force", "--reason", "user-requested"])
    #expect(p.flags["force"] == "")
    #expect(p.flags["reason"] == "user-requested")
}

@Test func argsMixedPositionalAndFlagsGreedyConsumes() {
    // The parser has no per-verb schema, so it greedily consumes the next
    // non-`--` token as the flag's value. For `directory.rename src
    // --overwrite dst`, callers MUST send `--overwrite` last (or use the
    // `--key=value` form) to avoid `dst` being absorbed as overwrite's
    // value. This is documented in `Args.swift` and re-tested here so the
    // behaviour doesn't regress silently.
    let p = ParsedArgs(["src.txt", "--overwrite", "dst.txt"])
    #expect(p.positionals == ["src.txt"])
    #expect(p.flags["overwrite"] == "dst.txt")
}

@Test func argsMixedWithExplicitBooleanForm() {
    // Workaround for the limitation above: senders that want a boolean
    // followed by a positional can use the `--key=` (empty value) form, or
    // place the boolean flag last in the arg list.
    let p = ParsedArgs(["src.txt", "dst.txt", "--overwrite"])
    #expect(p.positionals == ["src.txt", "dst.txt"])
    #expect(p.flags["overwrite"] == "")
}

@Test func argsIntFlag() {
    let p = ParsedArgs(["--quality", "75"])
    #expect(p.intFlag("quality") == 75)
    #expect(p.intFlag("missing") == nil)
}

@Test func argsIntFlagNonNumeric() {
    let p = ParsedArgs(["--quality", "not-a-number"])
    #expect(p.intFlag("quality") == nil)
}

@Test func captureFormatRawValues() {
    // Guards against the enum's rawValues drifting from the protocol enum.
    #expect(CaptureFormat.png.rawValue == "png")
    #expect(CaptureFormat.jpeg.rawValue == "jpeg")
    #expect(CaptureFormat.heic.rawValue == "heic")
    #expect(CaptureFormat.bmp.rawValue == "bmp")
}
