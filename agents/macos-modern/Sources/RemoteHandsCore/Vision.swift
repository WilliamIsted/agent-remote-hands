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
import CoreGraphics
import ImageIO
import Vision

public enum VisionError: Error, Equatable {
    case decodeFailed(String)
    case ocrFailed(String)
}

public struct OCRObservation: Sendable {
    public let text: String
    public let confidence: Double
    /// Bounding box in absolute image-pixel coordinates, origin top-left.
    public let bounds: CGRect

    public var jsonObject: [String: Any] {
        [
            "text": text,
            "confidence": confidence,
            "bounds": [
                "x": Int(bounds.origin.x), "y": Int(bounds.origin.y),
                "w": Int(bounds.size.width), "h": Int(bounds.size.height),
            ] as [String: Int],
        ]
    }
}

/// The full result of an OCR pass: the recognised text regions plus the
/// pixel size of the image they were found in and the language Vision
/// was asked to use.
public struct OCRResult: Sendable {
    public let observations: [OCRObservation]
    public let imageSize: CGSize
    public let languageUsed: String
}

public enum VisionOps {

    /// OCR a decoded image. `language` is an optional BCP-47 tag; when
    /// supplied it is set as Vision's preferred recognition language.
    /// Bounding boxes in the result are in image-pixel coordinates with a
    /// top-left origin.
    public static func ocr(cgImage cg: CGImage, language: String?, accurate: Bool = true) throws -> OCRResult {
        let imageWidth = CGFloat(cg.width)
        let imageHeight = CGFloat(cg.height)

        let request = VNRecognizeTextRequest()
        request.recognitionLevel = accurate ? .accurate : .fast
        if #available(macOS 14, *) {
            // Sonoma+ ships revision 3, with substantially better accuracy.
            request.revision = VNRecognizeTextRequestRevision3
        }
        if let language = language, !language.isEmpty {
            request.recognitionLanguages = [language]
        }

        let handler = VNImageRequestHandler(cgImage: cg, options: [:])
        do {
            try handler.perform([request])
        } catch {
            throw VisionError.ocrFailed("\(error)")
        }

        let observations: [OCRObservation] = (request.results ?? []).compactMap { obs in
            guard let top = obs.topCandidates(1).first else { return nil }
            // Vision returns boundingBox normalised (0-1) with origin
            // bottom-left. Convert to absolute pixel coords with origin
            // top-left, matching CG / screen-capture conventions.
            let b = obs.boundingBox
            let pxX = b.origin.x * imageWidth
            let pxY = (1.0 - b.origin.y - b.size.height) * imageHeight
            let pxW = b.size.width * imageWidth
            let pxH = b.size.height * imageHeight
            return OCRObservation(
                text: top.string,
                confidence: Double(top.confidence),
                bounds: CGRect(x: pxX, y: pxY, width: pxW, height: pxH)
            )
        }
        return OCRResult(
            observations: observations,
            imageSize: CGSize(width: imageWidth, height: imageHeight),
            languageUsed: (language?.isEmpty == false) ? language! : "en-US"
        )
    }

    /// The recognition languages the on-device OCR engine supports. Used by
    /// vision.calibrate to report engine capability. Returns [] on failure.
    public static func supportedRecognitionLanguages() -> [String] {
        // VNRecognizeTextRequest.supportedRecognitionLanguages(for:revision:)
        // is available on macOS 10.15+. The instance method form
        // (.supportedRecognitionLanguages()) requires macOS 13; use the class
        // method to stay within the macOS 12.3 deployment target.
        return (try? VNRecognizeTextRequest.supportedRecognitionLanguages(
            for: .accurate, revision: VNRecognizeTextRequestRevision2)) ?? []
    }

    /// OCR encoded image bytes (any format ImageIO recognises: PNG, JPEG,
    /// HEIC, BMP, TIFF, GIF, …). Decodes, then delegates to the CGImage
    /// path.
    public static func ocr(imageData: Data, language: String?, accurate: Bool = true) throws -> OCRResult {
        guard let src = CGImageSourceCreateWithData(imageData as CFData, nil) else {
            throw VisionError.decodeFailed("CGImageSourceCreateWithData returned nil")
        }
        guard let cg = CGImageSourceCreateImageAtIndex(src, 0, nil) else {
            throw VisionError.decodeFailed("CGImageSourceCreateImageAtIndex returned nil")
        }
        return try ocr(cgImage: cg, language: language, accurate: accurate)
    }
}
