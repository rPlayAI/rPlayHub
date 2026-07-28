/*
 * rp_video_stream — see rp_video_stream.h.
 *
 * Design notes worth knowing before changing anything here:
 *
 *  - NAL bytes are copied into an access-unit buffer rather than pointed at inside the input
 *    buffer. The input buffer is compacted and reallocated as bytes arrive, which would leave
 *    any pointers into it dangling.
 *  - A picture boundary is detected from the first-slice flag: HEVC's
 *    first_slice_segment_in_pic_flag and H.264's first_mb_in_slice == 0 (ue(v), so a leading 1
 *    bit) are both the top bit of the byte following the NAL header.
 *  - Frames are discarded until a keyframe arrives. Feeding a decoder unanchored P-frames
 *    produces a black picture with no error reported, which is far harder to diagnose than a
 *    deliberate, counted drop.
 */
#include "rp_video_stream.h"

#include <stdlib.h>
#include <string.h>

#define RP_MAX_AU_NALS 64
#define RP_MAX_PARAM_SETS 3

typedef struct {
    uint8_t *data;
    size_t   size;
    size_t   cap;
} rp_buf;

typedef struct {
    size_t off;
    size_t size;
    int    type;
} rp_au_entry;

typedef struct {
    int    type;
    rp_buf buf;
} rp_param_set;

struct rp_video_stream {
    rp_codec codec;

    rp_buf in;                                  /* bytes not yet split into NALs */
    rp_buf au;                                  /* current access unit, packed back to back */
    rp_au_entry au_nals[RP_MAX_AU_NALS];
    size_t      au_count;
    int         au_is_keyframe;

    rp_param_set params[RP_MAX_PARAM_SETS];
    size_t       param_count;
    int          params_dirty;

    int awaiting_keyframe;

    void              *ctx;
    rp_params_fn       on_params;
    rp_access_unit_fn  on_access_unit;

    rp_video_stats stats;
};

/* ------------------------------------------------------------------ buffers */

static int buf_reserve(rp_buf *b, size_t need)
{
    if (b->cap >= need) return 1;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < need) cap *= 2;
    uint8_t *p = (uint8_t *)realloc(b->data, cap);
    if (!p) return 0;
    b->data = p;
    b->cap = cap;
    return 1;
}

static int buf_append(rp_buf *b, const uint8_t *bytes, size_t n)
{
    if (!n) return 1;
    if (!buf_reserve(b, b->size + n)) return 0;
    memcpy(b->data + b->size, bytes, n);
    b->size += n;
    return 1;
}

static void buf_free(rp_buf *b)
{
    free(b->data);
    b->data = NULL;
    b->size = b->cap = 0;
}

/* ------------------------------------------------------------ classification */

int rp_nal_type(const uint8_t *nal, size_t size, rp_codec codec)
{
    if (!nal || size < 1) return -1;
    return codec == RP_CODEC_H264 ? (nal[0] & 0x1F) : ((nal[0] >> 1) & 0x3F);
}

int rp_nal_is_parameter_set(int type, rp_codec codec)
{
    if (codec == RP_CODEC_H264) return type == 7 || type == 8;
    return type >= 32 && type <= 34;            /* VPS, SPS, PPS */
}

int rp_nal_is_keyframe(int type, rp_codec codec)
{
    if (codec == RP_CODEC_H264) return type == 5;            /* IDR */
    return type >= 16 && type <= 23;                          /* BLA / IDR / CRA */
}

int rp_nal_is_vcl(int type, rp_codec codec)
{
    if (codec == RP_CODEC_H264) return type >= 1 && type <= 5;
    return type >= 0 && type < 32;
}

static size_t header_len(rp_codec codec)
{
    return codec == RP_CODEC_H264 ? 1u : 2u;
}

/* ------------------------------------------------------------------ packing */

size_t rp_pack_length_prefixed(const rp_nal *nals, size_t count, uint8_t *out, size_t out_size)
{
    size_t need = 0;
    for (size_t i = 0; i < count; i++) need += 4 + nals[i].size;
    if (!out) return need;
    if (out_size < need) return 0;
    size_t off = 0;
    for (size_t i = 0; i < count; i++) {
        uint32_t n = (uint32_t)nals[i].size;
        out[off++] = (uint8_t)(n >> 24);
        out[off++] = (uint8_t)(n >> 16);
        out[off++] = (uint8_t)(n >> 8);
        out[off++] = (uint8_t)n;
        memcpy(out + off, nals[i].data, nals[i].size);
        off += nals[i].size;
    }
    return off;
}

size_t rp_pack_annexb(const rp_nal *nals, size_t count, uint8_t *out, size_t out_size)
{
    size_t need = 0;
    for (size_t i = 0; i < count; i++) need += 4 + nals[i].size;
    if (!out) return need;
    if (out_size < need) return 0;
    size_t off = 0;
    for (size_t i = 0; i < count; i++) {
        out[off++] = 0; out[off++] = 0; out[off++] = 0; out[off++] = 1;
        memcpy(out + off, nals[i].data, nals[i].size);
        off += nals[i].size;
    }
    return off;
}

/* ------------------------------------------------------------------ lifetime */

rp_video_stream *rp_video_stream_create(rp_codec codec)
{
    rp_video_stream *s = (rp_video_stream *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->codec = codec;
    s->awaiting_keyframe = 1;
    return s;
}

void rp_video_stream_destroy(rp_video_stream *s)
{
    if (!s) return;
    buf_free(&s->in);
    buf_free(&s->au);
    for (size_t i = 0; i < s->param_count; i++) buf_free(&s->params[i].buf);
    free(s);
}

static void reset_decode_state(rp_video_stream *s)
{
    s->au.size = 0;
    s->au_count = 0;
    s->au_is_keyframe = 0;
    s->awaiting_keyframe = 1;
    for (size_t i = 0; i < s->param_count; i++) buf_free(&s->params[i].buf);
    s->param_count = 0;
    s->params_dirty = 0;
}

void rp_video_stream_set_codec(rp_video_stream *s, rp_codec codec)
{
    if (!s || s->codec == codec) return;
    s->codec = codec;
    reset_decode_state(s);
}

rp_codec rp_video_stream_codec(const rp_video_stream *s)
{
    return s ? s->codec : RP_CODEC_HEVC;
}

void rp_video_stream_set_callbacks(rp_video_stream *s, void *ctx,
                                   rp_params_fn on_params,
                                   rp_access_unit_fn on_access_unit)
{
    if (!s) return;
    s->ctx = ctx;
    s->on_params = on_params;
    s->on_access_unit = on_access_unit;
}

void rp_video_stream_await_keyframe(rp_video_stream *s)
{
    if (s) s->awaiting_keyframe = 1;
}

int rp_video_stream_is_awaiting_keyframe(const rp_video_stream *s)
{
    return s ? s->awaiting_keyframe : 1;
}

rp_video_stats rp_video_stream_stats(const rp_video_stream *s)
{
    rp_video_stats empty;
    memset(&empty, 0, sizeof empty);
    return s ? s->stats : empty;
}

/* ------------------------------------------------------- parameter set cache */

static rp_param_set *find_param(rp_video_stream *s, int type)
{
    for (size_t i = 0; i < s->param_count; i++)
        if (s->params[i].type == type) return &s->params[i];
    return NULL;
}

static void store_param(rp_video_stream *s, int type, const uint8_t *nal, size_t size)
{
    rp_param_set *p = find_param(s, type);
    if (!p) {
        if (s->param_count >= RP_MAX_PARAM_SETS) return;
        p = &s->params[s->param_count++];
        p->type = type;
        memset(&p->buf, 0, sizeof p->buf);
    }
    if (p->buf.size == size && p->buf.data && memcmp(p->buf.data, nal, size) == 0)
        return;                                  /* unchanged — do not re-announce */
    p->buf.size = 0;
    if (buf_append(&p->buf, nal, size)) s->params_dirty = 1;
}

static void announce_params_if_complete(rp_video_stream *s)
{
    if (!s->params_dirty || !s->on_params) return;

    static const int hevc_order[3] = { 32, 33, 34 };
    static const int h264_order[2] = { 7, 8 };
    const int *order = s->codec == RP_CODEC_H264 ? h264_order : hevc_order;
    size_t want = s->codec == RP_CODEC_H264 ? 2u : 3u;

    rp_nal sets[RP_MAX_PARAM_SETS];
    for (size_t i = 0; i < want; i++) {
        rp_param_set *p = find_param(s, order[i]);
        if (!p || !p->buf.size) return;          /* incomplete — wait */
        sets[i].data = p->buf.data;
        sets[i].size = p->buf.size;
        sets[i].type = p->type;
    }
    s->params_dirty = 0;
    s->on_params(s->ctx, sets, want, s->codec);
}

/* --------------------------------------------------------- access units */

static void emit_access_unit(rp_video_stream *s)
{
    if (!s->au_count) return;

    if (s->on_access_unit) {
        rp_nal nals[RP_MAX_AU_NALS];
        for (size_t i = 0; i < s->au_count; i++) {
            nals[i].data = s->au.data + s->au_nals[i].off;
            nals[i].size = s->au_nals[i].size;
            nals[i].type = s->au_nals[i].type;
        }
        s->on_access_unit(s->ctx, nals, s->au_count, s->au_is_keyframe);
    }
    s->stats.access_units++;
    if (s->au_is_keyframe) s->stats.keyframes++;

    s->au.size = 0;
    s->au_count = 0;
    s->au_is_keyframe = 0;
}

static void handle_nal(rp_video_stream *s, const uint8_t *nal, size_t size)
{
    if (size <= header_len(s->codec)) return;
    s->stats.nals_seen++;

    int type = rp_nal_type(nal, size, s->codec);

    if (rp_nal_is_parameter_set(type, s->codec)) {
        emit_access_unit(s);
        store_param(s, type, nal, size);
        announce_params_if_complete(s);
        return;
    }
    if (!rp_nal_is_vcl(type, s->codec))
        return;                                  /* SEI, AUD, end-of-sequence */

    /* A new picture begins at its first slice. */
    size_t flag = header_len(s->codec);
    if (flag < size && (nal[flag] & 0x80)) emit_access_unit(s);

    if (rp_nal_is_keyframe(type, s->codec)) {
        s->awaiting_keyframe = 0;
        s->au_is_keyframe = 1;
    } else if (s->awaiting_keyframe) {
        s->stats.frames_before_keyframe++;
        return;
    }

    if (s->au_count >= RP_MAX_AU_NALS) {
        /* Pathological slice count: emit what we have rather than dropping the picture. */
        emit_access_unit(s);
    }
    size_t off = s->au.size;
    if (!buf_append(&s->au, nal, size)) return;
    s->au_nals[s->au_count].off = off;
    s->au_nals[s->au_count].size = size;
    s->au_nals[s->au_count].type = type;
    s->au_count++;
}

/* ------------------------------------------------------------------- feeding */

/* Find the next start code at or after `from`. Returns its offset, or SIZE_MAX. */
static size_t next_start_code(const uint8_t *b, size_t n, size_t from, size_t *code_len)
{
    for (size_t i = from; i + 3 <= n; i++) {
        if (b[i] || b[i + 1]) continue;
        if (b[i + 2] == 1) { *code_len = 3; return i; }
        if (i + 4 <= n && b[i + 2] == 0 && b[i + 3] == 1) { *code_len = 4; return i; }
    }
    return SIZE_MAX;
}

void rp_video_stream_feed(rp_video_stream *s, const uint8_t *bytes, size_t n)
{
    if (!s || !bytes || !n) return;
    if (!buf_append(&s->in, bytes, n)) return;
    s->stats.bytes_in += n;

    const uint8_t *b = s->in.data;
    size_t size = s->in.size;

    size_t code_len = 0;
    size_t start = next_start_code(b, size, 0, &code_len);
    if (start == SIZE_MAX) {
        /* No start code yet. Keep a short tail so one split across reads still matches. */
        if (size > 4) {
            memmove(s->in.data, s->in.data + size - 3, 3);
            s->in.size = 3;
        }
        return;
    }

    size_t nal_start = start + code_len;
    for (;;) {
        size_t next_len = 0;
        size_t next = next_start_code(b, size, nal_start, &next_len);
        if (next == SIZE_MAX) break;             /* this NAL is still incomplete */

        size_t end = next;
        while (end > nal_start && b[end - 1] == 0) end--;   /* trailing_zero_8bits */
        if (end > nal_start) handle_nal(s, b + nal_start, end - nal_start);

        start = next;
        code_len = next_len;
        nal_start = next + next_len;
    }

    /* Retain from the last start code onward; it is an unfinished NAL. */
    size_t keep_from = start;
    if (keep_from > 0) {
        memmove(s->in.data, s->in.data + keep_from, size - keep_from);
        s->in.size = size - keep_from;
    }
}

void rp_video_stream_flush(rp_video_stream *s)
{
    if (!s) return;
    /* Whatever remains is one final NAL, if it is more than a bare start code. */
    if (s->in.size > 4) {
        size_t code_len = 0;
        size_t start = next_start_code(s->in.data, s->in.size, 0, &code_len);
        if (start != SIZE_MAX) {
            size_t nal_start = start + code_len;
            size_t end = s->in.size;
            while (end > nal_start && s->in.data[end - 1] == 0) end--;
            if (end > nal_start) handle_nal(s, s->in.data + nal_start, end - nal_start);
        }
        s->in.size = 0;
    }
    emit_access_unit(s);
}
