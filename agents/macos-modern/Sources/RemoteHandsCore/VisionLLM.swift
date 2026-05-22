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

/// Process-wide vision-LLM configuration. Set once during bootstrap in
/// main.swift, read by the vision.describe / vision.calibrate handlers on
/// connection worker threads. Mirrors the PowerPolicy static-policy pattern.
///
/// `nonisolated(unsafe)`: the only write happens in main.swift before the
/// server accepts connections; reads happen on worker threads afterward.
public enum VisionConfig {
    nonisolated(unsafe) public static var defaultEndpoint: String = ""
}

/// Failure modes of a vision-LLM round trip.
public enum VisionLLMError: Error, Equatable {
    case invalidURL(String)
    case badScheme(String)
    case transferFailed(String)
    case httpStatus(Int)
    case timeout
    case badResponse(String)
}

/// A successful vision-LLM round trip.
public struct VisionLLMResult: Sendable, Equatable {
    public let description: String
    public let model: String
    public let tokensIn: Int?
    public let tokensOut: Int?
}

/// OpenAI-compatible vision-LLM client. The endpoint is keyless (a local /
/// LAN server such as LM Studio); no Authorization header — matching
/// windows-modern.
public enum VisionLLM {

    /// Validate and parse an endpoint string. Requires an absolute http(s)
    /// URL with a host.
    public static func validateEndpoint(_ raw: String) -> Result<URL, VisionLLMError> {
        guard let url = URL(string: raw), let scheme = url.scheme, url.host != nil else {
            return .failure(.invalidURL(raw))
        }
        let s = scheme.lowercased()
        guard s == "http" || s == "https" else {
            return .failure(.badScheme(scheme))
        }
        return .success(url)
    }

    /// Build the OpenAI-compatible /v1/chat/completions request body.
    /// `model` is omitted entirely when empty (LM Studio convention).
    public static func buildRequestBody(
        model: String, prompt: String, imageDataURL: String,
        maxTokens: Int, temperature: Double
    ) -> Data {
        let content: [[String: Any]] = [
            ["type": "text", "text": prompt],
            ["type": "image_url", "image_url": ["url": imageDataURL]],
        ]
        var root: [String: Any] = [
            "messages": [["role": "user", "content": content]],
            "max_tokens": maxTokens,
            "temperature": temperature,
        ]
        if !model.isEmpty { root["model"] = model }
        return (try? JSONSerialization.data(withJSONObject: root, options: [.sortedKeys])) ?? Data()
    }

    /// Parse an OpenAI-compatible chat/completions response.
    public static func parseResponse(_ data: Data) -> Result<VisionLLMResult, VisionLLMError> {
        guard let obj = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] else {
            return .failure(.badResponse("response is not a JSON object"))
        }
        guard let choices = obj["choices"] as? [[String: Any]], let first = choices.first,
              let message = first["message"] as? [String: Any],
              let content = message["content"] as? String else {
            return .failure(.badResponse("response missing choices[0].message.content"))
        }
        let model = obj["model"] as? String ?? ""
        var tokensIn: Int? = nil
        var tokensOut: Int? = nil
        if let usage = obj["usage"] as? [String: Any] {
            tokensIn = (usage["prompt_tokens"] as? NSNumber)?.intValue
            tokensOut = (usage["completion_tokens"] as? NSNumber)?.intValue
        }
        return .success(VisionLLMResult(
            description: content, model: model,
            tokensIn: tokensIn, tokensOut: tokensOut))
    }

    /// Detect a supported image format from magic bytes. Returns the format
    /// token ("png"/"jpeg"/"bmp") or nil. Used by vision.calibrate to
    /// reject non-image payloads before any expensive decode.
    public static func detectImageFormat(_ data: Data) -> String? {
        let b = [UInt8](data.prefix(8))
        if b.count >= 8,
           b[0] == 0x89, b[1] == 0x50, b[2] == 0x4E, b[3] == 0x47,
           b[4] == 0x0D, b[5] == 0x0A, b[6] == 0x1A, b[7] == 0x0A {
            return "png"
        }
        if b.count >= 3, b[0] == 0xFF, b[1] == 0xD8, b[2] == 0xFF {
            return "jpeg"
        }
        if b.count >= 2, b[0] == 0x42, b[1] == 0x4D {
            return "bmp"
        }
        return nil
    }

    /// POST `body` to `endpoint` and return the raw response bytes. Runs
    /// URLSession asynchronously but blocks the calling worker thread on a
    /// semaphore — safe because the connection model is per-thread and the
    /// verb dispatcher is synchronous (the same rationale under which
    /// screen.capture uses synchronous CoreGraphics).
    public static func post(
        endpoint: URL, body: Data, timeoutMs: Int
    ) -> Result<Data, VisionLLMError> {
        var req = URLRequest(url: endpoint)
        req.httpMethod = "POST"
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        req.setValue("agent-remote-hands/0.3 (vision; URLSession)",
                     forHTTPHeaderField: "User-Agent")
        req.httpBody = body

        let config = URLSessionConfiguration.ephemeral
        let seconds = Double(timeoutMs) / 1000.0
        config.timeoutIntervalForRequest = seconds
        config.timeoutIntervalForResource = seconds
        let session = URLSession(configuration: config)

        let sem = DispatchSemaphore(value: 0)
        var outData: Data?
        var outResp: URLResponse?
        var outErr: Error?
        let task = session.dataTask(with: req) { d, resp, err in
            outData = d; outResp = resp; outErr = err
            sem.signal()
        }
        task.resume()
        sem.wait()

        if let err = outErr {
            let ns = err as NSError
            if ns.domain == NSURLErrorDomain && ns.code == NSURLErrorTimedOut {
                return .failure(.timeout)
            }
            return .failure(.transferFailed(err.localizedDescription))
        }
        guard let http = outResp as? HTTPURLResponse else {
            return .failure(.transferFailed("no HTTP response"))
        }
        guard (200..<300).contains(http.statusCode) else {
            return .failure(.httpStatus(http.statusCode))
        }
        return .success(outData ?? Data())
    }
}
