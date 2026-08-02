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

/// VTDecompressionSessionSetProperty is exported by VideoToolbox but is in neither the SDK
/// headers nor its .tbd, so resolve it at runtime. avconferenced uses it -- not the documented
/// VTSessionSetProperty -- to switch the hardware decoder into resolution-adaptation mode.
private let vtSessionSetProperty: (VTDecompressionSession, CFString, CFTypeRef) -> OSStatus = {
    typealias Fn = @convention(c) (VTDecompressionSession, CFString, CFTypeRef) -> OSStatus
    guard let sym = dlsym(dlopen(nil, RTLD_LAZY), "VTDecompressionSessionSetProperty") else {
        return { _, _, _ in -1 }
    }
    return unsafeBitCast(sym, to: Fn.self)
}()

/// The key the active size travels under, attached to each decoded picture.
let kRPActiveSize = "RPActiveSize" as CFString

/// Decodes access units into pictures. One session per format description; a new format
/// description (new parameter sets) transparently rebuilds it.
final class VideoDecoder {
    /// Called on the decoding thread for every picture, in decode order.
    var onFrame: ((CVPixelBuffer) -> Void)?

    /// The coded size of the next access unit, from the trailer the device appends to its last
    /// slice NAL (HEVCStream.parseActiveRectTrailer). Under motion the encoder drops the coded
    /// picture below the SPS size and squeezes the whole screen into the top-left corner; the
    /// trailer is the only place on the wire that says so. In RVRA mode the decoder needs this
    /// per frame. nil means no trailer seen yet, and the SPS size stands.
    var activeSize: (width: Int, height: Int)?

    /// The size belonging to the picture currently in flight, so it can be attached to that
    /// exact pixel buffer. A shared "current size" read later describes whichever frame has
    /// since been parsed, not the one on screen -- which is what makes the picture vibrate
    /// while the encoder flaps between tiers.
    private var sizeForCallback: (width: Int, height: Int)?

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

        // Per-frame coded resolution, exactly as avconferenced passes it. This is inert on an
        // ordinary session -- verified by replaying 401 of Apple's own captured frames with and
        // without it, which came back byte-identical. It only does anything once
        // VideoResolutionAdaptationType has put the decoder into RVRA mode below.
        let dims = CMVideoFormatDescriptionGetDimensions(desc)
        let aw = activeSize?.width ?? Int(dims.width)
        let ah = activeSize?.height ?? Int(dims.height)
        sizeForCallback = (aw, ah)
        let frameOptions = [
            "ActiveVideoResolution": ["Width": aw, "Height": ah],
            "ContentAnalyzerCropRectangle": ["X": 0, "Y": 0, "Width": aw, "Height": ah],
        ] as CFDictionary

        var flags = VTDecodeInfoFlags()
        let status: OSStatus
        if #available(macOS 15.0, *) {
            status = VTDecompressionSessionDecodeFrame(
                session, sampleBuffer: sample, flags: [], frameOptions: frameOptions,
                frameRefcon: nil, infoFlagsOut: &flags)
        } else {
            // Older SDKs do not surface the frameOptions overload. RVRA needs it, so the tier
            // switches will smear here -- the session properties above are set either way, and
            // a still screen is unaffected.
            status = VTDecompressionSessionDecodeFrame(
                session, sampleBuffer: sample, flags: [], frameRefcon: nil, infoFlagsOut: &flags)
        }
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
        let hevc = CMFormatDescriptionGetMediaSubType(desc) == kCMVideoCodecType_HEVC
        var spec: [String: Any] = [
            // Require, not merely enable. A software fallback keeps up on a still home screen and
            // falls behind exactly when a swipe raises the bitrate, which is the shape of the
            // symptom -- better to fail loudly at session creation than to decode slowly.
            kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder as String: true,
        ]
        if hevc {
            spec["NegotiationDetails"] = "RVRA1:0;SW:1;FLS"
            spec["DecoderUsage"] = 1
            spec["NumberOfTiles"] = 1
        }

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
                if let sz = me.sizeForCallback {
                    CVBufferSetAttachment(image, kRPActiveSize,
                                          ["w": sz.width, "h": sz.height] as CFDictionary,
                                          .shouldPropagate)
                }
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
        if hevc {
            // The switch that makes everything above mean anything. AppleVideoDecoder gates
            // resolution adaptation on VideoResolutionAdaptationType, and until it is set every
            // ActiveVideoResolution is ignored -- so each downshift smears until the next
            // keyframe. Verified offline: the same wire capture that decodes to a mosaic on a
            // plain session decodes clean on this one, all 601 frames.
            vtSessionSetProperty(created, "NegotiationDetails" as CFString,
                                 "RVRA1:0;SW:1;FLS" as CFString)
            vtSessionSetProperty(created, "DecoderUsage" as CFString, 0 as CFNumber)
            let st = vtSessionSetProperty(created, "VideoResolutionAdaptationType" as CFString,
                                          3 as CFNumber)
            AppBuild.log("RVRA: \(st == noErr ? "on" : "rejected (\(st))")")
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
    /// The coded size of the picture being enqueued right now, read back off that picture.
    var onPresentSize: ((CGSize) -> Void)?

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
        // The active size rides on the picture itself, so the view's transform is always the one
        // that belongs to the frame being shown. This is the only place it is published.
        if let d = CVBufferCopyAttachment(picture, kRPActiveSize, nil) as? [String: Int],
           let w = d["w"], let h = d["h"] {
            onPresentSize?(CGSize(width: w, height: h))
        }

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
