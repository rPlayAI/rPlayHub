#include "rp_media_offer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* ------------------------------------------------------------------ constants
 * All measured against Apple's own offer, not invented. */
#define DECODER_NAME            "Viceroy 1.7.0"
#define NEGOTIATOR_MODE_VIDEO   5
#define RES_ENTRY_CODEC_CAP_ID  50115
/* The feature-list string the device negotiates against.
 *
 * "FLS;SW:1;" is what Apple's own HEVC bank sends and is reproduced exactly. It carries no RVRA
 * token, so the encoder uses its default -- which is ON, and that is what makes the stream
 * undecodable by anything but AppleVideoDecoder: under motion it drops coded resolution and
 * predicts across the change, which standard HEVC cannot express. ffmpeg decodes the result
 * without a single warning and produces garbage.
 *
 * Apple's full string, recovered from AVConference's __cstring section, shows the knobs:
 *
 *   FLS;VRA:0;MVRA:0;RVRA1:1;AS:2;MS:-1;LTR;CABAC;CR:3;LF:-1;PR;CH1:4;CH:4;FA:5;
 *   AR:667/375,375/667;XR:3/2,2/3;
 *
 * alongside _VideoTransmitter_h264HwEncoderSupportsRVRA1 -- so RVRA1 is an encoder capability
 * negotiated here, not a decoder setting. Asking for RVRA1:0 should get a stream that any HEVC
 * decoder can handle, which is the whole cross-platform question.
 *
 * RPLAY_HEVC_FEATURES overrides it so that can be tested against the phone without a rebuild:
 *
 *   RPLAY_HEVC_FEATURES="FLS;VRA:0;MVRA:0;RVRA1:0;SW:1;" sudo ./host-c/cdhost
 *
 * Untested as of writing. If the device rejects the offer or ignores the token, the default is
 * unchanged and mirroring behaves exactly as before. */
#define HEVC_FEATURES_DEFAULT   "FLS;SW:1;"

static const char *hevc_features(void)
{
    const char *e = getenv("RPLAY_HEVC_FEATURES");
    const char *use = (e && *e) ? e : HEVC_FEATURES_DEFAULT;
    /* Print it once. An experiment that changes this string is worthless if there is no evidence
     * the string reached the offer -- a negative result would then be indistinguishable from the
     * override silently not applying. */
    static int said = 0;
    if (!said) { said = 1; fprintf(stderr, "[offer] HEVC features: %s\n", use); }
    return use;
}
#define AVC_FEATURES            "FLS;VRAE:0;SW:1;"
#define HEVC_PAYLOAD_TYPE       123
#define AVC_PAYLOAD_TYPE        100
#define HEVC_FLAGS              1
#define AVC_FLAGS               14
/* A fixed value lifted from a captured offer. The device does not appear to interpret it, but it
 * is reproduced exactly rather than zeroed, because "probably ignored" is not "ignored". */
#define CAPTURED_VIDEO_TIMESTAMP 17137042128614416384ULL

static const struct { uint32_t f1; uint64_t f2; uint32_t f3; int has_f3; } TIERS[] = {
    {4074, 0,          16384,   1}, {0, 75000000,  524288,  1},
    {0,    40000000,   12288,   1}, {16, 4100,     0,       0},
    {0,    20000000,   98304,   1}, {4,  6500,     0,       0},
    {0,    6000000,    131072,  1}, {0,  100000000, 1048576, 1},
    {0,    60000000,   262144,  1}, {1,  299,      0,       0},
};

/* ------------------------------------------------------------------ protobuf */

typedef struct { uint8_t *buf; size_t cap, len; int overflow; } pb;

static void pb_raw(pb *w, const void *p, size_t n)
{
    if (w->len + n > w->cap) { w->overflow = 1; return; }
    memcpy(w->buf + w->len, p, n);
    w->len += n;
}

static void pb_varint(pb *w, uint64_t v)
{
    uint8_t tmp[10];
    size_t n = 0;
    do { uint8_t b = v & 0x7F; v >>= 7; if (v) b |= 0x80; tmp[n++] = b; } while (v);
    pb_raw(w, tmp, n);
}

/* A varint padded to a fixed width with redundant continuation bits.
 *
 * The session id must occupy a constant number of bytes whatever its value, so the blob's length
 * does not change with it. Protobuf permits this: continuation bits set on otherwise-zero groups
 * decode to the same number. */
static void pb_varint_padded(pb *w, uint64_t v, int width)
{
    uint8_t tmp[16];
    int n = 0;
    do { tmp[n++] = (uint8_t)(v & 0x7F); v >>= 7; } while (v && n < width);
    while (n < width) tmp[n++] = 0;
    for (int i = 0; i < width - 1; i++) tmp[i] |= 0x80;
    pb_raw(w, tmp, (size_t)width);
}

static void pb_tag(pb *w, uint32_t field, uint32_t wire) { pb_varint(w, (field << 3) | wire); }
static void pb_fv(pb *w, uint32_t f, uint64_t v) { pb_tag(w, f, 0); pb_varint(w, v); }
static void pb_fb(pb *w, uint32_t f, const uint8_t *b, size_t n)
{
    pb_tag(w, f, 2); pb_varint(w, n); pb_raw(w, b, n);
}
static void pb_fs(pb *w, uint32_t f, const char *s) { pb_fb(w, f, (const uint8_t *)s, strlen(s)); }

/* ------------------------------------------------------------------ the blob */

static size_t res_entry(uint8_t *out, size_t cap, uint32_t pair_index)
{
    pb w = { out, cap, 0, 0 };
    pb_fv(&w, 1, 1);
    pb_fv(&w, 2, pair_index);
    pb_fv(&w, 3, RES_ENTRY_CODEC_CAP_ID);
    pb_fv(&w, 4, 0);
    return w.overflow ? 0 : w.len;
}

/* One VCMediaNegotiatorStreamGroupCodecConfiguration: payload type, resolution entries, the
 * feature string, and field 4 -- which is 1 for HEVC and 14 for AVC in Apple's own offer. */
static size_t codec_bank(uint8_t *out, size_t cap, uint32_t payload_type,
                         const char *features, uint32_t flags, int res_pairs)
{
    pb w = { out, cap, 0, 0 };
    pb_fv(&w, 1, payload_type);
    for (int i = 0; i < res_pairs; i++) {
        uint8_t e[32];
        size_t n = res_entry(e, sizeof e, 1 + (uint32_t)(i % 2));
        if (!n) return 0;
        pb_fb(&w, 2, e, n);
    }
    pb_fs(&w, 3, features);
    pb_fv(&w, 4, flags);
    return w.overflow ? 0 : w.len;
}

size_t rp_media_blob_video(const rp_offer_params *p, uint8_t *out, size_t cap)
{
    uint8_t hevc[256], avc[256], vs[1024];
    size_t nh = codec_bank(hevc, sizeof hevc, HEVC_PAYLOAD_TYPE, hevc_features(), HEVC_FLAGS, 4);
    size_t na = codec_bank(avc, sizeof avc, AVC_PAYLOAD_TYPE, AVC_FEATURES, AVC_FLAGS, 2);
    if (!nh || !na) return 0;

    /* VideoSettings (field 5). */
    pb v = { vs, sizeof vs, 0, 0 };
    pb_tag(&v, 1, 0);
    pb_varint_padded(&v, p->ssrc, 5);
    pb_fv(&v, 2, 0);
    if (p->codec == RP_OFFER_CODEC_AUTO || p->codec == RP_OFFER_CODEC_HEVC)
        pb_fb(&v, 3, hevc, nh);
    if (p->codec == RP_OFFER_CODEC_AUTO || p->codec == RP_OFFER_CODEC_H264)
        pb_fb(&v, 3, avc, na);
    /* Field 7 is ltrpEnabled and there is no field 10: we used to send one, Apple never does. */
    pb_fv(&v, 7, p->ltrp_enabled ? 1 : 0);
    pb_fv(&v, 8, 63);
    pb_fv(&v, 12, 1);
    if (v.overflow) return 0;

    pb w = { out, cap, 0, 0 };
    pb_fv(&w, 1, 1);
    pb_fv(&w, 2, 1);
    pb_fb(&w, 5, vs, v.len);
    pb_fs(&w, 6, DECODER_NAME);
    pb_fv(&w, 8, 0);
    for (size_t i = 0; i < sizeof TIERS / sizeof TIERS[0]; i++) {
        uint8_t t[64];
        pb tw = { t, sizeof t, 0, 0 };
        pb_fv(&tw, 1, TIERS[i].f1);
        pb_fv(&tw, 2, TIERS[i].f2);
        if (TIERS[i].has_f3) pb_fv(&tw, 3, TIERS[i].f3);
        if (tw.overflow) return 0;
        pb_fb(&w, 9, t, tw.len);
    }
    pb_fv(&w, 13, CAPTURED_VIDEO_TIMESTAMP);
    pb_fv(&w, 14, 2);
    pb_fv(&w, 16, 0);
    pb_fv(&w, 18, 1);
    return w.overflow ? 0 : w.len;
}

static size_t endpoint_info(const rp_offer_params *p, uint8_t *out, size_t cap)
{
    pb w = { out, cap, 0, 0 };
    pb_fv(&w, 1, 0);
    pb_fv(&w, 2, 1);
    pb_fs(&w, 3, p->model);
    pb_fs(&w, 4, p->os_version);
    pb_fs(&w, 5, p->build);
    return w.overflow ? 0 : w.len;
}

/* ------------------------------------------------------------------ binary plist
 *
 * Only what this one dictionary needs: four keys, ASCII strings, one small integer and two data
 * blobs. A general plist writer would be several times the size and none of it exercised.
 * Object order matches plistlib's, which writes keys first and then values, because the offer is
 * compared byte-for-byte against the Python during testing.
 */
typedef struct { uint8_t *buf; size_t cap, len; int overflow; size_t off[16]; int n; } bplist;

static void bp_raw(bplist *b, const void *p, size_t n)
{
    if (b->len + n > b->cap) { b->overflow = 1; return; }
    memcpy(b->buf + b->len, p, n);
    b->len += n;
}

static void bp_mark(bplist *b) { if (b->n < 16) b->off[b->n++] = b->len; }

static void bp_ascii(bplist *b, const char *s)
{
    size_t n = strlen(s);
    bp_mark(b);
    if (n < 15) { uint8_t m = 0x50 | (uint8_t)n; bp_raw(b, &m, 1); }
    else {
        uint8_t m = 0x5F, im = 0x10; bp_raw(b, &m, 1); bp_raw(b, &im, 1);
        uint8_t l = (uint8_t)n; bp_raw(b, &l, 1);
    }
    bp_raw(b, s, n);
}

static void bp_int(bplist *b, uint8_t v)
{
    bp_mark(b);
    uint8_t m = 0x10;                       /* integer, 1 byte */
    bp_raw(b, &m, 1);
    bp_raw(b, &v, 1);
}

static void bp_data(bplist *b, const uint8_t *d, size_t n)
{
    bp_mark(b);
    if (n < 15) { uint8_t m = 0x40 | (uint8_t)n; bp_raw(b, &m, 1); }
    else if (n < 256) {
        uint8_t m = 0x4F, im = 0x10, l = (uint8_t)n;
        bp_raw(b, &m, 1); bp_raw(b, &im, 1); bp_raw(b, &l, 1);
    } else {
        uint8_t m = 0x4F, im = 0x11;
        bp_raw(b, &m, 1); bp_raw(b, &im, 1);
        uint8_t l[2] = { (uint8_t)(n >> 8), (uint8_t)n };
        bp_raw(b, l, 2);
    }
    bp_raw(b, d, n);
}

size_t rp_build_offer(const rp_offer_params *p, uint8_t *out, size_t cap)
{
    uint8_t blob[4096], comp[4096], endp[128];
    size_t nblob = rp_media_blob_video(p, blob, sizeof blob);
    if (!nblob) return 0;

    /* Level 9 specifically. Other levels produce a valid deflate stream that the device
     * nonetheless refuses, which is the kind of thing that looks like a protocol bug. */
    uLongf clen = sizeof comp;
    if (compress2(comp, &clen, blob, (uLong)nblob, 9) != Z_OK) return 0;

    size_t nend = endpoint_info(p, endp, sizeof endp);
    if (!nend) return 0;

    /* Layout copied from plistlib, because the offer is compared byte-for-byte against it:
     * object 0 is the root dictionary, then the keys SORTED ALPHABETICALLY, then their values in
     * the same order. Writing the dictionary last, or in insertion order, produces a valid plist
     * that simply is not the same bytes -- and "valid but different" is not good enough for
     * something being diffed against a capture. */
    bplist b = { out, cap, 0, 0, {0}, 0 };
    bp_raw(&b, "bplist00", 8);

    bp_mark(&b);                             /* 0: the dictionary */
    uint8_t m = 0xD4;
    bp_raw(&b, &m, 1);
    uint8_t keyrefs[4] = {1, 2, 3, 4}, valrefs[4] = {5, 6, 7, 8};
    bp_raw(&b, keyrefs, 4);
    bp_raw(&b, valrefs, 4);

    bp_ascii(&b, "avcMediaStreamNegotiatorMediaBlob");        /* 1 */
    bp_ascii(&b, "avcMediaStreamNegotiatorMode");             /* 2 */
    bp_ascii(&b, "avcMediaStreamOptionCallID");               /* 3 */
    bp_ascii(&b, "avcMediaStreamOptionRemoteEndpointInfo");   /* 4 */
    bp_data(&b, comp, clen);                                  /* 5 */
    bp_int(&b, NEGOTIATOR_MODE_VIDEO);                        /* 6 */
    bp_ascii(&b, p->call_id);                                 /* 7 */
    bp_data(&b, endp, nend);                                  /* 8 */

    /* Offset entries are as wide as the largest offset needs. This blob runs past 255 bytes, so
     * assuming one byte silently truncates every offset after that point. */
    size_t table_off = b.len;
    int off_size = 1;
    for (int i = 0; i < b.n; i++) if (b.off[i] > 0xFF) { off_size = 2; break; }
    for (int i = 0; i < b.n; i++) {
        if (off_size == 1) { uint8_t o = (uint8_t)b.off[i]; bp_raw(&b, &o, 1); }
        else { uint8_t o[2] = { (uint8_t)(b.off[i] >> 8), (uint8_t)b.off[i] }; bp_raw(&b, o, 2); }
    }

    /* The trailer's three counters are 64-bit big-endian. Writing only the low byte works right
     * up until the offset table starts past 255 bytes in, which for this offer it does -- and the
     * result is a plist that differs from plistlib's by exactly one byte and is silently wrong. */
    uint8_t trailer[32];
    memset(trailer, 0, sizeof trailer);
    trailer[6] = (uint8_t)off_size;          /* offset table entry size */
    trailer[7] = 1;                          /* object reference size */
    for (int i = 0; i < 8; i++) {
        trailer[8  + i] = (uint8_t)((uint64_t)b.n      >> (8 * (7 - i)));  /* object count */
        trailer[16 + i] = 0;                 /* top object: the dictionary, written first */
        trailer[24 + i] = (uint8_t)((uint64_t)table_off >> (8 * (7 - i))); /* table start */
    }
    bp_raw(&b, trailer, sizeof trailer);

    return b.overflow ? 0 : b.len;
}
