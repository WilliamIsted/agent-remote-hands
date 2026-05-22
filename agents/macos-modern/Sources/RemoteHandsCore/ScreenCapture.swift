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
#if canImport(Darwin)
import Darwin
#endif

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
    /// CoreGraphics / ImageIO returned an error. Detail string is included
    /// for logging.
    case captureFailed(String)
}

/// Screen-capture entry points.
///
/// Capture uses `CGDisplayCreateImage` — a synchronous CoreGraphics call
/// that grabs the display framebuffer directly. Two other paths were tried
/// and rejected:
///
/// - **ScreenCaptureKit** (`SCShareableContent` / `SCStream`): its async
///   machinery requires a running main run loop, which this headless
///   wire-protocol server does not provide — the first SCK call hangs
///   indefinitely.
/// - **`CGWindowListCreateImage`**: returns `nil` in this process context
///   (it composites the window list, a path that fails outside a normal
///   GUI app). `CGDisplayCreateImage` grabs the framebuffer instead and
///   works — the same path Apple's `screencapture(1)` uses.
///
/// `CGDisplayCreateImage` was deprecated in Sequoia 15 and is absent from
/// the macOS 26 SDK headers, but remains in the runtime dylib on every
/// macOS this agent targets, so it is resolved via `dlsym` at first use.
public enum ScreenCapture {

    /// Capture the entire main display and encode in the requested format.
    /// `--region`, `--window`, `--monitor` selectors land in follow-up
    /// slices; current behaviour is "primary display, full bounds".
    public static func captureFullScreen(format: CaptureFormat, quality: Int) throws -> Data {
        try probeTCC()
        let image = try captureMainDisplay()
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

    /// C signature of `CGDisplayCreateImage(CGDirectDisplayID) -> CGImageRef`.
    /// `CGDirectDisplayID` is a `UInt32`. It is a "Create" function
    /// (returns a +1 reference), hence the `Unmanaged` return.
    private typealias CGDisplayCreateImageFn =
        @convention(c) (UInt32) -> Unmanaged<CGImage>?

    /// `CGDisplayCreateImage`, resolved once by symbol name. Absent from
    /// the macOS 26 build SDK but present in every macOS runtime this agent
    /// targets. `nil` only if a future macOS removes it from the runtime
    /// too — in which case capture fails cleanly rather than crashing.
    private static let displayCreateImage: CGDisplayCreateImageFn? = {
        guard let handle = dlopen(
                  "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics",
                  RTLD_NOW),
              let symbol = dlsym(handle, "CGDisplayCreateImage")
        else {
            return nil
        }
        return unsafeBitCast(symbol, to: CGDisplayCreateImageFn.self)
    }()

    /// Capture a specific display at full backing-pixel resolution.
    /// `internal` so the vision.ocr source resolver can capture any
    /// display, not just the main one.
    static func captureDisplay(_ displayID: CGDirectDisplayID) throws -> CGImage {
        guard let displayCreateImage = displayCreateImage else {
            throw CaptureError.captureFailed("CGDisplayCreateImage unavailable on this system")
        }
        guard let image = displayCreateImage(displayID)?.takeRetainedValue() else {
            throw CaptureError.captureFailed("CGDisplayCreateImage returned nil")
        }
        return image
    }

    /// Capture the main display at full backing-pixel resolution.
    private static func captureMainDisplay() throws -> CGImage {
        return try captureDisplay(CGMainDisplayID())
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
