/*
 * vtreplay — decode a vtcapture recording through VideoToolbox, with and without
 * Apple's per-frame ActiveVideoResolution.
 *
 * The recording holds exactly what avconferenced handed its decoder: the format
 * description, every frame's bytes, and the frameOptions dictionary that went with it.
 * Replaying it lets us answer one question with no wire, no network and no negotiation
 * in the way -- does honouring ActiveVideoResolution change the pictures?
 *
 * Standard HEVC says it cannot: the SPS fixes the coded size for the whole sequence.
 * Apple's hardware decoder does resolution adaptation (RVRA) inside a sequence anyway,
 * and this tool is how we find out whether that is what we have been missing.
 *
 *   vtreplay cap.vtc outdir [--active] [--from N] [--to N]
 */
#import <CoreFoundation/CoreFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>
#import <CoreImage/CoreImage.h>
#import <Foundation/Foundation.h>

enum { REC_SESSION_CREATE = 1, REC_DECODE_INPUT = 2, REC_FORMAT = 3, REC_NOTE = 4 };

static const uint8_t *g_p, *g_end;
static uint32_t rd32(void) { uint32_t v; memcpy(&v, g_p, 4); g_p += 4; return v; }
static uint64_t rd64(void) { uint64_t v; memcpy(&v, g_p, 8); g_p += 8; return v; }
static int64_t  rdi64(void){ int64_t  v; memcpy(&v, g_p, 8); g_p += 8; return v; }
static int32_t  rdi32(void){ int32_t  v; memcpy(&v, g_p, 4); g_p += 4; return v; }
static double   rdf64(void){ double   v; memcpy(&v, g_p, 8); g_p += 8; return v; }
static char *rdstr(void) {
    uint32_t n = rd32(); char *s = malloc(n + 1); memcpy(s, g_p, n); s[n] = 0; g_p += n; return s;
}

static NSString *g_out;
static int g_written;

static CFDictionaryRef numPair(const char *k1, int v1, const char *k2, int v2) {
    return (__bridge_retained CFDictionaryRef)@{ @(k1): @(v1), @(k2): @(v2) };
}

/* Pull "Width = 1088;" style values straight out of the recorded description. The
 * interposer stored CFCopyDescription output, not the dictionary itself, so this is
 * the only place the numbers survive. */
static void scanRect(const char *desc, const char *section, int *w, int *h) {
    *w = *h = 0;
    const char *s = strstr(desc, section);
    if (!s) return;
    const char *hp = strstr(s, "Height ="), *wp = strstr(s, "Width =");
    if (hp) *h = atoi(hp + 8);
    if (wp) *w = atoi(wp + 7);
}

static void onFrame(void *refcon, void *srcRefcon, OSStatus status, VTDecodeInfoFlags flags,
                    CVImageBufferRef image, CMTime pts, CMTime dur) {
    long idx = (long)srcRefcon;
    if (status != noErr || !image) {
        fprintf(stderr, "frame %ld: status %d\n", idx, (int)status);
        return;
    }
    @autoreleasepool {
        CIImage *ci = [CIImage imageWithCVImageBuffer:image];
        CIContext *ctx = [CIContext contextWithOptions:nil];
        NSURL *u = [NSURL fileURLWithPath:[g_out stringByAppendingFormat:@"/f%04ld.png", idx]];
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        [ctx writePNGRepresentationOfImage:ci toURL:u
                                    format:kCIFormatRGBA8 colorSpace:cs options:@{} error:nil];
        CGColorSpaceRelease(cs);
        g_written++;
    }
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: vtreplay cap.vtc outdir [--active] [--from N] [--to N]\n"); return 2; }
    const char *path = argv[1];
    g_out = @(argv[2]);
    int applyActive = 0, from = 0, to = INT32_MAX;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--active")) applyActive = 1;
        else if (!strcmp(argv[i], "--from") && i + 1 < argc) from = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--to") && i + 1 < argc) to = atoi(argv[++i]);
    }
    [[NSFileManager defaultManager] createDirectoryAtPath:g_out
                             withIntermediateDirectories:YES attributes:nil error:nil];

    NSData *blob = [NSData dataWithContentsOfFile:@(path)];
    if (!blob || blob.length < 4 || memcmp(blob.bytes, "VTC1", 4)) {
        fprintf(stderr, "%s: not a vtcapture file\n", path); return 1;
    }
    const uint8_t *base = blob.bytes, *fileEnd = base + blob.length;
    const uint8_t *cur = base + 4;

    CMVideoFormatDescriptionRef fmt = NULL;
    VTDecompressionSessionRef sess = NULL;
    long idx = -1;

    while (cur + 8 <= fileEnd) {
        uint32_t rtype, rlen;
        memcpy(&rtype, cur, 4); memcpy(&rlen, cur + 4, 4);
        cur += 8;
        if (cur + rlen > fileEnd) break;
        g_p = cur; g_end = cur + rlen;
        cur += rlen;

        if (rtype == REC_FORMAT) {
            rd32();                                   /* id */
            rd32(); rd32(); rd32();                   /* codec, width, height */
            uint32_t nalLen = rd32(), n = rd32();
            const uint8_t *ps[8]; size_t psz[8];
            if (n > 8) n = 8;
            for (uint32_t i = 0; i < n; i++) { uint32_t l = rd32(); ps[i] = g_p; psz[i] = l; g_p += l; }
            if (fmt) CFRelease(fmt);
            OSStatus st = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                kCFAllocatorDefault, n, ps, psz, (int)nalLen, NULL, &fmt);
            if (st != noErr) { fprintf(stderr, "format create failed %d\n", (int)st); return 1; }

            /* Same session options avconferenced used, minus the private ones. */
            NSDictionary *spec = @{ (__bridge id)kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder: @YES };
            VTDecompressionOutputCallbackRecord cb = { onFrame, NULL };
            if (sess) { VTDecompressionSessionInvalidate(sess); CFRelease(sess); }
            st = VTDecompressionSessionCreate(kCFAllocatorDefault, fmt,
                                              (__bridge CFDictionaryRef)spec, NULL, &cb, &sess);
            if (st != noErr) { fprintf(stderr, "session create failed %d\n", (int)st); return 1; }
            printf("session up (%u parameter sets, %u-byte NAL length)\n", n, nalLen);
        } else if (rtype == REC_DECODE_INPUT) {
            idx++;
            rdf64(); rd64();                          /* t, session */
            int64_t ptsV = rdi64(); int32_t ptsTS = rdi32();
            rdi64(); rdi32();                         /* dts */
            rd32();                                   /* flags */
            uint32_t notSync = rd32();
            rd32();                                   /* fmt id */
            char *opts = rdstr();
            uint32_t dlen = rd32();
            const uint8_t *data = g_p;

            if (idx < from || idx > to || !sess) { free(opts); continue; }

            CMBlockBufferRef bb = NULL;
            CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, (void *)data, dlen,
                                               kCFAllocatorNull, NULL, 0, dlen, 0, &bb);
            CMSampleBufferRef sb = NULL;
            CMSampleTimingInfo ti = { kCMTimeInvalid,
                                      CMTimeMake(ptsV, ptsTS ? ptsTS : 1000000000),
                                      kCMTimeInvalid };
            size_t sz = dlen;
            CMSampleBufferCreateReady(kCFAllocatorDefault, bb, fmt, 1, 1, &ti, 1, &sz, &sb);
            if (notSync) {
                CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sb, true);
                CFDictionarySetValue((CFMutableDictionaryRef)CFArrayGetValueAtIndex(att, 0),
                                     kCMSampleAttachmentKey_NotSync, kCFBooleanTrue);
            }

            CFDictionaryRef frameOpts = NULL;
            if (applyActive) {
                int aw, ah, cw, ch;
                scanRect(opts, "ActiveVideoResolution", &aw, &ah);
                scanRect(opts, "ContentAnalyzerCropRectangle", &cw, &ch);
                if (aw && ah) {
                    CFDictionaryRef active = numPair("Width", aw, "Height", ah);
                    NSMutableDictionary *d = [NSMutableDictionary dictionary];
                    d[@"ActiveVideoResolution"] = (__bridge_transfer id)active;
                    if (cw && ch)
                        d[@"ContentAnalyzerCropRectangle"] =
                            @{ @"X": @0, @"Y": @0, @"Width": @(cw), @"Height": @(ch) };
                    frameOpts = (__bridge_retained CFDictionaryRef)d;
                }
            }

            VTDecodeInfoFlags info = 0;
            OSStatus st = VTDecompressionSessionDecodeFrameWithOptions(
                sess, sb, kVTDecodeFrame_EnableAsynchronousDecompression,
                frameOpts, (void *)idx, &info);
            if (st != noErr) fprintf(stderr, "frame %ld: decode %d\n", idx, (int)st);
            if (frameOpts) CFRelease(frameOpts);
            CFRelease(sb); CFRelease(bb);
            free(opts);
        }
    }
    if (sess) { VTDecompressionSessionWaitForAsynchronousFrames(sess); }
    printf("wrote %d PNGs to %s (ActiveVideoResolution %s)\n",
           g_written, g_out.UTF8String, applyActive ? "APPLIED" : "ignored");
    return 0;
}
