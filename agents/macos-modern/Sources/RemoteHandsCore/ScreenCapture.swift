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
import ScreenCaptureKit

/// Supported screen-capture output formats on this family.
///
/// The protocol's `--format` enum is `[png, webp, bmp, jpeg, heic]`. `webp`
/// is declared for forward compatibility but not yet supported here — verb
/// handler returns `ERR unsupported_format`.
public enum CaptureFormat: String, CaseIterable, Sendable {
    case png
    case jpeg
    case heic
    case bmp

    /// Uniform Type Identifier passed to ImageIO. String-form keeps this
    /// file from needing `UniformTypeIdentifiers` (which would add a
    /// transitive dependency and complicate availability).
    public var utType: CFString {
        switch self {
        case .png:  return "public.png" as CFString
        case .jpeg: return "public.jpeg" as CFString
        case .heic: return "public.heic" as CFString
        case .bmp:  return "com.microsoft.bmp" as CFString
        }
    }
}

public enum CaptureError: Error, Equatable {
    /// TCC has not granted this binary Screen Recording. Caller must prompt
    /// the user via System Settings → Privacy & Security → Screen Recording.
    case permissionDenied
    /// Format not implemented on this family (currently: webp).
    case unsupportedFormat(String)
    /// ScreenCaptureKit / ImageIO returned an error. Detail string is
    /// included for logging.
    case captureFailed(String)
}

/// Screen-capture entry points.
///
/// MVP slice uses `SCScreenshotManager.captureImage` (Sonoma 14+). The
/// Ventura 13 fallback via `SCStream` is omitted — it would require an
/// async stream wrapper and frame-delivery callbacks, and Ventura support
/// can be re-added in a later slice if needed.
public enum ScreenCapture {

    /// Capture the entire main display and encode in the requested format.
    /// `--region`, `--window`, `--monitor` selectors land in follow-up
    /// slices; current behaviour is "primary display, full bounds".
    public static func captureFullScreen(format: CaptureFormat, quality: Int) throws -> Data {
        try probeTCC()
        let image = try captureSCSync()
        return try encode(image, format: format, quality: Double(quality) / 100.0)
    }

    /// Returns true if the binary currently holds Screen Recording TCC.
    /// Exposed for `system.info.capabilities.tcc.screen_recording` once
    /// that block lands in SystemInfo.
    public static func hasScreenRecordingPermission() -> Bool {
        return CGPreflightScreenCaptureAccess()
    }

    // MARK: private

    private static func probeTCC() throws {
        if !CGPreflightScreenCaptureAccess() {
            throw CaptureError.permissionDenied
        }
    }

    /// Synchronous bridge to ScreenCaptureKit's async one-shot. The verb
    /// dispatcher is called from a per-connection dispatch queue (sync
    /// world); ScreenCaptureKit is async-only. Bridging via Task.detached +
    /// DispatchSemaphore is the standard pattern; the box is `@unchecked
    /// Sendable` because the only writes happen inside the Task and the
    /// only read happens after `wait()`, with no overlap.
    private static func captureSCSync() throws -> CGImage {
        let box = ResultBox<CGImage>()
        let sem = DispatchSemaphore(value: 0)

        Task.detached {
            do {
                let content = try await SCShareableContent.excludingDesktopWindows(
                    false,
                    onScreenWindowsOnly: true
                )
                guard let display = content.displays.first else {
                    box.set(.failure(CaptureError.captureFailed("no displays available")))
                    sem.signal()
                    return
                }
                let filter = SCContentFilter(display: display, excludingWindows: [])
                let config = SCStreamConfiguration()
                // Capture at the display's pixel-accurate size. SCDisplay
                // .width/.height are in points; scale up by the backing
                // factor so Retina capture isn't downsampled.
                let scale = CGFloat(filter.pointPixelScale)
                config.width = Int(filter.contentRect.width * scale)
                config.height = Int(filter.contentRect.height * scale)
                config.showsCursor = false
                let img = try await SCScreenshotManager.captureImage(
                    contentFilter: filter,
                    configuration: config
                )
                box.set(.success(img))
            } catch {
                box.set(.failure(error))
            }
            sem.signal()
        }

        sem.wait()
        switch box.get() {
        case .success(let img): return img
        case .failure(let err):
            if let cap = err as? CaptureError { throw cap }
            throw CaptureError.captureFailed("\(err)")
        case .none:
            throw CaptureError.captureFailed("capture Task completed without result")
        }
    }

    private static func encode(_ image: CGImage, format: CaptureFormat, quality: Double) throws -> Data {
        guard let buffer = CFDataCreateMutable(nil, 0) else {
            throw CaptureError.captureFailed("CFDataCreateMutable failed")
        }
        guard let dest = CGImageDestinationCreateWithData(buffer, format.utType, 1, nil) else {
            throw CaptureError.unsupportedFormat(format.rawValue)
        }
        let props: [CFString: Any] = [
            kCGImageDestinationLossyCompressionQuality: max(0.0, min(1.0, quality)),
        ]
        CGImageDestinationAddImage(dest, image, props as CFDictionary)
        if !CGImageDestinationFinalize(dest) {
            throw CaptureError.captureFailed("CGImageDestinationFinalize failed")
        }
        return Data(referencing: buffer)
    }
}

/// Minimal Sendable-tolerant container for sync↔async bridging.
private final class ResultBox<T>: @unchecked Sendable {
    private var value: Result<T, Error>?
    private let lock = NSLock()

    func set(_ r: Result<T, Error>) {
        lock.lock(); defer { lock.unlock() }
        value = r
    }

    func get() -> Result<T, Error>? {
        lock.lock(); defer { lock.unlock() }
        return value
    }
}
