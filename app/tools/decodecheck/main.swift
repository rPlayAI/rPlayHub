//
//  decodecheck — run the app's real decode path over a file, with no device and no GUI.
//
//      decodecheck <annex-b file> [chunk-size]
//
//  The engine has been proven to emit decodable video: an independent decoder rendered 382 frames
//  from build/diagnose/live.h265 with 0% loss, while the app showed heavy artifacting on the same
//  stream. That leaves two possibilities, and inspection cannot separate them — so this feeds a
//  known-good file through the app's own AnnexBParser and HEVCStream, in socket-sized chunks, and
//  reports:
//
//    * whether the access units it assembles round-trip back to the input byte-for-byte
//      (any difference IS corruption, introduced by our parser rather than by the device), and
//    * whether VideoToolbox decodes those access units, frame by frame, with real error codes.
//
//  Chunk size matters: the parser is fed whatever the socket returns, so a bug that only appears
//  when a NAL straddles a read boundary is invisible at chunk = whole file. Pass a small chunk to
//  provoke it.
//
import AVFoundation
import CoreMedia
import Foundation
import CoreGraphics
import ImageIO
import UniformTypeIdentifiers
import VideoToolbox

let args = CommandLine.arguments
guard args.count >= 2 else {
    FileHandle.standardError.write(Data("usage: decodecheck <annex-b file> [chunk-size]\n".utf8))
    exit(2)
}
let path = args[1]
let chunkSize = args.count > 2 ? Int(args[2]) ?? 65536 : 65536

guard let input = FileManager.default.contents(atPath: path) else {
    FileHandle.standardError.write(Data("cannot read \(path)\n".utf8))
    exit(2)
}
print("input: \(path)  \(input.count) bytes, feeding in \(chunkSize)-byte chunks")

// ---------------------------------------------------------------- what the input actually holds
func scanNALs(_ data: Data) -> [Data] {
    var out: [Data] = []
    var starts: [Int] = []
    var i = 0
    while i + 3 <= data.count {
        if data[i] == 0, data[i + 1] == 0 {
            if data[i + 2] == 1 { starts.append(i); i += 3; continue }
            if i + 4 <= data.count, data[i + 2] == 0, data[i + 3] == 1 { starts.append(i); i += 4; continue }
        }
        i += 1
    }
    for (idx, s) in starts.enumerated() {
        let codeLen = (s + 3 < data.count && data[s + 2] == 0 && data[s + 3] == 1) ? 4 : 3
        let end = idx + 1 < starts.count ? starts[idx + 1] : data.count
        var nal = data.subdata(in: (s + codeLen)..<end)
        while nal.last == 0 { nal.removeLast() }
        if !nal.isEmpty { out.append(nal) }
    }
    return out
}

let inputNALs = scanNALs(input)
var inputTypes: [Int: Int] = [:]
for n in inputNALs { inputTypes[Int((n[n.startIndex] >> 1) & 0x3F), default: 0] += 1 }
print("input NALs: \(inputNALs.count)  types: \(inputTypes.sorted { $0.key < $1.key }.map { "\($0.key)x\($0.value)" }.joined(separator: " "))")

// ------------------------------------------------------------------ run the app's own decode path
let stream = HEVCStream(decoder: VideoDecoder())
stream.codec = .hevc

var assembled: [[Data]] = []
stream.onAccessUnit = { assembled.append($0) }

let parser = AnnexBParser()
var emittedNALs: [Data] = []
var offset = 0
while offset < input.count {
    let end = min(offset + chunkSize, input.count)
    parser.feed(input.subdata(in: offset..<end)) { nal in
        emittedNALs.append(nal)
        stream.handle(nal: nal)
    }
    offset = end
}

// Release the picture still waiting for a successor, the same way the app's idle timer does.
// Without this the harness under-reports by one and hides the held-frame behaviour entirely.
stream.flushPendingIfIdle(after: 0)

print("parser emitted \(emittedNALs.count) NALs (input had \(inputNALs.count); "
      + "the last is expected to be withheld as incomplete)")

// Round-trip: do the NALs the parser produced match the ones actually in the file?
var mismatches = 0
for (i, nal) in emittedNALs.enumerated() where i < inputNALs.count {
    if nal != inputNALs[i] {
        mismatches += 1
        if mismatches <= 5 {
            print("  MISMATCH at NAL \(i): parser \(nal.count) bytes, file \(inputNALs[i].count) bytes")
        }
    }
}
print(mismatches == 0
      ? "  round-trip: every emitted NAL is byte-identical to the file"
      : "  round-trip: \(mismatches) NALs DIFFER from the file — the parser is corrupting the stream")

print("access units assembled: \(assembled.count)  "
      + "(frames before keyframe: \(stream.framesBeforeKeyframe), NALs seen: \(stream.nalsSeen))")

// --------------------------------------------------------------------------- does it actually decode
let sets = inputNALs.filter { (32...34).contains(Int(($0[$0.startIndex] >> 1) & 0x3F)) }
var vps: Data?, sps: Data?, pps: Data?
for s in sets {
    switch Int((s[s.startIndex] >> 1) & 0x3F) {
    case 32: if vps == nil { vps = s }
    case 33: if sps == nil { sps = s }
    default: if pps == nil { pps = s }
    }
}
guard let vps, let sps, let pps else {
    print("no complete VPS/SPS/PPS in the file — cannot build a format description")
    exit(1)
}

var format: CMVideoFormatDescription?
let mk: OSStatus = vps.withUnsafeBytes { v in sps.withUnsafeBytes { s in pps.withUnsafeBytes { p in
    var ptrs = [v.baseAddress!.assumingMemoryBound(to: UInt8.self),
                s.baseAddress!.assumingMemoryBound(to: UInt8.self),
                p.baseAddress!.assumingMemoryBound(to: UInt8.self)]
    var sizes = [vps.count, sps.count, pps.count]
    return CMVideoFormatDescriptionCreateFromHEVCParameterSets(
        allocator: kCFAllocatorDefault, parameterSetCount: 3,
        parameterSetPointers: &ptrs, parameterSetSizes: &sizes,
        nalUnitHeaderLength: 4, extensions: nil, formatDescriptionOut: &format)
} } }
guard mk == noErr, let format else { print("format description failed: \(mk)"); exit(1) }
let dims = CMVideoFormatDescriptionGetDimensions(format)
print("format: \(dims.width)x\(dims.height)")

// Decode through the SHIPPING path — the same VideoDecoder the app uses, BGRA and all — and
// write pictures out as PNG. "VideoToolbox returned no error" is not the same as "the picture is
// right", and only looking at a frame settles which.
// Not beside the input: captures often live in root-owned directories left by `sudo
// diagnose.sh`, and createDirectory there fails silently, which cost a debugging round.
let outDir = ProcessInfo.processInfo.environment["RPLAYHUB_FRAMES"]
    ?? FileManager.default.currentDirectoryPath + "/build/decodecheck-frames"
do { try FileManager.default.createDirectory(atPath: outDir, withIntermediateDirectories: true) }
catch { print("cannot create \(outDir): \(error)") }

let vt = VideoDecoder()
var decoded = 0
var written = 0
vt.onFrame = { picture in
    decoded += 1
    // How many frames to write. The default is a spot check — the first, then every 60th, up to
    // eight — because that is enough to see whether a stream is healthy. Set RPLAYHUB_FRAME_STRIDE=1
    // to dump every frame when the whole sequence matters.
    let every = Int(ProcessInfo.processInfo.environment["RPLAYHUB_FRAME_STRIDE"] ?? "") ?? 60
    let cap = Int(ProcessInfo.processInfo.environment["RPLAYHUB_FRAME_LIMIT"] ?? "") ?? 8
    guard decoded == 1 || decoded % every == 0, written < cap else { return }
    let lockRC = CVPixelBufferLockBaseAddress(picture, .readOnly)
    defer { CVPixelBufferUnlockBaseAddress(picture, .readOnly) }
    let w = CVPixelBufferGetWidth(picture)
    let h = CVPixelBufferGetHeight(picture)
    let fmt = CVPixelBufferGetPixelFormatType(picture)
    let fourcc = String(bytes: [UInt8((fmt >> 24) & 0xFF), UInt8((fmt >> 16) & 0xFF),
                                UInt8((fmt >> 8) & 0xFF), UInt8(fmt & 0xFF)], encoding: .ascii) ?? "?"
    if decoded == 1 {
        print("  picture: \(w)x\(h) format=\(fourcc) planar=\(CVPixelBufferIsPlanar(picture)) lock=\(lockRC) surface=\(CVPixelBufferGetIOSurface(picture) != nil)")
    }
    guard let base = CVPixelBufferGetBaseAddress(picture) else {
        if decoded == 1 { print("  !! no base address") }
        return
    }
    let stride = CVPixelBufferGetBytesPerRow(picture)
    let cs = CGColorSpaceCreateDeviceRGB()
    let info = CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipFirst.rawValue
                            | CGBitmapInfo.byteOrder32Little.rawValue)
    guard let ctx = CGContext(data: base, width: w, height: h, bitsPerComponent: 8,
                              bytesPerRow: stride, space: cs, bitmapInfo: info.rawValue),
          let image = ctx.makeImage() else { if decoded == 1 { print("  !! CGContext/makeImage failed") }; return }
    let url = URL(fileURLWithPath: outDir).appendingPathComponent("frame-\(String(format: "%04d", decoded)).png")
    guard let dest = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil)
    else { return }
    CGImageDestinationAddImage(dest, image, nil)
    if CGImageDestinationFinalize(dest) { written += 1 }
}

var submitted = 0, buildFailures = 0
for unit in assembled {
    var payload = Data()
    for nal in unit {
        var be = UInt32(nal.count).bigEndian
        withUnsafeBytes(of: &be) { payload.append(contentsOf: $0) }
        payload.append(nal)
    }
    var block: CMBlockBuffer?
    guard CMBlockBufferCreateWithMemoryBlock(allocator: kCFAllocatorDefault, memoryBlock: nil,
            blockLength: payload.count, blockAllocator: kCFAllocatorDefault, customBlockSource: nil,
            offsetToData: 0, dataLength: payload.count, flags: kCMBlockBufferAssureMemoryNowFlag,
            blockBufferOut: &block) == noErr, let block else { buildFailures += 1; continue }
    _ = payload.withUnsafeBytes { CMBlockBufferReplaceDataBytes(with: $0.baseAddress!,
            blockBuffer: block, offsetIntoDestination: 0, dataLength: payload.count) }
    var sample: CMSampleBuffer?
    var size = payload.count
    var timing = CMSampleTimingInfo(duration: CMTime(value: 1, timescale: 60),
                                    presentationTimeStamp: CMTime(value: Int64(submitted), timescale: 60),
                                    decodeTimeStamp: .invalid)
    guard CMSampleBufferCreateReady(allocator: kCFAllocatorDefault, dataBuffer: block,
            formatDescription: format, sampleCount: 1, sampleTimingEntryCount: 1,
            sampleTimingArray: &timing, sampleSizeEntryCount: 1, sampleSizeArray: &size,
            sampleBufferOut: &sample) == noErr, let sample else { buildFailures += 1; continue }
    vt.decode(sample)
    submitted += 1
}

print("submitted \(submitted) access units, sample-buffer build failures \(buildFailures)")
print("VideoDecoder produced \(decoded) pictures, failures \(vt.decodeFailures)"
      + (vt.lastError != nil ? "  (\(vt.lastError!))" : ""))
print("wrote \(written) PNGs to \(outDir)")

if mismatches == 0 && vt.decodeFailures == 0 && decoded >= submitted - 1 {
    print("\nVERDICT: parse, assemble and decode are all clean. Look at the PNGs — if they are")
    print("         correct too, the remaining fault is display only.")
} else {
    print("\nVERDICT: the app's own decode path is at fault on bytes an independent decoder accepts.")
}
