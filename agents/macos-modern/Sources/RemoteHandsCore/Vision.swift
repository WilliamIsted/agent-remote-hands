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

public enum VisionOps {

    /// Run OCR over the supplied image bytes (any format ImageIO recognises:
    /// PNG, JPEG, HEIC, BMP, TIFF, GIF, etc.). Returns each text region
    /// found with its confidence score and pixel bounds.
    public static func ocr(imageData: Data, accurate: Bool = true) throws -> [OCRObservation] {
        guard let src = CGImageSourceCreateWithData(imageData as CFData, nil) else {
            throw VisionError.decodeFailed("CGImageSourceCreateWithData returned nil")
        }
        guard let cg = CGImageSourceCreateImageAtIndex(src, 0, nil) else {
            throw VisionError.decodeFailed("CGImageSourceCreateImageAtIndex returned nil")
        }
        let imageWidth = CGFloat(cg.width)
        let imageHeight = CGFloat(cg.height)

        let request = VNRecognizeTextRequest()
        request.recognitionLevel = accurate ? .accurate : .fast
        if #available(macOS 14, *) {
            // Sonoma+ ships revision 3, with substantially better accuracy.
            request.revision = VNRecognizeTextRequestRevision3
        }

        let handler = VNImageRequestHandler(cgImage: cg, options: [:])
        do {
            try handler.perform([request])
        } catch {
            throw VisionError.ocrFailed("\(error)")
        }

        guard let observations = request.results else { return [] }
        return observations.compactMap { obs in
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
    }
}
