//
//  HEVCStream.swift
//  Annex-B byte stream → NAL units → access units → CMSampleBuffer.
//
//  The engine sends a bare Annex-B byte stream: start codes, no container, no timestamps, with
//  the cached VPS/SPS/PPS sent first so a viewer joining mid-stream can configure a decoder.
//  This turns that into access units and hands each one to VideoDecoder.
//
//  It used to hand them to an AVSampleBufferDisplayLayer instead, which decoded them itself. That
//  is the shorter path and it is wrong here: the layer drops frames it judges late, and it drops
//  them before decoding. See VideoDecoder.swift for why that is fatal on this particular stream.
//

import AVFoundation
import CoreMedia
import Foundation

/// Splits an Annex-B byte stream into NAL units, tolerating reads that land anywhere.
final class AnnexBParser {
    private var buf = Data()

    func feed(_ chunk: Data, onNAL: (Data) -> Void) {
        buf.append(chunk)
        guard buf.count > 4 else { return }

        var starts: [Int] = []
        var i = buf.startIndex
        let end = buf.endIndex
        while i + 3 <= end {
            if buf[i] == 0, buf[i + 1] == 0 {
                if i + 3 <= end, buf[i + 2] == 1 {
                    starts.append(i)
                    i += 3
                    continue
                }
                if i + 4 <= end, buf[i + 2] == 0, buf[i + 3] == 1 {
                    starts.append(i)
                    i += 4
                    continue
                }
            }
            i += 1
        }
        guard let last = starts.last else {
            // No start code yet. Keep a small tail so a code split across reads survives.
            if buf.count > 1 << 20 { buf.removeFirst(buf.count - 3) }
            return
        }

        for (idx, s) in starts.enumerated() where idx + 1 < starts.count {
            let codeLen = codeLength(at: s)
            var nal = buf.subdata(in: (s + codeLen)..<starts[idx + 1])
            while nal.last == 0 { nal.removeLast() }      // trailing_zero_8bits
            if !nal.isEmpty { onNAL(nal) }
        }
        // Everything from the final start code onward is still incomplete.
        buf = buf.subdata(in: last..<end)
    }

    private func codeLength(at i: Int) -> Int {
        (i + 3 < buf.endIndex && buf[i + 2] == 0 && buf[i + 3] == 1) ? 4 : 3
    }
}

/// Which codec the engine negotiated. The device picks from the banks we offer, so this is told
/// to us by the engine (`stream_info`) rather than assumed.
enum VideoCodec: String {
    case hevc
    case h264

    /// HEVC has a 2-byte NAL header with the type in bits 1-6; H.264 a 1-byte header, bits 0-4.
    var headerLength: Int { self == .h264 ? 1 : 2 }

    func nalType(_ nal: Data) -> Int {
        guard let first = nal.first else { return -1 }
        return self == .h264 ? Int(first & 0x1F) : Int((first >> 1) & 0x3F)
    }

    func isParameterSet(_ type: Int) -> Bool {
        self == .h264 ? (type == 7 || type == 8) : (32...34).contains(type)
    }

    func isVCL(_ type: Int) -> Bool {
        self == .h264 ? (1...5).contains(type) : type < 32
    }

    func isKeyframe(_ type: Int) -> Bool {
        self == .h264 ? type == 5 : (16...23).contains(type)
    }

    /// The parameter sets a format description needs, in order.
    var parameterSetTypes: [Int] { self == .h264 ? [7, 8] : [32, 33, 34] }
}

/// Assembles NAL units into access units and hands them to a display layer.
final class HEVCStream {
    /// Set from the engine's `stream_info` before frames arrive. Changing it resets the decoder.
    var codec: VideoCodec = .hevc {
        didSet {
            guard codec != oldValue else { return }
            NSLog("rPlayHub: codec is \(codec.rawValue)")
            parameterSets.removeAll()
            format = nil
            accessUnit.removeAll()
            awaitingKeyframe = true
        }
    }

    private var parameterSets: [Int: Data] = [:]
    private var format: CMVideoFormatDescription?
    private var accessUnit: [Data] = []

    /// Frame dimensions, once the parameter sets have told us. Reported on the main queue.
    var onFormat: ((CGSize) -> Void)?
    /// Every assembled access unit, as its list of NAL units, before it becomes a sample buffer.
    /// Only the offline decode-check harness sets this; it exists so the access units this class
    /// builds can be compared against the byte stream they came from.
    var onAccessUnit: (([Data]) -> Void)?
    /// Counters, for the status line.
    private(set) var framesEnqueued = 0
    private(set) var nalsSeen = 0
    /// Frames discarded because no keyframe has arrived to anchor them yet.
    private(set) var framesBeforeKeyframe = 0
    /// Times the transport reported an access unit that did not arrive intact.
    private(set) var discontinuities = 0
    var decodeFailures: Int { decoder.decodeFailures }
    var lastError: String? { decoder.lastError }
    var framesDecoded: Int { decoder.framesDecoded }

    /// True while we are throwing away frames for lack of a keyframe.
    ///
    /// Parameter sets are not enough to start decoding: without an IRAP every P-frame references
    /// pictures the decoder never saw. Feeding it those produces a *black window with no error*,
    /// which is exactly how the first live run failed — so we drop them deliberately and say so
    /// instead, and the engine is asked for a fresh keyframe when a viewer connects.
    private(set) var awaitingKeyframe = true

    /// Splits the direct path's byte stream into NAL units.
    private let annexb = AnnexBParser()
    private var framesSubmitted: Int64 = 0
    /// When the last NAL arrived, so a finished picture can be released once the stream goes quiet.
    private var lastNALAt = CFAbsoluteTimeGetCurrent()
    private let lock = NSLock()
    let decoder: VideoDecoder

    init(decoder: VideoDecoder) {
        self.decoder = decoder
    }

    /// The transport lost packets inside an access unit, so the reference chain is broken.
    ///
    /// Decoding onwards from here is worse than decoding nothing. Every following picture predicts
    /// from one whose reference never arrived, so the decoder reports
    /// kVTVideoDecoderBadDataErr (−12909) for the first and then silently produces wrong pictures
    /// for the rest — which is exactly the symptom this stream shows, because until now the
    /// transport counted the loss and handed the damaged unit over anyway.
    ///
    /// The waiting-for-keyframe state already exists for session start and does precisely the
    /// right thing; it simply was never re-entered mid-stream.
    func signalDiscontinuity() {
        lock.lock()
        defer { lock.unlock() }
        accessUnit.removeAll(keepingCapacity: true)
        awaitingKeyframe = true
        discontinuities += 1
    }

    /// Feed raw Annex-B bytes, as the direct RTP path produces them.
    ///
    /// Takes a pointer rather than Data so the RTP thread does not allocate per NAL: at 60 fps
    /// with fragmented frames that is thousands of allocations a second on a real-time path.
    func feedAnnexB(_ bytes: UnsafePointer<UInt8>, _ count: Int) {
        annexb.feed(Data(bytesNoCopy: UnsafeMutableRawPointer(mutating: bytes), count: count,
                         deallocator: .none)) { [weak self] nal in
            self?.handle(nal: nal)
        }
    }

    func handle(nal: Data) {
        guard nal.count > codec.headerLength else { return }
        lock.lock()
        defer { lock.unlock() }
        lastNALAt = CFAbsoluteTimeGetCurrent()
        nalsSeen += 1
        let type = codec.nalType(nal)

        if codec.isParameterSet(type) {
            flushAccessUnit()
            parameterSets[type] = nal
            rebuildFormatIfPossible()
            return
        }

        guard codec.isVCL(type) else {
            return      // SEI, AUD, end-of-sequence — nothing to display
        }

        // A new picture starts when the first slice does. HEVC signals it with
        // first_slice_segment_in_pic_flag; H.264 with first_mb_in_slice == 0, which as ue(v) is
        // also a leading 1 bit. Both therefore reduce to the top bit of the byte after the header.
        let flagIndex = nal.startIndex + codec.headerLength
        let isFirstSlice = flagIndex < nal.endIndex && (nal[flagIndex] & 0x80) != 0
        if isFirstSlice { flushAccessUnit() }

        if codec.isKeyframe(type) {
            awaitingKeyframe = false
        } else if awaitingKeyframe {
            framesBeforeKeyframe += 1
            return
        }
        accessUnit.append(nal)
    }

    /// The transport says this access unit is complete — submit it now.
    ///
    /// This is what the RTP marker bit is for, and it beats every heuristic below it: no waiting
    /// for the next picture's first slice, no idle timer, and correct for multi-slice pictures
    /// which the first-slice test handles only by luck of them arriving back to back.
    func endAccessUnit() {
        lock.lock()
        defer { lock.unlock() }
        flushAccessUnit()
    }

    /// Release a finished picture that is only waiting for the next one to arrive.
    ///
    /// A picture is assembled when its first slice appears and submitted when the FOLLOWING
    /// picture's first slice appears — which means the newest frame is always held back, and once
    /// motion stops the last frame is never shown at all. On this device that is a whole frame of
    /// latency during movement and a stale screen the moment it ends.
    ///
    /// Waiting for the stream to go quiet is what makes this safe: flushing the instant a slice
    /// arrives would submit half of any multi-slice picture. This device sends exactly one slice
    /// per picture (measured: 601 pictures, 601 first-slice NALs, no continuation slices), but the
    /// idle test costs nothing and keeps the code honest for encoders that do not.
    func flushPendingIfIdle(after seconds: Double = 0.02) {
        lock.lock()
        defer { lock.unlock() }
        guard !accessUnit.isEmpty,
              CFAbsoluteTimeGetCurrent() - lastNALAt >= seconds else { return }
        flushAccessUnit()
    }

    /// Hold every parameter set as a C pointer for the duration of one call. Copying into owned
    /// buffers keeps this readable for any number of sets, rather than nesting withUnsafeBytes.
    private func withParameterSets<R>(
        _ sets: [Data],
        _ body: (UnsafePointer<UnsafePointer<UInt8>>, UnsafePointer<Int>) -> R) -> R {
        var owned: [UnsafeMutablePointer<UInt8>] = []
        var pointers: [UnsafePointer<UInt8>] = []
        var sizes: [Int] = []
        for set in sets {
            let buf = UnsafeMutablePointer<UInt8>.allocate(capacity: set.count)
            set.copyBytes(to: buf, count: set.count)
            owned.append(buf)
            pointers.append(UnsafePointer(buf))
            sizes.append(set.count)
        }
        defer { owned.forEach { $0.deallocate() } }
        return pointers.withUnsafeBufferPointer { pp in
            sizes.withUnsafeBufferPointer { ss in
                body(pp.baseAddress!, ss.baseAddress!)
            }
        }
    }

    private func rebuildFormatIfPossible() {
        guard format == nil else { return }
        let wanted = codec.parameterSetTypes
        let sets = wanted.compactMap { parameterSets[$0] }
        guard sets.count == wanted.count else { return }     // still waiting for one

        var desc: CMVideoFormatDescription?
        let status: OSStatus = withParameterSets(sets) { pointers, sizes in
            switch codec {
            case .hevc:
                return CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                    allocator: kCFAllocatorDefault,
                    parameterSetCount: sets.count,
                    parameterSetPointers: pointers,
                    parameterSetSizes: sizes,
                    nalUnitHeaderLength: 4,
                    extensions: nil,
                    formatDescriptionOut: &desc)
            case .h264:
                return CMVideoFormatDescriptionCreateFromH264ParameterSets(
                    allocator: kCFAllocatorDefault,
                    parameterSetCount: sets.count,
                    parameterSetPointers: pointers,
                    parameterSetSizes: sizes,
                    nalUnitHeaderLength: 4,
                    formatDescriptionOut: &desc)
            }
        }
        guard status == noErr, let desc else {
            NSLog("rPlayHub: could not build \(codec.rawValue) format description (status \(status))")
            return
        }
        format = desc
        let dims = CMVideoFormatDescriptionGetDimensions(desc)
        let size = CGSize(width: CGFloat(dims.width), height: CGFloat(dims.height))
        DispatchQueue.main.async { [weak self] in self?.onFormat?(size) }
    }

    private func flushAccessUnit() {
        defer { accessUnit.removeAll(keepingCapacity: true) }
        guard !accessUnit.isEmpty else { return }
        onAccessUnit?(accessUnit)
        guard let format else { return }

        // Strip the per-frame trailer and hand its size to the decoder. The device appends the
        // coded resolution to the last slice NAL of every access unit; avconferenced removes it
        // before decoding (measured: 601/601 slice NALs on the wire carry it, 0/1368 of what
        // avconferenced feeds VideoToolbox does). The bytes themselves are harmless -- they sit
        // past rbsp_slice_trailing_bits and every decoder ignores them, which is why stripping
        // alone changes nothing -- but the size is the only signal that the encoder downshifted.
        if !accessUnit.isEmpty,
           let (w, h, cut) = Self.parseActiveRectTrailer(accessUnit[accessUnit.count - 1]) {
            let last = accessUnit.count - 1
            accessUnit[last] = accessUnit[last].prefix(cut)
            decoder.activeSize = (w, h)
        }

        // Length-prefixed (HVCC-style) is what the format description declares.
        var payload = Data()
        payload.reserveCapacity(accessUnit.reduce(0) { $0 + $1.count + 4 })
        for nal in accessUnit {
            var be = UInt32(nal.count).bigEndian
            withUnsafeBytes(of: &be) { payload.append(contentsOf: $0) }
            payload.append(nal)
        }

        guard let sample = makeSampleBuffer(payload, format: format) else { return }

        // Every access unit is decoded, unconditionally and in order. Nothing is dropped here:
        // the device sends one IDR per session and never another, so a skipped frame would
        // corrupt the reference chain for good. Whether the picture then reaches the screen is
        // the display layer's business, and skipping *there* is free.
        decoder.decode(sample)
        framesEnqueued += 1
    }

    /// Find the coded-resolution trailer at the end of a slice NAL, returning
    /// (width, height, byte offset where the trailer starts) or nil.
    ///
    ///     [width:u16be][height:u16be][00 ...][4-byte session tag]
    ///     04a0 0a10 0000030000049209e403   = 1184x2576
    ///     0440 0780 0000030000049209e403   = 1088x1920
    ///     02d0 0500 0000030000049209e403   =  720x1280
    ///
    /// Matching against the tiers this encoder family actually uses, plus a zero byte after the
    /// pair, is what keeps entropy-coded slice data from matching by accident: five fixed bytes
    /// over a ~20-byte window is far too specific to hit by chance, and a false positive would
    /// truncate real slice data.
    static func parseActiveRectTrailer(_ nal: Data) -> (Int, Int, Int)? {
        let tiers: [(Int, Int)] = [(1184, 2576), (1088, 1920), (720, 1280)]
        let bytes = [UInt8](nal)
        let n = bytes.count
        guard n > 24 else { return nil }
        for (w, h) in tiers {
            let pat = [UInt8(w >> 8), UInt8(w & 0xff), UInt8(h >> 8), UInt8(h & 0xff)]
            var i = n - 5
            while i >= max(1, n - 24) {
                if bytes[i] == pat[0], bytes[i + 1] == pat[1],
                   bytes[i + 2] == pat[2], bytes[i + 3] == pat[3], bytes[i + 4] == 0 {
                    return (w, h, i)
                }
                i -= 1
            }
        }
        return nil
    }

    private func makeSampleBuffer(_ payload: Data,
                                  format: CMVideoFormatDescription) -> CMSampleBuffer? {
        var block: CMBlockBuffer?
        var status = CMBlockBufferCreateWithMemoryBlock(
            allocator: kCFAllocatorDefault,
            memoryBlock: nil,
            blockLength: payload.count,
            blockAllocator: kCFAllocatorDefault,
            customBlockSource: nil,
            offsetToData: 0,
            dataLength: payload.count,
            flags: kCMBlockBufferAssureMemoryNowFlag,
            blockBufferOut: &block)
        guard status == noErr, let block else { return nil }

        status = payload.withUnsafeBytes { raw in
            CMBlockBufferReplaceDataBytes(with: raw.baseAddress!,
                                          blockBuffer: block,
                                          offsetIntoDestination: 0,
                                          dataLength: payload.count)
        }
        guard status == noErr else { return nil }

        var sample: CMSampleBuffer?
        var size = payload.count
        // Simple monotonic timestamps. Nothing schedules on these any more — we decode each
        // access unit ourselves the moment it is assembled and show the picture as soon as it
        // comes back — but VideoToolbox still wants timing that strictly increases.
        var timing = CMSampleTimingInfo(
            duration: CMTime(value: 1, timescale: 60),
            presentationTimeStamp: CMTime(value: framesSubmitted, timescale: 60),
            decodeTimeStamp: .invalid)
        framesSubmitted += 1
        status = CMSampleBufferCreateReady(
            allocator: kCFAllocatorDefault,
            dataBuffer: block,
            formatDescription: format,
            sampleCount: 1,
            sampleTimingEntryCount: 1,
            sampleTimingArray: &timing,
            sampleSizeEntryCount: 1,
            sampleSizeArray: &size,
            sampleBufferOut: &sample)
        guard status == noErr, let sample else { return nil }

        return sample
    }
}
