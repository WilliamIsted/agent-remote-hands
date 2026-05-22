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
import AppKit

/// A resolved OCR input: the image to recognise, the coordinate space its
/// bounding boxes should be reported in (`"screen"` or `"image"`), and the
/// screen-space origin (in points) to add to image-local boxes. `origin`
/// is `.zero` for image-space sources.
struct ResolvedVisionImage {
    let cgImage: CGImage
    let coordinateSpace: String
    let origin: CGPoint
}

enum VisionSourceError: Error {
    case invalidArgs(String)
    case notFound(String)
}

/// Resolves `vision.ocr`'s source selectors to a single image to OCR.
/// Exactly one of region/window/monitor/path/bytes may be supplied; none
/// means "the whole main display".
enum VisionSource {

    static func resolve(_ parsed: ParsedArgs, payload: Data) throws -> ResolvedVisionImage {
        let selectors = ["region", "window", "monitor", "path"].filter { parsed.flags[$0] != nil }
        let hasBytes = !payload.isEmpty || parsed.flags["bytes-format"] != nil
        if selectors.count + (hasBytes ? 1 : 0) > 1 {
            throw VisionSourceError.invalidArgs(
                "vision.ocr sources are mutually exclusive (region/window/monitor/path/bytes)")
        }

        if let region = parsed.flags["region"] { return try resolveRegion(region) }
        if let window = parsed.flags["window"] { return try resolveWindow(window) }
        if let monitor = parsed.flags["monitor"] { return try resolveMonitor(monitor) }
        if let path = parsed.flags["path"] { return try resolvePath(path) }
        if hasBytes { return try resolveBytes(payload, format: parsed.flags["bytes-format"]) }

        // No selector — OCR the whole main display.
        let image = try ScreenCapture.captureDisplay(CGMainDisplayID())
        return ResolvedVisionImage(cgImage: image, coordinateSpace: "screen", origin: .zero)
    }

    // MARK: source resolvers

    private static func resolveRegion(_ raw: String) throws -> ResolvedVisionImage {
        let parts = raw.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }
        guard parts.count == 4,
              let x = Int(parts[0]), let y = Int(parts[1]),
              let w = Int(parts[2]), let h = Int(parts[3]) else {
            throw VisionSourceError.invalidArgs("--region must be x,y,w,h integers")
        }
        guard w >= 1, h >= 1 else {
            throw VisionSourceError.invalidArgs("--region width and height must be >= 1")
        }
        let full = try ScreenCapture.captureDisplay(CGMainDisplayID())
        let cropped = try crop(full, toPointsRect: CGRect(x: x, y: y, width: w, height: h))
        return ResolvedVisionImage(
            cgImage: cropped, coordinateSpace: "screen", origin: CGPoint(x: x, y: y))
    }

    private static func resolveWindow(_ handle: String) throws -> ResolvedVisionImage {
        let info: WindowInfo
        do {
            info = try Window.resolveWindow(idStr: handle)
        } catch {
            throw VisionSourceError.notFound("no window for handle \"\(handle)\"")
        }
        let full = try ScreenCapture.captureDisplay(CGMainDisplayID())
        let cropped = try crop(full, toPointsRect: info.bounds)
        return ResolvedVisionImage(
            cgImage: cropped, coordinateSpace: "screen", origin: info.bounds.origin)
    }

    private static func resolveMonitor(_ raw: String) throws -> ResolvedVisionImage {
        guard let index = Int(raw), index >= 0 else {
            throw VisionSourceError.invalidArgs("--monitor must be a non-negative integer")
        }
        let screens = NSScreen.screens
        guard index < screens.count else {
            throw VisionSourceError.notFound("--monitor \(index) out of range (\(screens.count) displays)")
        }
        let screen = screens[index]
        guard let num = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber else {
            throw VisionSourceError.notFound("could not resolve display id for monitor \(index)")
        }
        let image = try ScreenCapture.captureDisplay(CGDirectDisplayID(num.uint32Value))
        return ResolvedVisionImage(
            cgImage: image, coordinateSpace: "screen", origin: screen.frame.origin)
    }

    private static func resolvePath(_ path: String) throws -> ResolvedVisionImage {
        try FileGuard.ensureAllowed(path)
        guard let data = FileManager.default.contents(atPath: path) else {
            throw VisionSourceError.invalidArgs("--path: cannot read \"\(path)\"")
        }
        let image = try decode(data)
        return ResolvedVisionImage(cgImage: image, coordinateSpace: "image", origin: .zero)
    }

    private static func resolveBytes(_ payload: Data, format: String?) throws -> ResolvedVisionImage {
        guard let format = format, !format.isEmpty else {
            throw VisionSourceError.invalidArgs("--bytes-format is required when image bytes are supplied")
        }
        let allowed: Set<String> = ["png", "jpeg", "bmp", "webp", "heic", "tiff"]
        guard allowed.contains(format) else {
            throw VisionSourceError.invalidArgs("--bytes-format \"\(format)\" is not one of \(allowed.sorted())")
        }
        guard !payload.isEmpty else {
            throw VisionSourceError.invalidArgs("--bytes-format given but no image bytes were supplied")
        }
        let image = try decode(payload)
        return ResolvedVisionImage(cgImage: image, coordinateSpace: "image", origin: .zero)
    }

    // MARK: helpers

    /// Decode encoded image bytes via ImageIO.
    private static func decode(_ data: Data) throws -> CGImage {
        guard let src = CGImageSourceCreateWithData(data as CFData, nil),
              let image = CGImageSourceCreateImageAtIndex(src, 0, nil) else {
            throw VisionSourceError.invalidArgs("image bytes are not a decodable image")
        }
        return image
    }

    /// Crop a backing-pixel CGImage to a rectangle expressed in points.
    /// `CGDisplayCreateImage` returns a backing-pixel image (2× on HiDPI),
    /// but region / window bounds are in points — scale the crop rect by
    /// the main display's backing factor.
    private static func crop(_ image: CGImage, toPointsRect rect: CGRect) throws -> CGImage {
        let scale = NSScreen.main?.backingScaleFactor ?? 1.0
        let pixelRect = CGRect(
            x: rect.origin.x * scale, y: rect.origin.y * scale,
            width: rect.size.width * scale, height: rect.size.height * scale)
        guard let cropped = image.cropping(to: pixelRect) else {
            throw VisionSourceError.invalidArgs(
                "crop rectangle \(rect) is outside the captured image bounds")
        }
        return cropped
    }
}
