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

/// MCP-stdio framing per the v2.2 spec — after `connection.hello`, every
/// message on the wire is `Content-Length: N\r\n\r\n<N JSON-RPC 2.0 bytes>`.
public enum MCPFrame {

    /// Encode a JSON object as one MCP-stdio frame.
    public static func encode(_ obj: [String: Any]) -> Data {
        let body = (try? JSONSerialization.data(withJSONObject: obj, options: [.sortedKeys])) ?? Data("{}".utf8)
        var out = Data()
        out.append(Data("Content-Length: \(body.count)\r\n\r\n".utf8))
        out.append(body)
        return out
    }
}

/// Pull-mode reader for MCP-stdio frames. Mirrors `FrameReader`'s
/// incremental contract: caller appends bytes, calls `nextFrame()` until it
/// returns `.incomplete`.
public final class MCPFrameReader {
    private var buffer = Data()

    public enum Result: Equatable {
        case incomplete
        case parseError(String)
        case ok([String: AnyHashable])
    }

    public init() {}

    public func append(_ bytes: Data) { buffer.append(bytes) }
    public func append(_ bytes: [UInt8]) { buffer.append(contentsOf: bytes) }
    public var bufferedByteCount: Int { buffer.count }

    /// Returns the next complete frame as a parsed JSON object, or
    /// `.incomplete` if more bytes are needed.
    public func nextFrame() -> Result {
        // Look for the header/body separator (CRLF CRLF).
        guard let split = findHeaderEnd() else { return .incomplete }
        let headerBytes = buffer[0..<split]
        let headerStr = String(decoding: headerBytes, as: UTF8.self)
        var contentLength: Int? = nil
        for line in headerStr.components(separatedBy: "\r\n") {
            guard !line.isEmpty else { continue }
            let parts = line.split(separator: ":", maxSplits: 1, omittingEmptySubsequences: false)
            guard parts.count == 2 else { continue }
            let key = parts[0].trimmingCharacters(in: .whitespaces).lowercased()
            let val = parts[1].trimmingCharacters(in: .whitespaces)
            if key == "content-length" {
                contentLength = Int(val)
            }
        }
        guard let length = contentLength else {
            buffer.removeSubrange(0..<(split + 4))
            return .parseError("missing Content-Length header")
        }
        let bodyStart = split + 4
        let bodyEnd = bodyStart + length
        if buffer.count < bodyEnd { return .incomplete }
        let bodyBytes = buffer[bodyStart..<bodyEnd]
        buffer.removeSubrange(0..<bodyEnd)
        do {
            let any = try JSONSerialization.jsonObject(with: bodyBytes)
            guard let obj = any as? [String: AnyHashable] else {
                return .parseError("MCP body is not a JSON object")
            }
            return .ok(obj)
        } catch {
            return .parseError("MCP body parse failed: \(error)")
        }
    }

    private func findHeaderEnd() -> Int? {
        // Look for \r\n\r\n
        let needle: [UInt8] = [0x0D, 0x0A, 0x0D, 0x0A]
        guard buffer.count >= needle.count else { return nil }
        for i in 0...(buffer.count - needle.count) {
            if buffer[i] == needle[0] && buffer[i+1] == needle[1] &&
               buffer[i+2] == needle[2] && buffer[i+3] == needle[3] {
                return i
            }
        }
        return nil
    }
}

// MARK: - JSON-RPC 2.0 helpers

public enum JSONRPC {

    /// Build a JSON-RPC 2.0 response object for a request `id`.
    public static func response(id: AnyHashable, result: [String: Any]) -> [String: Any] {
        return ["jsonrpc": "2.0", "id": id, "result": result]
    }

    /// Build a JSON-RPC 2.0 error response for a request `id`.
    public static func error(id: AnyHashable, code: Int, message: String, data: [String: Any]? = nil) -> [String: Any] {
        var err: [String: Any] = ["code": code, "message": message]
        if let data = data { err["data"] = data }
        return ["jsonrpc": "2.0", "id": id, "error": err]
    }

    // Standard JSON-RPC error codes.
    public static let parseError = -32700
    public static let invalidRequest = -32600
    public static let methodNotFound = -32601
    public static let invalidParams = -32602
    public static let internalError = -32603
}

// MARK: - ARH ↔ MCP `tools/call` translation

/// Convert MCP `tools/call.arguments` shape back into the ARH (verb, args,
/// payload) tuple our verb dispatcher already understands. Inverse of the
/// conformance suite's `_args_to_dict` in `tests/conformance/wire.py`.
public func arhFromMCPArguments(_ args: [String: Any]) -> (positionalsAndFlags: [String], payload: Data) {
    var out: [String] = []
    // Positionals come from `_args` if present.
    if let posList = args["_args"] as? [String] {
        out.append(contentsOf: posList)
    }
    // Payload bytes come from `content_b64` (base64-encoded).
    var payload = Data()
    if let b64 = args["content_b64"] as? String, let bytes = Data(base64Encoded: b64) {
        payload = bytes
    }
    // All other keys are --key value pairs. Boolean true → just the flag.
    for (key, value) in args {
        if key == "_args" || key == "content_b64" { continue }
        if let b = value as? Bool, b {
            out.append("--\(key)")
        } else if let s = value as? String {
            out.append("--\(key)")
            out.append(s)
        } else {
            out.append("--\(key)")
            out.append(String(describing: value))
        }
    }
    // Some payload-bearing verbs expect a `<length>` positional at the end
    // of the ARH args. The dispatcher reads that via `args.last`; supply it
    // here when a payload exists and no explicit `_args` length was given.
    if !payload.isEmpty {
        // Append the length as a positional so verbs that grep `args.last`
        // (file.write etc.) keep working.
        out.append("\(payload.count)")
    }
    return (out, payload)
}
