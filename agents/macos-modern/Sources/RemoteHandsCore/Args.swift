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

/// Verb argument parser for the `[--key value | --flag]` style used by every
/// verb in PROTOCOL.md §4. Positional arguments (e.g. `file.write <path>
/// <length>`) are kept in `positionals` in source order; flags consume the
/// following token as their value unless the next token also begins with
/// `--`, in which case the flag is treated as a boolean and stored with an
/// empty value.
///
/// This parser is intentionally permissive — it accepts any flag name and
/// does not validate against a per-verb schema. Verb handlers validate
/// their own flags and return `ERR invalid_args` on bad input.
public struct ParsedArgs: Sendable {
    public let positionals: [String]
    public let flags: [String: String]

    public init(_ args: [String]) {
        var positionals: [String] = []
        var flags: [String: String] = [:]
        var i = 0
        while i < args.count {
            let tok = args[i]
            if tok.hasPrefix("--") {
                let key = String(tok.dropFirst(2))
                // --key=value form
                if let eq = key.firstIndex(of: "=") {
                    let k = String(key[..<eq])
                    let v = String(key[key.index(after: eq)...])
                    flags[k] = v
                    i += 1
                    continue
                }
                // --key value form (peek next)
                if i + 1 < args.count && !args[i + 1].hasPrefix("--") {
                    flags[key] = args[i + 1]
                    i += 2
                } else {
                    flags[key] = ""  // boolean flag
                    i += 1
                }
            } else {
                positionals.append(tok)
                i += 1
            }
        }
        self.positionals = positionals
        self.flags = flags
    }

    /// Look up an integer flag. Returns nil if the flag is missing or
    /// non-numeric. Callers decide how to map nil to a default vs. an error.
    public func intFlag(_ name: String) -> Int? {
        guard let raw = flags[name] else { return nil }
        return Int(raw)
    }
}
