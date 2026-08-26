//
//  groundtruth — decode an Annex-B capture through the app's REAL RVRA VideoToolbox path and
//  dump every picture as raw YUV, as the oracle for portable-decoder work.
//
//      groundtruth <capture.h265> <out.y4m> [index.tsv]
//
//  Why this exists: ffmpeg (and every conformant HEVC decoder) decodes this stream conformantly
//  and WRONG — RVRA requires references to be resampled when the encoder downshifts, which
//  standard HEVC does not do, so software decode is a garbled mosaic from the first downshift on
//  (doc/RVRA-AND-PORTABILITY.md). The one decoder that gets it right is VideoToolbox with the
//  private RVRA properties, which is exactly the path HEVCStream + VideoDecoder already implement
//  for the live app. This harness runs that path over a file and writes what it produces, so a
//  patched software decoder has a frame-by-frame target to converge on:
//
//      ffmpeg -i <capture.h265> -strict -1 -f yuv4mpegpipe candidate.y4m
//      python3 scripts/compare-decodes.py groundtruth.y4m candidate.y4m <capture.h265>
//
//  The output is y4m (C420jpeg: 4:2:0, full range, as the stream is coded) because ffmpeg reads
//  it directly — the comparator is ffmpeg's own psnr filter, not bespoke pixel code. The .tsv
//  records each frame's active size from the RVRA trailer, so divergence can be read against the
//  tier switches that cause it.
//
import AVFoundation
import CoreMedia
import CoreVideo
import Foundation
import VideoToolbox

let args = CommandLine.arguments
guard args.count >= 3 else {
    FileHandle.standardError.write(Data("usage: groundtruth <capture.h265> <out.y4m> [index.tsv]\n".utf8))
    exit(2)
}
guard let input = FileManager.default.contents(atPath: args[1]) else {
    FileHandle.standardError.write(Data("cannot read \(args[1])\n".utf8))
    exit(2)
}
FileManager.default.createFile(atPath: args[2], contents: nil)
guard let out = FileHandle(forWritingAtPath: args[2]) else {
    FileHandle.standardError.write(Data("cannot write \(args[2])\n".utf8))
    exit(2)
}
var indexLines = "frame\tactive_w\tactive_h\n"

// The app's real path: parser -> access units -> trailer strip + activeSize -> RVRA session.
let decoder = VideoDecoder()
let stream = HEVCStream(decoder: decoder)
stream.codec = .hevc

var frames = 0
var wroteHeader = false
var reportedFormat = false

decoder.onFrame = { picture in
    CVPixelBufferLockBaseAddress(picture, .readOnly)
    defer { CVPixelBufferUnlockBaseAddress(picture, .readOnly) }
    let w = CVPixelBufferGetWidth(picture)
    let h = CVPixelBufferGetHeight(picture)
    let fmt = CVPixelBufferGetPixelFormatType(picture)

    if !wroteHeader {
        // XCOLORRANGE is what modern ffmpeg actually keys full-range off; C420jpeg alone reads
        // as limited and the psnr filter would silently range-convert one input (a constant
        // ~29 dB floor that looks like decode error and is not).
        out.write(Data("YUV4MPEG2 W\(w) H\(h) F60:1 Ip A1:1 C420jpeg XYSCSS=420JPEG XCOLORRANGE=FULL\n".utf8))
        wroteHeader = true
    }
    if !reportedFormat {
        reportedFormat = true
        let cc = String(bytes: [UInt8((fmt >> 24) & 0xFF), UInt8((fmt >> 16) & 0xFF),
                                UInt8((fmt >> 8) & 0xFF), UInt8(fmt & 0xFF)], encoding: .ascii) ?? "?"
        print("picture: \(w)x\(h) format=\(cc) planes=\(CVPixelBufferGetPlaneCount(picture))")
    }
    out.write(Data("FRAME\n".utf8))

    // Copy a plane row by row (bytesPerRow includes padding the file must not).
    func writePlane(_ idx: Int, _ pw: Int, _ ph: Int) {
        let base = CVPixelBufferGetBaseAddressOfPlane(picture, idx)!
        let stride = CVPixelBufferGetBytesPerRowOfPlane(picture, idx)
        var row = Data(count: pw)
        for y in 0..<ph {
            row.withUnsafeMutableBytes { dst in
                memcpy(dst.baseAddress!, base.advanced(by: y * stride), pw)
            }
            out.write(row)
        }
    }

    switch CVPixelBufferGetPlaneCount(picture) {
    case 3:                                          // already planar 4:2:0
        writePlane(0, w, h)
        writePlane(1, w / 2, h / 2)
        writePlane(2, w / 2, h / 2)
    case 2:                                          // NV12-style: deinterleave the chroma plane
        writePlane(0, w, h)
        let base = CVPixelBufferGetBaseAddressOfPlane(picture, 1)!
        let stride = CVPixelBufferGetBytesPerRowOfPlane(picture, 1)
        let cw = w / 2, ch = h / 2
        var u = Data(count: cw * ch), v = Data(count: cw * ch)
        u.withUnsafeMutableBytes { up in
            v.withUnsafeMutableBytes { vp in
                let ud = up.bindMemory(to: UInt8.self).baseAddress!
                let vd = vp.bindMemory(to: UInt8.self).baseAddress!
                for y in 0..<ch {
                    let src = base.advanced(by: y * stride).assumingMemoryBound(to: UInt8.self)
                    for x in 0..<cw {
                        ud[y * cw + x] = src[2 * x]
                        vd[y * cw + x] = src[2 * x + 1]
                    }
                }
            }
        }
        out.write(u)
        out.write(v)
    default:
        FileHandle.standardError.write(Data("unsupported plane count\n".utf8))
        exit(1)
    }

    // The active size the decoder attached to this very picture (kRPActiveSize), so the index
    // is what was actually decoded with, not a separate re-parse that could drift.
    var aw = w, ah = h
    if let att = CVBufferGetAttachment(picture, kRPActiveSize, nil)?.takeUnretainedValue()
        as? [String: Int], let x = att["w"], let y = att["h"] {
        aw = x; ah = y
    }
    indexLines += "\(frames)\t\(aw)\t\(ah)\n"
    frames += 1
}

// Feed in socket-sized chunks, exactly as the live path would see the bytes.
var offset = 0
while offset < input.count {
    let end = min(offset + 65536, input.count)
    let chunk = input.subdata(in: offset..<end)
    stream.feedAnnexB([UInt8](chunk), chunk.count)
    offset = end
}
// The parser withholds the final NAL until a next start code proves it complete; a file has no
// next start code, so provide one, then release the last assembled picture.
stream.feedAnnexB([0, 0, 0, 1], 4)
stream.flushPendingIfIdle(after: 0)

try? out.close()
if args.count > 3 {
    try? indexLines.write(toFile: args[3], atomically: true, encoding: .utf8)
}
print("decoded \(frames) frames (\(stream.framesBeforeKeyframe) dropped before keyframe, "
      + "\(decoder.decodeFailures) failures\(decoder.lastError.map { ", last: \($0)" } ?? ""))")
exit(decoder.decodeFailures == 0 && frames > 0 ? 0 : 1)
