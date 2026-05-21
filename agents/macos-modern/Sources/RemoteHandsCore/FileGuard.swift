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

/// Protected-path policy. Blocks every `file.*` / `directory.*` verb from
/// reading, writing, listing, or even stating paths the agent must not
/// expose. The headline target is the agent's own token file — a read-tier
/// client that could `file.read` it could then `connection.tier_raise` to
/// `extra_risky` and bypass the whole tier ladder. Protection is **absolute
/// across all tiers**: even an `extra_risky`-tier connection cannot touch
/// these paths via the wire.
///
/// Bypass-attempts via symlinks are caught by canonicalising both the input
/// path and each protected prefix through `realpath(3)` before comparison.
/// The raw (tilde-expanded but not realpath'd) form is also checked so a
/// query against a non-existent symlink target still resolves correctly.
public enum FileGuard {

    /// Throw `FSError.protectedPath` if `path` (in any form) names a file
    /// or directory under a protected prefix. Call at the top of every
    /// `FileSystem.*` entry point that accepts a path.
    public static func ensureAllowed(_ path: String) throws {
        if isProtected(path) {
            throw FSError.protectedPath
        }
    }

    /// Returns true iff `path` resolves to (or sits under) a protected
    /// prefix. The same `ensureAllowed` check inverted, exposed for callers
    /// that want to filter rather than fail (e.g. directory listings that
    /// hide protected entries silently).
    public static func isProtected(_ path: String) -> Bool {
        // Compare both the tilde-expanded raw form and the realpath-resolved
        // form so a symlink pointing into a protected location can't
        // exfiltrate via the raw path.
        let raw = (path as NSString).expandingTildeInPath
        let canonical = realpathCanonical(raw)
        for prefix in protectedPrefixes {
            if matches(raw, prefix: prefix) { return true }
            if let c = canonical, matches(c, prefix: prefix) { return true }
        }
        return false
    }

    // MARK: prefix list

    /// Concrete prefixes to deny. Built once at first access; subsequent
    /// `ensureAllowed` calls reuse the same list. Both the raw form and
    /// any realpath-resolved form are included so existence-changes
    /// post-startup don't open holes.
    public static let protectedPrefixes: [String] = {
        let home = NSHomeDirectory()
        let raw: [String] = [
            // Agent's own token file. Per-user store + the system-wide path
            // a future install script might use.
            "\(home)/Library/Application Support/AgentRemoteHands",
            "/Library/Application Support/AgentRemoteHands",
            // SSH credentials.
            "\(home)/.ssh",
            // macOS keychain databases.
            "\(home)/Library/Keychains",
            "/Library/Keychains",
            // System keychain & secrets vault.
            "/private/var/db/SystemKeychain",
        ]
        var set = Set<String>(raw)
        for p in raw {
            if let c = realpathCanonical(p) { set.insert(c) }
        }
        return Array(set)
    }()

    // MARK: helpers

    /// `realpath(3)` wrapper. Returns nil if the path doesn't exist (in
    /// which case there's no symlink to follow and the tilde-expanded form
    /// is authoritative).
    private static func realpathCanonical(_ path: String) -> String? {
        var buf = [CChar](repeating: 0, count: 4096)
        return path.withCString { cstr in
            if realpath(cstr, &buf) != nil {
                return String(cString: buf)
            }
            return nil
        }
    }

    private static func matches(_ candidate: String, prefix: String) -> Bool {
        if candidate == prefix { return true }
        // Block anything strictly under the prefix. A trailing '/' on the
        // prefix avoids /Library/KeychainsFooBar masking as
        // /Library/Keychains.
        return candidate.hasPrefix(prefix + "/")
    }
}
