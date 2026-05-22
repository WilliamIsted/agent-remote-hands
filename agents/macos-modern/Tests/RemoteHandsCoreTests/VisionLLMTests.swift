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
import Testing
@testable import RemoteHandsCore

@Test func validateEndpointRejectsNonURL() {
    let r = VisionLLM.validateEndpoint("not-a-url")
    guard case .failure(.invalidURL) = r else {
        Issue.record("expected .invalidURL, got \(r)")
        return
    }
}

@Test func validateEndpointRejectsNonHTTPScheme() {
    let r = VisionLLM.validateEndpoint("ftp://example.test/x")
    guard case .failure(.badScheme) = r else {
        Issue.record("expected .badScheme, got \(r)")
        return
    }
}

@Test func validateEndpointAcceptsHTTP() {
    let r = VisionLLM.validateEndpoint("http://192.0.2.1:1234/v1/chat/completions")
    guard case .success = r else {
        Issue.record("expected .success, got \(r)")
        return
    }
}

@Test func buildRequestBodyOmitsEmptyModel() throws {
    let data = VisionLLM.buildRequestBody(
        model: "", prompt: "hi", imageDataURL: "data:image/png;base64,AAAA",
        maxTokens: 256, temperature: 0.2)
    let obj = try JSONSerialization.jsonObject(with: data) as! [String: Any]
    #expect(obj["model"] == nil)
    #expect(obj["max_tokens"] as? Int == 256)
    let messages = obj["messages"] as! [[String: Any]]
    let content = messages[0]["content"] as! [[String: Any]]
    #expect(content[0]["type"] as? String == "text")
    #expect(content[1]["type"] as? String == "image_url")
}

@Test func buildRequestBodyIncludesModelWhenSet() throws {
    let data = VisionLLM.buildRequestBody(
        model: "qwen2-vl", prompt: "hi", imageDataURL: "data:image/png;base64,AAAA",
        maxTokens: 256, temperature: 0.2)
    let obj = try JSONSerialization.jsonObject(with: data) as! [String: Any]
    #expect(obj["model"] as? String == "qwen2-vl")
}

@Test func parseResponseExtractsContentAndUsage() {
    let json = """
    {"model":"m","choices":[{"message":{"content":"a description"}}],
     "usage":{"prompt_tokens":11,"completion_tokens":7}}
    """
    let r = VisionLLM.parseResponse(Data(json.utf8))
    guard case .success(let result) = r else {
        Issue.record("expected .success, got \(r)")
        return
    }
    #expect(result.description == "a description")
    #expect(result.model == "m")
    #expect(result.tokensIn == 11)
    #expect(result.tokensOut == 7)
}

@Test func parseResponseRejectsMissingChoices() {
    let r = VisionLLM.parseResponse(Data(#"{"model":"m"}"#.utf8))
    guard case .failure(.badResponse) = r else {
        Issue.record("expected .badResponse, got \(r)")
        return
    }
}

@Test func parseResponseRejectsNonJSON() {
    let r = VisionLLM.parseResponse(Data("<html>".utf8))
    guard case .failure(.badResponse) = r else {
        Issue.record("expected .badResponse, got \(r)")
        return
    }
}
