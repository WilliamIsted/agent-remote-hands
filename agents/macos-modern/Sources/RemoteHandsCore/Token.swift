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
import Security
#if canImport(Darwin)
import Darwin
#endif

/// Tier-elevation token. Generated at agent startup, written to a file with
/// 0o600 perms, and rotated on each restart. Callers prove authorisation by
/// reading the file (requires filesystem access to the agent host) and
/// quoting the contents in `connection.tier_raise`.
public final class TokenStore: @unchecked Sendable {
    public let value: String
    public let path: String

    /// Generate a fresh 256-bit random token, hex-encode it, write to disk,
    /// and return the live store. Throws on filesystem failure; entropy
    /// failure is treated as fatal (would mean the system CSPRNG is
    /// broken).
    public static func initialise() throws -> TokenStore {
        let token = try generate()
        let path = defaultPath()
        try writeTokenFile(token: token, to: path)
        return TokenStore(value: token, path: path)
    }

    private init(value: String, path: String) {
        self.value = value
        self.path = path
    }

    /// Constant-time string equality — guards against timing side-channels
    /// during token comparison. Both operands must be ASCII (hex strings
    /// only); behaviour on multi-byte UTF-8 differing-length strings is
    /// "always false" via the early length check.
    public func matches(_ candidate: String) -> Bool {
        return constantTimeEqual(value, candidate)
    }

    // MARK: helpers

    public static func defaultPath() -> String {
        let supportDir = ("~/Library/Application Support/AgentRemoteHands" as NSString).expandingTildeInPath
        return (supportDir as NSString).appendingPathComponent("token")
    }

    private static func generate() throws -> String {
        var bytes = [UInt8](repeating: 0, count: 32)
        let status = bytes.withUnsafeMutableBytes { buf -> OSStatus in
            guard let base = buf.baseAddress else { return errSecAllocate }
            return SecRandomCopyBytes(kSecRandomDefault, 32, base)
        }
        guard status == errSecSuccess else {
            throw TokenError.entropyFailure(Int(status))
        }
        return bytes.map { String(format: "%02x", $0) }.joined()
    }

    private static func writeTokenFile(token: String, to path: String) throws {
        let url = URL(fileURLWithPath: path)
        let dir = url.deletingLastPathComponent()
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let data = Data(token.utf8)
        try data.write(to: url, options: [.atomic])
        // chmod after write — `.atomic` does a rename so any prior mode
        // doesn't carry over.
        path.withCString { cstr in
            _ = chmod(cstr, 0o600)
        }
    }
}

public enum TokenError: Error, Equatable {
    case entropyFailure(Int)
}

private func constantTimeEqual(_ a: String, _ b: String) -> Bool {
    let aBytes = Array(a.utf8)
    let bBytes = Array(b.utf8)
    if aBytes.count != bBytes.count { return false }
    var diff: UInt8 = 0
    for i in 0..<aBytes.count {
        diff |= aBytes[i] ^ bBytes[i]
    }
    return diff == 0
}
