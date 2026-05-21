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

/// Per-connection MCP request handler. Routes `initialize` /
/// `notifications/initialized` / `tools/list` / `tools/call` JSON-RPC
/// methods to the existing verb-dispatch infrastructure.
///
/// `tools/call` reconstructs an ARH-style `WireRequest` from the call's
/// arguments + base64 payload, dispatches via `dispatchVerb`, and translates
/// the resulting `VerbOutcome` back into MCP's content-array + isError
/// shape (per the conformance suite's expectations in `tests/conformance/wire.py`).
public final class MCPSession {
    private var initialised = false

    public init() {}

    public enum Action {
        /// Send a JSON object as a single MCP frame.
        case send([String: Any])
        /// Notification frame — no response expected.
        case noResponse
        /// Tier change on the connection in addition to the response.
        case tierChange(Tier, [String: Any])
        /// Close the connection after sending the response.
        case sendThenClose([String: Any])
    }

    /// Handle one incoming MCP frame. Returns the action the connection
    /// should take.
    public func handle(frame: [String: AnyHashable], context: DispatchContext) -> Action {
        guard frame["jsonrpc"] as? String == "2.0" else {
            return .send(JSONRPC.error(id: 0, code: JSONRPC.invalidRequest, message: "missing jsonrpc=2.0"))
        }
        let id: AnyHashable = frame["id"] ?? AnyHashable(0)
        let method = frame["method"] as? String ?? ""

        switch method {
        case "initialize":
            initialised = true
            return .send(JSONRPC.response(id: id, result: [
                "protocolVersion": "2025-03-26",
                "capabilities": [
                    "tools": [String: Any]() as [String: Any],
                ] as [String: Any],
                "serverInfo": [
                    "name": AgentIdentity.name,
                    "version": AgentIdentity.version,
                ] as [String: String],
            ]))
        case "notifications/initialized":
            // No response per JSON-RPC notification semantics.
            return .noResponse
        case "tools/list":
            return .send(JSONRPC.response(id: id, result: ["tools": minimalToolList()]))
        case "tools/call":
            return handleToolsCall(id: id, params: frame["params"] as? [String: Any] ?? [:], context: context)
        default:
            return .send(JSONRPC.error(id: id, code: JSONRPC.methodNotFound, message: "method not found: \(method)"))
        }
    }

    // MARK: tools/list

    /// Minimal tool descriptors. The conformance suite mostly uses
    /// `system.capabilities` for discovery; `tools/list` exists for MCP
    /// protocol compliance. A future slice can enrich each tool with a
    /// real JSON Schema input — for now we emit name + description only.
    private func minimalToolList() -> [[String: Any]] {
        return VerbTable.specs.keys.sorted().map { verb in
            return [
                "name": verb,
                "description": "Agent verb \(verb) (tier=\(VerbTable.specs[verb]?.tier.rawValue ?? "?"))",
                "inputSchema": [
                    "type": "object",
                    "properties": [String: Any]() as [String: Any],
                    "additionalProperties": true,
                ] as [String: Any],
            ]
        }
    }

    // MARK: tools/call

    private func handleToolsCall(id: AnyHashable, params: [String: Any], context: DispatchContext) -> Action {
        guard let verb = params["name"] as? String else {
            return .send(JSONRPC.error(id: id, code: JSONRPC.invalidParams, message: "tools/call: missing name"))
        }
        let args = params["arguments"] as? [String: Any] ?? [:]
        let (flatArgs, payload) = arhFromMCPArguments(args)
        let request = WireRequest(verb: verb, args: flatArgs, payload: payload)

        let outcome = dispatchVerb(request, context: context)

        switch outcome {
        case .ok(let body):
            return .send(JSONRPC.response(id: id, result: mcpContentOK(body)))
        case .okThenClose(let body):
            return .sendThenClose(JSONRPC.response(id: id, result: mcpContentOK(body)))
        case .okWithTierChange(let newTier, let body):
            return .tierChange(newTier, JSONRPC.response(id: id, result: mcpContentOK(body)))
        case .err(let code, let detail):
            return .send(JSONRPC.response(id: id, result: mcpContentERR(code: code, detail: detail)))
        }
    }

    /// Wrap a successful verb body as an MCP `tools/call` result.
    ///
    /// JSON / text verbs produce UTF-8 bodies — emitted as a single text
    /// content item (the conformance suite extracts `result.content[0].text`
    /// and `json.loads()` it). Binary verbs (`screen.capture`) produce image
    /// bytes that are not valid UTF-8; those are emitted as an MCP image
    /// content item with base64 `data`, rather than being lost to a failed
    /// `String(data:encoding:)` conversion.
    private func mcpContentOK(_ body: Data) -> [String: Any] {
        if let text = String(data: body, encoding: .utf8) {
            return [
                "content": [
                    ["type": "text", "text": text] as [String: String],
                ],
                "isError": false,
            ]
        }
        return [
            "content": [
                [
                    "type": "image",
                    "data": body.base64EncodedString(),
                    "mimeType": sniffImageMimeType(body) ?? "application/octet-stream",
                ] as [String: String],
            ],
            "isError": false,
        ]
    }

    /// Sniff an image MIME type from a body's leading magic bytes. Returns
    /// nil if the bytes are not a recognised image format.
    private func sniffImageMimeType(_ data: Data) -> String? {
        let b = [UInt8](data.prefix(12))
        if b.starts(with: [0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]) {
            return "image/png"
        }
        if b.starts(with: [0xFF, 0xD8, 0xFF]) {
            return "image/jpeg"
        }
        if b.starts(with: [0x42, 0x4D]) {
            return "image/bmp"
        }
        // HEIC: "ftyp" box at offset 4, an HEIF/HEIC brand at offset 8.
        if b.count >= 12, b[4] == 0x66, b[5] == 0x74, b[6] == 0x79, b[7] == 0x70 {
            let brand = String(bytes: b[8..<12], encoding: .ascii) ?? ""
            if ["heic", "heix", "hevc", "hevx", "mif1", "msf1"].contains(brand) {
                return "image/heic"
            }
        }
        return nil
    }

    /// Wrap a verb-level ERR as MCP `tools/call` result with `isError:
    /// true`. The original ARH error code is carried in `arh_error_code`
    /// so the conformance suite's `request()` mapping picks it up
    /// unchanged.
    private func mcpContentERR(code: String, detail: [String: String]) -> [String: Any] {
        let detailJSON = (try? JSONSerialization.data(withJSONObject: detail, options: [.sortedKeys])) ?? Data()
        let text = String(data: detailJSON, encoding: .utf8) ?? "{}"
        return [
            "content": [
                ["type": "text", "text": text] as [String: String],
            ],
            "isError": true,
            "arh_error_code": code,
        ]
    }
}
