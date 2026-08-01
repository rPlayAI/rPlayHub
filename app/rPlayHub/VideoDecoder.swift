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
import CoreImage
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

    /// Where to write live decoded pictures, and how many to write, from the environment.
    private lazy var liveDumpDir: String? = {
        let d = ProcessInfo.processInfo.environment["RPLAYHUB_LIVE_FRAMES"]
        guard let d, !d.isEmpty else { return nil }
        try? FileManager.default.createDirectory(atPath: d, withIntermediateDirectories: true)
        return d
    }()
    private lazy var liveDumpEvery =
        Int(ProcessInfo.processInfo.environment["RPLAYHUB_LIVE_STRIDE"] ?? "") ?? 1
    private lazy var liveDumpLimit =
        Int(ProcessInfo.processInfo.environment["RPLAYHUB_LIVE_LIMIT"] ?? "") ?? 120
    private var liveDumped = 0
    private static let ciContext = CIContext(options: [.useSoftwareRenderer: false])

    /// Write a decoded picture as it left the decoder, before the display layer sees it.
    ///
    /// Deliberately identical in method to app/tools/decodecheck: lock, wrap the base address in a
    /// CGContext, write a PNG. If that harness renders a stream correctly and this does not, the
    /// difference is upstream of display; if both are correct, only display is left.
    fileprivate func dumpIfAsked(_ picture: CVPixelBuffer) {
        guard let dir = liveDumpDir, liveDumped < liveDumpLimit,
              framesDecoded % max(1, liveDumpEvery) == 0 else { return }
        // Go through CoreImage rather than wrapping the base address in a CGContext. The first
        // attempt did the latter and wrote nothing at all: it assumed BGRA, and the live session's
        // decoder emits its native format, so CGContext creation failed and the frame was dropped
        // silently. CIImage handles whatever the decoder produced, biplanar 4:2:0 included.
        let fourcc = CVPixelBufferGetPixelFormatType(picture)
        let ci = CIImage(cvPixelBuffer: picture)
        guard let cg = Self.ciContext.createCGImage(ci, from: ci.extent) else {
            if liveDumped == 0 {
                let c = String(bytes: [UInt8((fourcc >> 24) & 0xff), UInt8((fourcc >> 16) & 0xff),
                                       UInt8((fourcc >> 8) & 0xff), UInt8(fourcc & 0xff)],
                               encoding: .ascii) ?? "?"
                AppBuild.log("live frame dump: cannot render pixel format '\(c)'")
            }
            return
        }
        let url = URL(fileURLWithPath: dir)
            .appendingPathComponent(String(format: "live-%05d.png", framesDecoded))
        guard let dest = CGImageDestinationCreateWithURL(
                url as CFURL, "public.png" as CFString, 1, nil) else { return }
        CGImageDestinationAddImage(dest, cg, nil)
        if CGImageDestinationFinalize(dest) {
            if liveDumped == 0 {
                let c = String(bytes: [UInt8((fourcc >> 24) & 0xff), UInt8((fourcc >> 16) & 0xff),
                                       UInt8((fourcc >> 8) & 0xff), UInt8(fourcc & 0xff)],
                               encoding: .ascii) ?? "?"
                AppBuild.log("live frame dump: writing \(url.deletingLastPathComponent().path), "
                             + "pixel format '\(c)'")
            }
            liveDumped += 1
        }
    }
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

        // Set RPLAYHUB_LIVE_FRAMES=<dir> to write what the LIVE decoder produced, straight from
        // the CVPixelBuffer, before it reaches the display layer.
        //
        // This exists because the offline harness and the running app disagree. Fed the exact
        // bytes captured from a session that looked garbled, `decodecheck` — the same parser, the
        // same assembler, the same VideoDecoder — produced 197 pixel-perfect pictures with zero
        // failures. The only stage it does not exercise is presentation. Dumping here says which
        // of the two it is without any inference: if these PNGs are clean while the window is
        // garbled, nothing upstream of the display layer is at fault.
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
                me.dumpIfAsked(image)
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
    /// Pictures decoded but not shown. Not a decode loss: the decoder consumed every one of them,
    /// so the reference chain is intact — these were superseded before a display refresh came
    /// round, which is the point of the design rather than a failure of it.
    private(set) var framesSkipped = 0
    /// Pictures actually put on screen, one per display refresh at most.
    private(set) var framesPresented = 0

    private var format: CMVideoFormatDescription?

    // MARK: - Timed presentation
    //
    // Modelled on what avconferenced actually does, read off its own thread names and stack
    // frames while it was mirroring:
    //
    //     VCJitterBuffer_EnqueuePacket
    //     VideoReceiver_VideoAlarmForDecode          decode on a media-clock alarm
    //     _VideoReceiver_EnqueueDecodedFrameForDisplay
    //     _VCImageQueue_EnqueuePixelBuffer           into an image queue
    //     VideoReceiver_DisplayLinkTick              presented on the display refresh
    //
    // We used to hand every decoded picture straight to the layer with DisplayImmediately, from
    // the decode thread, once per access unit. At 40 fps into a layer that accepts frames at its
    // own pace, that measured 29,609 of 53,664 pictures discarded by the layer -- 55% -- with the
    // choice of *which* to discard left to AVSampleBufferDisplayLayer.
    //
    // Now the newest decoded picture simply replaces the pending one, and a display link presents
    // whatever is pending when the screen is actually about to refresh. Same decode path, same
    // reference chain, but the picture shown is the most recent one that exists at the moment the
    // display can use it, and nothing is enqueued that will never be seen.
    /// The live picture rectangle, as the encoder last declared it. The decoded frame is always
    /// full size; only this sub-rectangle, anchored top-left, holds the current image.
    private var activeSize: CGSize?
    private var pending: CVPixelBuffer?
    private var pendingAt: CFTimeInterval = 0
    private let pendingLock = NSLock()
    private var displayLink: CVDisplayLink?

    override init() {
        super.init()
        startDisplayLink()
    }

    override init(layer: Any) {
        super.init(layer: layer)
        startDisplayLink()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        startDisplayLink()
    }

    deinit {
        if let displayLink { CVDisplayLinkStop(displayLink) }
    }

    /// CVDisplayLink is deprecated in favour of NSView.displayLink, but the presentation target
    /// here is a layer with no view of its own, and the layer is what the ports need to keep
    /// (core/hwdecoder.h hands back frames, not views). Revisit if this moves into the view.
    private func startDisplayLink() {
        var link: CVDisplayLink?
        guard CVDisplayLinkCreateWithActiveCGDisplays(&link) == kCVReturnSuccess,
              let link else { return }
        let me = Unmanaged.passUnretained(self).toOpaque()
        CVDisplayLinkSetOutputCallback(link, { _, _, _, _, _, ctx in
            guard let ctx else { return kCVReturnSuccess }
            Unmanaged<VideoLayer>.fromOpaque(ctx).takeUnretainedValue().displayTick()
            return kCVReturnSuccess
        }, me)
        CVDisplayLinkStart(link)
        displayLink = link
    }

    /// The coded picture changed size. Rescale so the live region fills the layer, matching what
    /// avconferenced does with ContentAnalyzerCropRectangle{X:0,Y:0,W,H}.
    func setActiveRect(width: Int, height: Int) {
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.activeSize = CGSize(width: width, height: height)
            self.applyActiveRect()
        }
    }

    private func applyActiveRect() {
        // NOT contentsRect: that applies to a layer's `contents`, and AVSampleBufferDisplayLayer
        // renders its own content and ignores it. Setting it looked like a fix and did nothing.
        // An anchored scale works on any layer, and the superlayer clips what spills out.
        guard let full = format.map({ CMVideoFormatDescriptionGetDimensions($0) }),
              full.width > 0, full.height > 0 else { return }
        var active = activeSize ?? CGSize(width: CGFloat(full.width), height: CGFloat(full.height))
        // RPLAYHUB_ACTIVE_RECT=WxH forces a fixed rectangle, so the cropping can be verified on its
        // own without depending on the wire signal being decoded correctly.
        if let forced = ProcessInfo.processInfo.environment["RPLAYHUB_ACTIVE_RECT"] {
            let p = forced.lowercased().split(separator: "x").compactMap { Double($0) }
            if p.count == 2 { active = CGSize(width: p[0], height: p[1]) }
        }
        let fx = max(0.05, min(1.0, active.width / CGFloat(full.width)))
        let fy = max(0.05, min(1.0, active.height / CGFloat(full.height)))
        // Anchor at the top-left corner, which is where the live region lives
        // (ContentAnalyzerCropRectangle is always X=0, Y=0), then scale it up to fill.
        anchorPoint = CGPoint(x: 0, y: 0)
        position = CGPoint(x: frame.origin.x, y: frame.origin.y)
        transform = CATransform3DMakeScale(1.0 / fx, 1.0 / fy, 1)
    }

    /// One display refresh. Take whatever the decoder has produced most recently and show it.
    private func displayTick() {
        pendingLock.lock()
        let picture = pending
        pending = nil
        pendingLock.unlock()
        guard let picture else { return }      // nothing new since the last refresh
        enqueueForDisplay(picture)
    }

    /// Called on the decode thread for every decoded picture, in order.
    ///
    /// Cheap by design: it stores the picture and returns. No sample buffer is built, no work is
    /// dispatched to the main queue, and nothing touches the layer -- all of which used to happen
    /// once per access unit on the thread that also reads the socket.
    func present(_ picture: CVPixelBuffer) {
        pendingLock.lock()
        if pending != nil { framesSkipped += 1 }   // superseded before the screen could use it
        pending = picture
        pendingAt = CACurrentMediaTime()
        pendingLock.unlock()
    }

    /// Build the sample buffer and hand it to the layer. Runs on the display link.
    private func enqueueForDisplay(_ picture: CVPixelBuffer) {
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

        // Already on the display link, one frame per refresh at most, so there is no queue to
        // build up and no reason to bounce through the main queue first. Flush only on hard
        // failure: flushing because the layer is merely "not ready" discards what is already
        // queued and starves the renderer -- rplay carries a comment about exactly that.
        if status == .failed { flush() }
        if isReadyForMoreMediaData {
            enqueue(sample)
            framesPresented += 1
        } else {
            framesSkipped += 1
        }
    }
}
