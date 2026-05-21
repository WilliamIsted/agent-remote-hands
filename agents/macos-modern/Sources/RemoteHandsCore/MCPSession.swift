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

    /// Wrap a successful verb body as an MCP `tools/call` result. The
    /// verb's JSON body is the single text content item (conformance suite
    /// extracts `result.content[0].text` and `json.loads()` it).
    private func mcpContentOK(_ body: Data) -> [String: Any] {
        let text = String(data: body, encoding: .utf8) ?? ""
        return [
            "content": [
                ["type": "text", "text": text] as [String: String],
            ],
            "isError": false,
        ]
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
