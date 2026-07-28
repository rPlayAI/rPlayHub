//
//  VideoDecoder.swift
//  Explicit VideoToolbox decoding, and a layer that shows the pictures it produces.
//
//  This replaces AVSampleBufferDisplayLayer's enqueue path, for one reason: that layer decides
//  for itself when to drop frames, and it drops them BEFORE decoding. For ordinary video that is
//  invisible, because the next keyframe repairs whatever the gap corrupted. displayservice sends
//  exactly ONE IDR per session and ignores PLI and FIR, so there is no repair — a single frame
//  the layer decides to skip corrupts every frame after it for the rest of the session. That is
//  the heavy artifacting seen while the engine measured 0% packet loss, and an offline harness
//  confirmed the rest of the path is innocent: VideoToolbox decoded 380 of 380 access units from
//  a live capture with no errors, from bytes the app rendered as snow.
//
//  So the two stages are separated here. Decode is mandatory and happens for every access unit,
//  in order, keeping the reference chain intact. Display is best-effort: if the screen cannot
//  keep up, the newest picture replaces the pending one and older pictures are simply never
//  shown. Skipping a *displayed* picture costs nothing, because the decoder already consumed it.
//
//  This is also the shape the ports need. `core/hwdecoder.h` is the common decoder interface —
//  VideoToolbox here, ffmpeg/SDL on Linux and Windows — and that interface hands back frames,
//  not sample buffers. AVSampleBufferDisplayLayer has no equivalent anywhere else.
//

import CoreMedia
import CoreVideo
import Foundation
import IOSurface
import QuartzCore
import VideoToolbox

/// Decodes access units into pictures. One session per format description; a new format
/// description (new parameter sets) transparently rebuilds it.
final class VideoDecoder {
    /// Called on the decoding thread for every picture, in decode order.
    var onFrame: ((CVPixelBuffer) -> Void)?

    private(set) var framesDecoded = 0
    private(set) var decodeFailures = 0
    private(set) var lastError: String?

    private var session: VTDecompressionSession?
    private var format: CMVideoFormatDescription?

    deinit { invalidate() }

    func invalidate() {
        if let session {
            VTDecompressionSessionWaitForAsynchronousFrames(session)
            VTDecompressionSessionInvalidate(session)
        }
        session = nil
        format = nil
    }

    /// Decode one access unit. Synchronous: VideoToolbox invokes the output callback before this
    /// returns, so pictures arrive in decode order with no reordering queue to manage. The stream
    /// has no B-frames (confirmed on a real capture: one I-frame followed by P-frames only), so
    /// decode order is display order and there is nothing to gain from asynchronous decoding.
    func decode(_ sample: CMSampleBuffer) {
        guard let desc = CMSampleBufferGetFormatDescription(sample),
              let session = ensureSession(for: desc) else { return }

        var flags = VTDecodeInfoFlags()
        let status = VTDecompressionSessionDecodeFrame(
            session, sampleBuffer: sample, flags: [], frameRefcon: nil, infoFlagsOut: &flags)
        if status != noErr {
            decodeFailures += 1
            lastError = "decode failed (\(status))"
            // A session that has started refusing frames stays broken; rebuild it on the next
            // access unit rather than reporting the same error forever.
            if status == kVTInvalidSessionErr || status == kVTVideoDecoderMalfunctionErr {
                invalidate()
            }
        }
    }

    private func ensureSession(for desc: CMVideoFormatDescription) -> VTDecompressionSession? {
        if let session, let format, CMFormatDescriptionEqual(format, otherFormatDescription: desc) {
            return session
        }
        invalidate()

        // BGRA rather than a YCbCr plane pair: CoreAnimation displays a BGRA IOSurface directly
        // as layer contents, so the picture reaches the screen with no conversion of our own.
        let attributes: [String: Any] = [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
            kCVPixelBufferIOSurfacePropertiesKey as String: [:] as CFDictionary,
            kCVPixelBufferMetalCompatibilityKey as String: true,
        ]
        let spec: [String: Any] = [
            kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder as String: true,
        ]

        var callback = VTDecompressionOutputCallbackRecord(
            decompressionOutputCallback: { refcon, _, status, _, image, _, _ in
                guard let refcon else { return }
                let me = Unmanaged<VideoDecoder>.fromOpaque(refcon).takeUnretainedValue()
                guard status == noErr, let image else {
                    me.decodeFailures += 1
                    me.lastError = "picture dropped by the decoder (\(status))"
                    return
                }
                me.framesDecoded += 1
                me.onFrame?(image)
            },
            decompressionOutputRefCon: Unmanaged.passUnretained(self).toOpaque())

        var created: VTDecompressionSession?
        let status = VTDecompressionSessionCreate(
            allocator: kCFAllocatorDefault,
            formatDescription: desc,
            decoderSpecification: spec as CFDictionary,
            imageBufferAttributes: attributes as CFDictionary,
            outputCallback: &callback,
            decompressionSessionOut: &created)
        guard status == noErr, let created else {
            decodeFailures += 1
            lastError = "could not create a decoder (\(status))"
            return nil
        }
        session = created
        format = desc
        return created
    }
}

/// A layer that shows decoded pictures, newest-wins.
///
/// Pictures arrive on the decoding thread; layers must be touched on the main thread. Rather than
/// queueing every picture onto the main queue — which would build an unbounded backlog and show
/// progressively staler frames — the newest picture replaces whatever has not been shown yet, and
/// exactly one main-queue hop is in flight at a time.
final class VideoLayer: CALayer {
    /// Pictures decoded but never displayed because a newer one arrived first. Not a decode loss.
    private(set) var framesSkipped = 0

    private let lock = NSLock()
    private var pending: CVPixelBuffer?
    private var scheduled = false

    /// The picture currently on screen, held for exactly as long as it is on screen.
    ///
    /// This reference is load-bearing, not bookkeeping. VideoToolbox hands out pixel buffers from
    /// a recycling pool, and a buffer returns to that pool the moment its last CVPixelBuffer
    /// reference goes away — the IOSurface that CoreAnimation retains as layer contents does NOT
    /// hold it. Dropping our reference after assigning `contents` therefore frees the decoder to
    /// decode the *next* frame straight into the surface being displayed, so the screen shows a
    /// buffer being overwritten under it: tearing and torn-in garbage on a stream with no packet
    /// loss at all. Holding the buffer until it is replaced keeps it out of the pool.
    private var displayed: CVPixelBuffer?

    func present(_ picture: CVPixelBuffer) {
        lock.lock()
        if pending != nil { framesSkipped += 1 }
        pending = picture
        let needsHop = !scheduled
        scheduled = true
        lock.unlock()

        guard needsHop else { return }
        DispatchQueue.main.async { [weak self] in self?.show() }
    }

    private func show() {
        lock.lock()
        let picture = pending
        pending = nil
        scheduled = false
        lock.unlock()

        guard let picture, let surface = CVPixelBufferGetIOSurface(picture) else { return }
        displayed = picture          // must outlive the assignment below — see the declaration
        // Implicit animation on `contents` would cross-fade every frame into the next.
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        contents = surface.takeUnretainedValue()
        CATransaction.commit()
    }
}
