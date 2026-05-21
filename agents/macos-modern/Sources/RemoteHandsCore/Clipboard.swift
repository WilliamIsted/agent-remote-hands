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
import AppKit

public enum ClipboardError: Error, Equatable {
    case empty
    case formatUnavailable
}

/// `clipboard.*` implementations. NSPasteboard works without a running
/// NSApplication for basic string get/set, so the CLI tool can use it
/// directly. The agent does need to live in the user's Aqua session
/// (launchd LaunchAgent, not LaunchDaemon) for the pasteboard to be the
/// user's — which is the same constraint we already document for the
/// service mode.
public enum Clipboard {

    /// Read the pasteboard's text content as UTF-8 bytes.
    /// Returns empty payload if the pasteboard has a text type set to an
    /// empty string; throws `ClipboardError.empty` if there is no text
    /// type at all.
    public static func getText() throws -> Data {
        let pb = NSPasteboard.general
        guard let s = pb.string(forType: .string) else {
            // Distinguish "no text type" from "empty string" — both are
            // valid pasteboard states but useful to keep separate on the
            // wire so callers can decide what to do.
            if pb.types?.isEmpty ?? true {
                throw ClipboardError.empty
            }
            throw ClipboardError.formatUnavailable
        }
        return Data(s.utf8)
    }

    /// Replace the pasteboard's text content with the supplied UTF-8 bytes.
    public static func setText(_ payload: Data) throws {
        // We don't fail on invalid UTF-8 — NSString init from non-UTF8 data
        // returns nil and we then set an empty string, which is what most
        // callers want over a hard error. Senders are expected to send
        // UTF-8; the protocol's payload is opaque so this is a forgiveness
        // choice rather than a correctness one.
        let s = String(data: payload, encoding: .utf8) ?? ""
        let pb = NSPasteboard.general
        pb.clearContents()
        pb.setString(s, forType: .string)
    }
}
