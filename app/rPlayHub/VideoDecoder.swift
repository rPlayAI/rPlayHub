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

import AVFoundation
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

        // No destination attributes, so the decoder emits its own native format and nothing
        // converts anything. We used to ask for BGRA, which made VideoToolbox colour-convert
        // every single frame -- 1184x2544 of it -- purely so the picture could be assigned as
        // CALayer contents. Handing pictures to AVSampleBufferDisplayLayer instead removes the
        // reason that conversion existed: it takes YCbCr directly. This is what rplay does.
        let spec: [String: Any] = [
            // Require, not merely enable. A software fallback keeps up on a still home screen and
            // falls behind exactly when a swipe raises the bitrate, which is the shape of the
            // symptom -- better to fail loudly at session creation than to decode slowly.
            kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder as String: true,
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
            imageBufferAttributes: nil,   // native format -- see above
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

/// Shows decoded pictures, using the same presentation rplay uses (`libAirPlay2/hwdecoder.m`).
///
/// Every picture is wrapped in a CMSampleBuffer carrying `kCMSampleAttachmentKey_DisplayImmediately`
/// and enqueued; if the layer is not ready, the picture is dropped rather than queued.
///
/// Enqueueing DECODED pictures is what makes this safe, and it is the whole difference from the
/// arrangement this file was originally written to escape. Feeding *compressed* samples to this
/// layer lets it decide what to decode, and it drops before decoding -- fatal on a stream that
/// sends one IDR per session, because a skipped frame breaks the reference chain and corrupts
/// everything after it. Here decoding has already happened, in order, for every access unit. The
/// layer only ever chooses whether to show a finished picture, which costs nothing.
///
/// `DisplayImmediately` is the part that matters for latency: without it the layer honours each
/// sample's presentation timestamp and holds the picture until that time arrives. For a live
/// mirror there is nothing to synchronise against, so waiting only adds delay.
final class VideoLayer: AVSampleBufferDisplayLayer {
    /// Pictures decoded but not shown because the layer was not ready. Not a decode loss: the
    /// decoder consumed them, so the reference chain is intact.
    private(set) var framesSkipped = 0

    private var format: CMVideoFormatDescription?

    func present(_ picture: CVPixelBuffer) {
        var desc = format
        if desc == nil || !CMVideoFormatDescriptionMatchesImageBuffer(desc!, imageBuffer: picture) {
            var made: CMVideoFormatDescription?
            guard CMVideoFormatDescriptionCreateForImageBuffer(
                    allocator: kCFAllocatorDefault,
                    imageBuffer: picture,
                    formatDescriptionOut: &made) == noErr, let made else { return }
            format = made
            desc = made
        }

        var timing = CMSampleTimingInfo(duration: .invalid,
                                        presentationTimeStamp: .invalid,
                                        decodeTimeStamp: .invalid)
        var sample: CMSampleBuffer?
        guard CMSampleBufferCreateForImageBuffer(
                allocator: kCFAllocatorDefault,
                imageBuffer: picture,
                dataReady: true,
                makeDataReadyCallback: nil,
                refcon: nil,
                formatDescription: desc!,
                sampleTiming: &timing,
                sampleBufferOut: &sample) == noErr, let sample else { return }

        if let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: true),
           CFArrayGetCount(attachments) > 0 {
            let dict = unsafeBitCast(CFArrayGetValueAtIndex(attachments, 0),
                                     to: CFMutableDictionary.self)
            CFDictionarySetValue(dict,
                                 Unmanaged.passUnretained(kCMSampleAttachmentKey_DisplayImmediately).toOpaque(),
                                 Unmanaged.passUnretained(kCFBooleanTrue).toOpaque())
        }

        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            // Flush only on hard failure. Flushing because the layer is merely "not ready"
            // discards what is already queued and starves the renderer -- rplay carries a comment
            // about exactly that, having chased the resulting lag once.
            if self.status == .failed { self.flush() }
            if self.isReadyForMoreMediaData {
                self.enqueue(sample)
            } else {
                self.framesSkipped += 1
            }
        }
    }
}
