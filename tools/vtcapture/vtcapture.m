// vtcapture — record what AVConference feeds its video decoder, and how that
// decoder is configured, from inside Device Hub.
//
// Every measurement in doc/RENDERING-HANDOFF.md is our pipeline measuring
// itself. This observes Apple's. Device Hub links AVConference directly and
// AVConference imports VTDecompressionSessionDecodeFrame and
// VTDecompressionSessionCreateWithOptions from VideoToolbox, so a dyld
// interposer loaded into Device Hub sees the decoder's input exactly as the
// decoder sees it — after AVConference's own depacketizer, which is the stage we
// have never been able to observe.
//
// Load it with DYLD_INSERT_LIBRARIES. That works on Device Hub despite its
// entitlements only because this machine boots with amfi_get_out_of_my_way=1.
//
// PERTURBATION. RPLAY_RTP_FORWARD taught us that one extra syscall per packet on
// a media thread is enough to destroy the thing being measured. So the hooks
// never do I/O: they memcpy into a preallocated ring and a writer thread drains
// it. Nothing here allocates or blocks on the decode thread. Frames are ~40/s,
// not ~360/s like packets, so the margin is much larger than it was there, but
// the discipline is the same.
//
// This build is READ-ONLY. It records arguments and passes them through
// untouched; it does not substitute the decoder's output callback. Capturing
// emitted pictures needs that substitution and is a separate, riskier step.

#import <CoreImage/CoreImage.h>
#include <ImageIO/ImageIO.h>
#include <sys/stat.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <VideoToolbox/VideoToolbox.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ---------------------------------------------------------------- record format

#define VTCAP_MAGIC "VTC1"

enum {
    REC_SESSION_CREATE = 1,  // a decompression session was created
    REC_DECODE_INPUT   = 2,  // a sample buffer was handed to the decoder
    REC_FORMAT         = 3,  // a format description (with parameter sets)
    REC_NOTE           = 4,  // free text / a raw plane of a decoded picture
    REC_DECODE_OUTPUT  = 5,  // a picture the decoder produced
};

// Wire layout, little-endian throughout:
//   u32 type | u32 len | <len bytes>
// REC_DECODE_INPUT payload:
//   f64 t | u64 session | i64 pts_value | i32 pts_scale | i64 dts_value |
//   i32 dts_scale | u32 decode_flags | u32 not_sync | u32 fmt_id | u32 data_len |
//   <data_len bytes>
// REC_FORMAT payload:
//   u32 fmt_id | u32 codec | u32 width | u32 height | u32 nal_len_size |
//   u32 n_param_sets | { u32 len | bytes } * n | u32 desc_len | <utf8>
// REC_SESSION_CREATE payload:
//   f64 t | u64 session | u32 fmt_id | u32 which | u32 n_dicts |
//   { u32 label_len | utf8 | u32 desc_len | utf8 } * n

// ---------------------------------------------------------------- ring buffer

#define RING_BYTES (96u * 1024u * 1024u)

static struct {
    uint8_t *buf;
    size_t   head;              // producer writes here
    size_t   tail;              // consumer reads here
    pthread_mutex_t lock;
    pthread_cond_t  wake;
    int      fd;
    int      running;
    _Atomic uint64_t dropped;   // records lost to a full ring
    _Atomic uint64_t written;
    double   t0;
} R;

static double now_sec(void) {
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0) mach_timebase_info(&tb);
    uint64_t t = mach_absolute_time();
    return (double)t * tb.numer / tb.denom / 1e9;
}

static size_t ring_free(void) {
    // One byte kept unused so head == tail always means empty.
    return (R.tail + RING_BYTES - R.head - 1) % RING_BYTES;
}

static void ring_put(const void *p, size_t n) {
    size_t first = RING_BYTES - R.head;
    if (first > n) first = n;
    memcpy(R.buf + R.head, p, first);
    if (n > first) memcpy(R.buf, (const uint8_t *)p + first, n - first);
    R.head = (R.head + n) % RING_BYTES;
}

// Append one record. Called on media threads: memcpy and a mutex, never I/O.
static void emit(uint32_t type, const void *a, size_t alen,
                 const void *b, size_t blen) {
    uint32_t len = (uint32_t)(alen + blen);
    size_t need = 8 + len;
    pthread_mutex_lock(&R.lock);
    if (!R.running || ring_free() < need) {
        atomic_fetch_add(&R.dropped, 1);
        pthread_mutex_unlock(&R.lock);
        return;
    }
    ring_put(&type, 4);
    ring_put(&len, 4);
    if (alen) ring_put(a, alen);
    if (blen) ring_put(b, blen);
    atomic_fetch_add(&R.written, 1);
    pthread_cond_signal(&R.wake);
    pthread_mutex_unlock(&R.lock);
}

static void *writer_thread(void *unused) {
    (void)unused;
    uint8_t *chunk = malloc(4u * 1024u * 1024u);
    for (;;) {
        pthread_mutex_lock(&R.lock);
        while (R.running && R.head == R.tail)
            pthread_cond_wait(&R.wake, &R.lock);
        size_t avail = (R.head + RING_BYTES - R.tail) % RING_BYTES;
        if (!avail && !R.running) { pthread_mutex_unlock(&R.lock); break; }
        if (avail > 4u * 1024u * 1024u) avail = 4u * 1024u * 1024u;
        size_t first = RING_BYTES - R.tail;
        if (first > avail) first = avail;
        memcpy(chunk, R.buf + R.tail, first);
        if (avail > first) memcpy(chunk + first, R.buf, avail - first);
        R.tail = (R.tail + avail) % RING_BYTES;
        pthread_mutex_unlock(&R.lock);
        ssize_t off = 0;
        while ((size_t)off < avail) {
            ssize_t w = write(R.fd, chunk + off, avail - off);
            if (w <= 0) break;
            off += w;
        }
    }
    free(chunk);
    return NULL;
}

// ---------------------------------------------------------------- helpers

// A small buffer we build records in, on the stack, to avoid malloc on hot paths.
typedef struct { uint8_t b[4096]; size_t n; } buf_t;

static void put(buf_t *s, const void *p, size_t n) {
    if (s->n + n > sizeof s->b) return;
    memcpy(s->b + s->n, p, n);
    s->n += n;
}
static void put_u32(buf_t *s, uint32_t v) { put(s, &v, 4); }
static void put_u64(buf_t *s, uint64_t v) { put(s, &v, 8); }
static void put_i64(buf_t *s, int64_t v)  { put(s, &v, 8); }
static void put_i32(buf_t *s, int32_t v)  { put(s, &v, 4); }
static void put_f64(buf_t *s, double v)   { put(s, &v, 8); }

static void put_cfstr(buf_t *s, CFStringRef str) {
    char tmp[1024];
    tmp[0] = 0;
    if (str) CFStringGetCString(str, tmp, sizeof tmp, kCFStringEncodingUTF8);
    uint32_t n = (uint32_t)strlen(tmp);
    put_u32(s, n);
    put(s, tmp, n);
}

static void put_cfdesc(buf_t *s, CFTypeRef obj) {
    if (!obj) { put_u32(s, 0); return; }
    CFStringRef d = CFCopyDescription(obj);
    put_cfstr(s, d);
    if (d) CFRelease(d);
}

// Format descriptions are recorded once each and referred to by id afterwards,
// so a 60 fps stream does not re-record its parameter sets 60 times a second.
static struct { const void *fmt; uint32_t id; } fmt_seen[64];
static uint32_t fmt_count = 0;
static pthread_mutex_t fmt_lock = PTHREAD_MUTEX_INITIALIZER;

static void record_format(CMFormatDescriptionRef fmt, uint32_t id);

static uint32_t fmt_id_for(CMFormatDescriptionRef fmt) {
    if (!fmt) return 0;
    pthread_mutex_lock(&fmt_lock);
    for (uint32_t i = 0; i < fmt_count; i++)
        if (fmt_seen[i].fmt == fmt) {
            uint32_t id = fmt_seen[i].id;
            pthread_mutex_unlock(&fmt_lock);
            return id;
        }
    uint32_t id = 0;
    if (fmt_count < 64) {
        id = ++fmt_count;
        fmt_seen[id - 1].fmt = fmt;
        fmt_seen[id - 1].id  = id;
    }
    pthread_mutex_unlock(&fmt_lock);
    if (id) record_format(fmt, id);
    return id;
}

static void record_format(CMFormatDescriptionRef fmt, uint32_t id) {
    buf_t s = {0};
    CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(fmt);
    FourCharCode codec = CMFormatDescriptionGetMediaSubType(fmt);

    size_t count = 0;
    int nal_len = 0;
    int hevc = (codec == kCMVideoCodecType_HEVC);
    OSStatus st = hevc
        ? CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fmt, 0, NULL, NULL, &count, &nal_len)
        : CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, 0, NULL, NULL, &count, &nal_len);
    if (st != noErr) { count = 0; nal_len = 0; }

    put_u32(&s, id);
    put_u32(&s, codec);
    put_u32(&s, (uint32_t)dim.width);
    put_u32(&s, (uint32_t)dim.height);
    put_u32(&s, (uint32_t)nal_len);
    put_u32(&s, (uint32_t)count);
    for (size_t i = 0; i < count; i++) {
        const uint8_t *p = NULL;
        size_t n = 0;
        OSStatus r = hevc
            ? CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fmt, i, &p, &n, NULL, NULL)
            : CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, i, &p, &n, NULL, NULL);
        if (r != noErr || !p) { put_u32(&s, 0); continue; }
        put_u32(&s, (uint32_t)n);
        put(&s, p, n);
    }
    CFDictionaryRef ext = CMFormatDescriptionGetExtensions(fmt);
    put_cfdesc(&s, ext);
    emit(REC_FORMAT, s.b, s.n, NULL, 0);
}

// ---------------------------------------------------------------- the hooks

static uint8_t *sample_bytes(CMSampleBufferRef sb, size_t *out_len) {
    CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
    if (!bb) { *out_len = 0; return NULL; }
    size_t total = CMBlockBufferGetDataLength(bb);
    if (!total || total > 32u * 1024u * 1024u) { *out_len = 0; return NULL; }
    // Thread-local scratch avoids malloc on the decode thread.
    static __thread uint8_t *scratch = NULL;
    static __thread size_t   scratch_n = 0;
    if (scratch_n < total) {
        uint8_t *p = realloc(scratch, total);
        if (!p) { *out_len = 0; return NULL; }
        scratch = p;
        scratch_n = total;
    }
    if (CMBlockBufferCopyDataBytes(bb, 0, total, scratch) != kCMBlockBufferNoErr) {
        *out_len = 0;
        return NULL;
    }
    *out_len = total;
    return scratch;
}

static uint32_t sample_not_sync(CMSampleBufferRef sb) {
    CFArrayRef atts = CMSampleBufferGetSampleAttachmentsArray(sb, false);
    if (!atts || CFArrayGetCount(atts) == 0) return 0;
    CFDictionaryRef d = CFArrayGetValueAtIndex(atts, 0);
    CFBooleanRef v = CFDictionaryGetValue(d, kCMSampleAttachmentKey_NotSync);
    return (v && CFBooleanGetValue(v)) ? 1 : 0;
}

static OSStatus my_VTDecompressionSessionDecodeFrame(
        VTDecompressionSessionRef session, CMSampleBufferRef sampleBuffer,
        VTDecodeFrameFlags decodeFlags, void *sourceFrameRefCon,
        VTDecodeInfoFlags *infoFlagsOut) {

    if (R.running && sampleBuffer) {
        CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sampleBuffer);
        uint32_t fid = fmt_id_for(fmt);
        CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
        CMTime dts = CMSampleBufferGetDecodeTimeStamp(sampleBuffer);
        size_t len = 0;
        uint8_t *data = sample_bytes(sampleBuffer, &len);

        buf_t s = {0};
        put_f64(&s, now_sec() - R.t0);
        put_u64(&s, (uint64_t)(uintptr_t)session);
        put_i64(&s, pts.value);
        put_i32(&s, pts.timescale);
        put_i64(&s, dts.value);
        put_i32(&s, dts.timescale);
        put_u32(&s, (uint32_t)decodeFlags);
        put_u32(&s, sample_not_sync(sampleBuffer));
        put_u32(&s, fid);
        put_u32(&s, (uint32_t)len);
        emit(REC_DECODE_INPUT, s.b, s.n, data, len);
    }

    return VTDecompressionSessionDecodeFrame(session, sampleBuffer, decodeFlags,
                                             sourceFrameRefCon, infoFlagsOut);
}

// The public creator. AVConference imports the WithOptions variant, but hooking
// this one costs nothing and covers anything else in the process.
static OSStatus my_VTDecompressionSessionCreate(
        CFAllocatorRef allocator, CMVideoFormatDescriptionRef fmt,
        CFDictionaryRef spec, CFDictionaryRef destAttrs,
        const VTDecompressionOutputCallbackRecord *cb,
        VTDecompressionSessionRef *out) {

    OSStatus st = VTDecompressionSessionCreate(allocator, fmt, spec, destAttrs, cb, out);
    if (R.running) {
        buf_t s = {0};
        put_f64(&s, now_sec() - R.t0);
        put_u64(&s, (uint64_t)(uintptr_t)(out ? *out : NULL));
        put_u32(&s, fmt_id_for(fmt));
        put_u32(&s, 0);                       // which = plain Create
        put_u32(&s, 2);
        put_cfstr(&s, CFSTR("videoDecoderSpecification"));
        put_cfdesc(&s, spec);
        put_cfstr(&s, CFSTR("destinationImageBufferAttributes"));
        put_cfdesc(&s, destAttrs);
        emit(REC_SESSION_CREATE, s.b, s.n, NULL, 0);
    }
    return st;
}

// The decode entry avconferenced actually uses.
//
// Hooking the public VTDecompressionSessionDecodeFrame in avconferenced caught nothing across two
// sessions, including one whose session was created nine seconds in with video flowing. Sampling
// its stacks showed why:
//
//     _VideoReceiver_DequeueAndDecode
//       VCPDecompressionSessionDecodeFrame
//         VTDecompressionSessionDecodeFrameWithOptions      <- a DIFFERENT symbol
//           VideoDecoder_DecodeFrame
//
// So "zero decode calls" was our hook watching a function Apple does not call, not evidence that
// it decodes below the public API. The WithOptions variant is public as of macOS 15 and fully
// typed, so unlike the Create hook below this needs no argument guessing.
static int looks_like_cf(const void *p);

static OSStatus my_VTDecompressionSessionDecodeFrameWithOptions(
        VTDecompressionSessionRef session, CMSampleBufferRef sampleBuffer,
        VTDecodeFrameFlags decodeFlags, CFDictionaryRef frameOptions,
        void *sourceFrameRefCon, VTDecodeInfoFlags *infoFlagsOut) {

    if (R.running && sampleBuffer) {
        CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sampleBuffer);
        uint32_t fid = fmt_id_for(fmt);
        CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
        CMTime dts = CMSampleBufferGetDecodeTimeStamp(sampleBuffer);
        size_t len = 0;
        uint8_t *data = sample_bytes(sampleBuffer, &len);

        buf_t s = {0};
        put_f64(&s, now_sec() - R.t0);
        put_u64(&s, (uint64_t)(uintptr_t)session);
        put_i64(&s, pts.value);
        put_i32(&s, pts.timescale);
        put_i64(&s, dts.value);
        put_i32(&s, dts.timescale);
        put_u32(&s, (uint32_t)decodeFlags);
        put_u32(&s, sample_not_sync(sampleBuffer));
        put_u32(&s, fid);
        // Per-frame options. This is the one argument the first capture skipped, and it is the
        // only thing Apple passes per frame that we do not pass at all. Recorded as its CF
        // description so any key/value shows up without needing to know the schema.
        put_cfdesc(&s, frameOptions);
        put_u32(&s, (uint32_t)len);
        emit(REC_DECODE_INPUT, s.b, s.n, data, len);
    }

    return VTDecompressionSessionDecodeFrameWithOptions(
        session, sampleBuffer, decodeFlags, frameOptions, sourceFrameRefCon, infoFlagsOut);
}


// ---------------------------------------------------------------- decoder OUTPUT
//
// Everything so far captured what goes IN to Apple's decoder, and that input reconstructs to the
// same mosaic in ffmpeg and in VideoToolbox. Device Hub's window is clean from the same session.
// Both cannot be true unless the difference is at or after the decoder's output, and that is the
// one stage never observed.
//
// So substitute the output callback: record what Apple's decoder actually produced, then call
// their function with their refcon so the session behaves exactly as before.
//
// Opt-in via VTCAP_HOOK_OUTPUT=1. This runs inside a daemon that also serves FaceTime, and unlike
// every other hook here it does not merely observe -- it replaces a function pointer. If it goes
// wrong avconferenced dies and launchd restarts it, costing the mirror session and nothing else,
// but it is off unless asked for.
typedef void (*vt_output_fn)(void *refcon, void *srcFrameRefcon, OSStatus status,
                             VTDecodeInfoFlags flags, CVImageBufferRef image,
                             CMTime pts, CMTime dur);
typedef struct { vt_output_fn callback; void *refcon; } vt_cb_record;

static vt_output_fn g_real_output;
static void        *g_real_refcon;
static _Atomic int  g_out_seen;

// PNG writing is far too slow for a decode callback, so the picture is retained here and written
// by a background thread. Only a sample is kept: enough to look at, not enough to perturb.
// The picture is copied into the same ring the rest of the capture uses. The first attempt wrote
// PNGs from a background thread and produced nothing: CoreImage and ImageIO inside a launchd
// daemon fail quietly, and a daemon's stderr goes nowhere, so the failure was invisible. The ring
// had already shown it can write 16 MB from this process, so use what is known to work.
//
// Layout: a REC_DECODE_OUTPUT header, then one REC_NOTE per plane holding that plane's bytes.
static void my_output(void *refcon, void *srcFrameRefcon, OSStatus status,
                      VTDecodeInfoFlags flags, CVImageBufferRef image,
                      CMTime pts, CMTime dur) {
    int seq = atomic_fetch_add(&g_out_seen, 1);
    // Every 40th picture: enough to see what the decoder produced, rare enough that copying a
    // 1184x2576 frame never competes with decoding.
    if (R.running && image && status == noErr && (seq % 40) == 0) {
        CVPixelBufferRef pb = (CVPixelBufferRef)image;
        if (CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly) == kCVReturnSuccess) {
            size_t w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb);
            OSType fmt = CVPixelBufferGetPixelFormatType(pb);
            int planar = CVPixelBufferIsPlanar(pb);
            int planes = planar ? (int)CVPixelBufferGetPlaneCount(pb) : 1;
            buf_t s = {0};
            put_f64(&s, now_sec() - R.t0);
            put_u32(&s, (uint32_t)seq);
            put_u32(&s, (uint32_t)w);
            put_u32(&s, (uint32_t)h);
            put_u32(&s, (uint32_t)fmt);
            put_u32(&s, (uint32_t)planes);
            for (int i = 0; i < planes; i++) {
                put_u32(&s, (uint32_t)(planar ? CVPixelBufferGetBytesPerRowOfPlane(pb, i)
                                              : CVPixelBufferGetBytesPerRow(pb)));
                put_u32(&s, (uint32_t)(planar ? CVPixelBufferGetHeightOfPlane(pb, i) : h));
            }
            emit(REC_DECODE_OUTPUT, s.b, s.n, NULL, 0);
            for (int i = 0; i < planes; i++) {
                const void *base = planar ? CVPixelBufferGetBaseAddressOfPlane(pb, i)
                                          : CVPixelBufferGetBaseAddress(pb);
                size_t stride = planar ? CVPixelBufferGetBytesPerRowOfPlane(pb, i)
                                       : CVPixelBufferGetBytesPerRow(pb);
                size_t rows = planar ? CVPixelBufferGetHeightOfPlane(pb, i) : h;
                if (base) emit(REC_NOTE, base, stride * rows, NULL, 0);
            }
            CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
        }
    }
    if (g_real_output) g_real_output(g_real_refcon, srcFrameRefcon, status, flags, image, pts, dur);
}

static void start_output_writer(void) { /* output rides the main ring; nothing to start */ }


// VTDecompressionSessionCreateWithOptions is private and its signature is not
// published. Rather than assume an argument order, take eight pointer-sized
// arguments, record what each one actually IS (by CFGetTypeID, which is safe on
// a CF object and simply fails to match on anything else), and pass all eight
// through. On arm64 that is safe whether the real function takes six, seven or
// eight: surplus arguments sit unread in registers.
//
// The point of recording the options is narrow and specific. If avconference's
// decoder input matches ours byte for byte, the remaining difference has to be
// in how the decoder is set up — and this is that setup, next to ours in
// app/rPlayHub/VideoDecoder.swift.
extern OSStatus VTDecompressionSessionCreateWithOptions(
    void *, void *, void *, void *, void *, void *, void *, void *);

// Read through a pointer without risking a fault: vm_read_overwrite reports an
// error on an unmapped address instead of raising. Anything reaching this hook
// is an argument of unknown type, so nothing may be dereferenced until it has
// passed through here.
static int safe_read(const void *addr, void *out, size_t n) {
    if (!addr || ((uintptr_t)addr & 7)) return 0;
    mach_vm_size_t got = 0;
    kern_return_t kr = mach_vm_read_overwrite(mach_task_self(),
                                              (mach_vm_address_t)(uintptr_t)addr,
                                              (mach_vm_size_t)n,
                                              (mach_vm_address_t)(uintptr_t)out,
                                              &got);
    return kr == KERN_SUCCESS && got == n;
}

// True only if the pointer looks like a CoreFoundation-family object: readable,
// and its isa lands inside an image that vends such objects. CFGetTypeID on
// anything else would read an arbitrary class pointer and could crash, which
// would cost us the whole capture.
static int looks_readable(const void *p) {
    uint64_t w = 0;
    return safe_read(p, &w, sizeof w);
}

static int looks_like_cf(const void *p) {
    void *isa = NULL;
    if (!safe_read(p, &isa, sizeof isa)) return 0;
    // arm64e signs some isa pointers; strip the top bits before resolving.
    isa = (void *)((uintptr_t)isa & 0x0000007ffffffff8ull);
    Dl_info di;
    if (!dladdr(isa, &di) || !di.dli_fname) return 0;
    return strstr(di.dli_fname, "CoreFoundation") ||
           strstr(di.dli_fname, "CoreMedia") ||
           strstr(di.dli_fname, "VideoToolbox") ||
           strstr(di.dli_fname, "libobjc");
}

static void put_arg(buf_t *s, const char *label, void *arg) {
    uint32_t n = (uint32_t)strlen(label);
    put_u32(s, n);
    put(s, label, n);

    if (looks_like_cf(arg)) {
        CFTypeID id = CFGetTypeID((CFTypeRef)arg);
        if (id == CFDictionaryGetTypeID() || id == CFStringGetTypeID() ||
            id == CFArrayGetTypeID() || id == CFNumberGetTypeID() ||
            id == CFBooleanGetTypeID() || id == CMFormatDescriptionGetTypeID() ||
            id == CMSampleBufferGetTypeID()) {
            put_cfdesc(s, (CFTypeRef)arg);
            return;
        }
    }
    // Not a CF object, or not readable. Record the pointer and its first word,
    // which is enough to tell an out-parameter from a callback record offline.
    uint64_t word = 0;
    int readable = safe_read(arg, &word, sizeof word);
    char v[96];
    snprintf(v, sizeof v, "<ptr %p%s%016llx>", arg,
             readable ? " word=0x" : " unreadable ",
             (unsigned long long)word);
    uint32_t vn = (uint32_t)strlen(v);
    put_u32(s, vn);
    put(s, v, vn);
}

static OSStatus my_VTDecompressionSessionCreateWithOptions(
        void *a0, void *a1, void *a2, void *a3,
        void *a4, void *a5, void *a6, void *a7) {

    // Substitute the output callback before the session is created, so every decoded picture
    // passes through us. a4 is the VTDecompressionOutputCallbackRecord: a function pointer
    // followed by a refcon, which is what the read-only pass reported it to be.
    static vt_cb_record ours;
    const char *hook_out = getenv("VTCAP_HOOK_OUTPUT");
    if (hook_out && hook_out[0] == '1' && a4 && looks_readable(a4)) {
        vt_cb_record *rec = (vt_cb_record *)a4;
        if (rec->callback) {
            g_real_output = rec->callback;
            g_real_refcon = rec->refcon;
            ours.callback = my_output;
            ours.refcon   = NULL;
            a4 = &ours;
            start_output_writer();
        }
    }

    OSStatus st = VTDecompressionSessionCreateWithOptions(a0, a1, a2, a3, a4, a5, a6, a7);

    if (R.running) {
        buf_t s = {0};
        put_f64(&s, now_sec() - R.t0);
        // The out-parameter is whichever trailing argument now points at a
        // session; we do not guess, we record the arguments and let the
        // decoder script work it out.
        put_u64(&s, 0);
        put_u32(&s, (looks_like_cf(a1) &&
                     CFGetTypeID((CFTypeRef)a1) == CMFormatDescriptionGetTypeID())
                    ? fmt_id_for((CMFormatDescriptionRef)a1) : 0);
        put_u32(&s, 1);                       // which = CreateWithOptions
        put_u32(&s, 8);
        void *args[8] = {a0, a1, a2, a3, a4, a5, a6, a7};
        for (int i = 0; i < 8; i++) {
            char lbl[16];
            snprintf(lbl, sizeof lbl, "arg%d", i);
            put_arg(&s, lbl, args[i]);
        }
        emit(REC_SESSION_CREATE, s.b, s.n, NULL, 0);
    }
    return st;
}

// ---------------------------------------------------------------- interpose

#define INTERPOSE(new, old)                                                    \
    __attribute__((used, section("__DATA,__interpose")))                       \
    static const struct { const void *n; const void *o; }                      \
    _interpose_##old = { (const void *)(uintptr_t)&new,                        \
                         (const void *)(uintptr_t)&old };

INTERPOSE(my_VTDecompressionSessionDecodeFrame, VTDecompressionSessionDecodeFrame)
INTERPOSE(my_VTDecompressionSessionCreate, VTDecompressionSessionCreate)
INTERPOSE(my_VTDecompressionSessionCreateWithOptions, VTDecompressionSessionCreateWithOptions)
INTERPOSE(my_VTDecompressionSessionDecodeFrameWithOptions, VTDecompressionSessionDecodeFrameWithOptions)

// ---------------------------------------------------------------- lifecycle

static pthread_t writer;

static void finish(void) {
    pthread_mutex_lock(&R.lock);
    if (!R.running) { pthread_mutex_unlock(&R.lock); return; }
    R.running = 0;
    pthread_cond_signal(&R.wake);
    pthread_mutex_unlock(&R.lock);
    pthread_join(writer, NULL);
    fprintf(stderr, "[vtcapture] %llu records written, %llu dropped\n",
            (unsigned long long)atomic_load(&R.written),
            (unsigned long long)atomic_load(&R.dropped));
    close(R.fd);
}

__attribute__((constructor))
static void vtcapture_init(void) {
    const char *path = getenv("VTCAP_OUT");
    if (!path || !*path) return;

    // Only the process we are aiming at, so helper tools spawned by it do not
    // truncate the file out from under the capture.
    const char *want = getenv("VTCAP_PROC");
    if (want && *want) {
        char self[1024];
        uint32_t n = sizeof self;
        extern int _NSGetExecutablePath(char *, uint32_t *);
        if (_NSGetExecutablePath(self, &n) == 0 && !strstr(self, want)) return;
    }

    R.fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (R.fd < 0) {
        fprintf(stderr, "[vtcapture] cannot open %s: %s (errno %d)\n",
                path, strerror(errno), errno);
        return;
    }
    R.buf = malloc(RING_BYTES);
    if (!R.buf) { close(R.fd); return; }
    pthread_mutex_init(&R.lock, NULL);
    pthread_cond_init(&R.wake, NULL);
    R.t0 = now_sec();
    R.running = 1;
    write(R.fd, VTCAP_MAGIC, 4);
    pthread_create(&writer, NULL, writer_thread, NULL);
    atexit(finish);
    fprintf(stderr, "[vtcapture] armed -> %s\n", path);
}
