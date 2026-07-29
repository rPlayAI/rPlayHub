/*
 * rp_rtp_assembler.c — see rp_rtp_assembler.h for provenance and licence.
 *
 * Ported from RTPReceiver::Source (the reorder queue and the declare-lost timer) and
 * RTPReceiver::H264Assembler (fragment reassembly), Apache 2.0, extended for HEVC.
 */
#include "rp_rtp_assembler.h"

#include <string.h>

#define HEVC_AP    48
#define HEVC_FU    49
#define AVC_STAP_A 24
#define AVC_FU_A   28

int rp_ra_is_rtcp(const uint8_t *pkt, size_t len)
{
    if (len < 2) return 1;
    unsigned pt = pkt[1] & 0x7F;
    return pt >= 64 && pt <= 95;
}

int rp_ra_init(rp_rtp_assembler *a, rp_ra_codec codec,
               rp_ra_packet *queue, int queue_cap,
               uint8_t *nal_storage, size_t nal_cap,
               rp_ra_nal_fn on_nal, rp_ra_frame_fn on_frame, void *ctx)
{
    if (!a || !queue || queue_cap < 8 || !nal_storage || nal_cap < 1024) return -1;
    memset(a, 0, sizeof *a);
    a->codec = codec;
    a->codec_locked = (codec != RP_RA_CODEC_AUTO);
    a->queue = queue;
    a->queue_cap = queue_cap;
    a->nal = nal_storage;
    a->nal_cap = nal_cap;
    a->on_nal = on_nal;
    a->on_frame = on_frame;
    a->ctx = ctx;
    memset(queue, 0, (size_t)queue_cap * sizeof *queue);
    return 0;
}

/* ---- fragment reassembly ------------------------------------------------------------------ */

/* Drop whatever is half-assembled. Called on a declared loss, and on any malformed packet.
 *
 * This is H264Assembler::signalDiscontinuity, and it is the reason this port exists: a fragment
 * that survives a gap gets welded to the fragment on the other side of it, producing a NAL that
 * decodes without complaint into garbage. */
static void reset_fragment(rp_rtp_assembler *a)
{
    a->nal_len = 0;
    a->in_fragment = 0;
}

static void emit_nal(rp_rtp_assembler *a, const uint8_t *nal, size_t len)
{
    if (len && a->on_nal) a->on_nal(a->ctx, nal, len);
}

static void append(rp_rtp_assembler *a, const uint8_t *p, size_t n)
{
    if (a->nal_len + n > a->nal_cap) {   /* too large to be real; abandon rather than truncate */
        a->malformed++;
        reset_fragment(a);
        return;
    }
    memcpy(a->nal + a->nal_len, p, n);
    a->nal_len += n;
}

static void sniff_codec(rp_rtp_assembler *a, const uint8_t *p, size_t n)
{
    if (a->codec_locked || n < 2) return;
    /* The payload STRUCTURE decides this, never the payload type number: the device assigns
     * payload numbers in its answer and they need not match the banks we offered. A live HEVC
     * session was measured arriving on payload type 100, the number our own offer uses for its
     * H.264 bank. */
    int hevc = (p[0] >> 1) & 0x3F;
    int avc  = p[0] & 0x1F;
    if (hevc == HEVC_AP || hevc == HEVC_FU) { a->codec = RP_RA_CODEC_HEVC; a->codec_locked = 1; }
    else if (avc == AVC_STAP_A || avc == AVC_FU_A) { a->codec = RP_RA_CODEC_H264; a->codec_locked = 1; }
}

/* Returns 0 on success, -1 if the packet was malformed (which resets the fragment). */
static int assemble(rp_rtp_assembler *a, const uint8_t *p, size_t n)
{
    if (n < 1) return -1;
    sniff_codec(a, p, n);

    if (a->codec == RP_RA_CODEC_HEVC) {
        if (n < 2) return -1;
        int type = (p[0] >> 1) & 0x3F;

        if (type == HEVC_AP) {
            size_t i = 2;
            while (i + 2 <= n) {
                size_t size = ((size_t)p[i] << 8) | p[i + 1];
                i += 2;
                if (i + size > n) return -1;      /* declared length exceeds the packet */
                emit_nal(a, p + i, size);
                i += size;
            }
            return i == n ? 0 : -1;
        }

        if (type == HEVC_FU) {
            if (n < 3) return -1;
            uint8_t fuh = p[2];
            if (fuh & 0x80) {                      /* start */
                reset_fragment(a);
                uint8_t hdr[2];
                hdr[0] = (uint8_t)((p[0] & 0x81) | ((fuh & 0x3F) << 1));
                hdr[1] = p[1];
                append(a, hdr, 2);
                a->fu_indicator = p[0];
                a->fu_type = (uint8_t)(fuh & 0x3F);
                a->in_fragment = 1;
            } else {
                /* Continuation. It must continue the unit we are actually holding: same layer
                 * and temporal id, same NAL type, no start bit. The shipping receiver treats any
                 * disagreement as malformed and discards the whole unit. */
                if (!a->in_fragment) return 0;     /* nothing to continue; silently ignore */
                if (p[0] != a->fu_indicator || (fuh & 0x3F) != a->fu_type) return -1;
            }
            append(a, p + 3, n - 3);
            if ((fuh & 0x40) && a->in_fragment && a->nal_len) {   /* end */
                emit_nal(a, a->nal, a->nal_len);
                reset_fragment(a);
            }
            return 0;
        }

        emit_nal(a, p, n);                          /* single NAL */
        return 0;
    }

    /* H.264 */
    int type = p[0] & 0x1F;
    if (type == AVC_STAP_A) {
        size_t i = 1;
        while (i + 2 <= n) {
            size_t size = ((size_t)p[i] << 8) | p[i + 1];
            i += 2;
            if (i + size > n) return -1;
            emit_nal(a, p + i, size);
            i += size;
        }
        return i == n ? 0 : -1;
    }
    if (type == AVC_FU_A) {
        if (n < 2) return -1;
        uint8_t fuh = p[1];
        if (fuh & 0x80) {
            reset_fragment(a);
            uint8_t hdr = (uint8_t)((fuh & 0x1F) | (p[0] & 0xE0));
            append(a, &hdr, 1);
            a->fu_indicator = p[0];
            a->fu_type = (uint8_t)(fuh & 0x1F);
            a->in_fragment = 1;
        } else {
            if (!a->in_fragment) return 0;
            if (p[0] != a->fu_indicator || (fuh & 0x1F) != a->fu_type) return -1;
        }
        append(a, p + 2, n - 2);
        if ((fuh & 0x40) && a->in_fragment && a->nal_len) {
            emit_nal(a, a->nal, a->nal_len);
            reset_fragment(a);
        }
        return 0;
    }
    emit_nal(a, p, n);
    return 0;
}

/* ---- reorder queue ------------------------------------------------------------------------ */

static void deliver(rp_rtp_assembler *a, rp_ra_packet *pk)
{
    if (assemble(a, pk->data, pk->len) != 0) {
        a->malformed++;
        reset_fragment(a);
    }
    if (pk->marker) {
        a->frames++;
        if (a->on_frame) a->on_frame(a->ctx);
    }
}

/* Emit every packet sitting at the head of the queue in unbroken sequence. */
static void drain(rp_rtp_assembler *a)
{
    while (a->queue_len > 0 && a->queue[0].ext_seq == a->awaiting) {
        deliver(a, &a->queue[0]);
        memmove(a->queue, a->queue + 1, (size_t)(a->queue_len - 1) * sizeof a->queue[0]);
        a->queue_len--;
        a->awaiting++;
    }
}

/* The missing packet is not coming. Step over it, and raise a discontinuity so no fragment
 * spans the gap — this is RTPReceiver::Source's kWhatDeclareLost. */
static void declare_lost(rp_rtp_assembler *a)
{
    a->lost++;
    a->discontinuities++;
    reset_fragment(a);
    a->awaiting++;
    drain(a);
}

void rp_ra_tick(rp_rtp_assembler *a, uint64_t now_us)
{
    if (!a->have_awaiting) return;
    /* Only the head of the queue can be waiting on a gap; anything behind it is waiting on the
     * head. Give the missing packet its full deadline measured from when its successor landed. */
    while (a->queue_len > 0 && a->queue[0].ext_seq != a->awaiting) {
        if (now_us < a->queue[0].arrived_us + RP_RA_LOST_AFTER_US) break;
        declare_lost(a);
    }
}

void rp_ra_feed(rp_rtp_assembler *a, const uint8_t *pkt, size_t len, uint64_t now_us)
{
    if (len < 12 || rp_ra_is_rtcp(pkt, len)) return;

    uint16_t seq = (uint16_t)((pkt[2] << 8) | pkt[3]);
    uint32_t rtp_time = ((uint32_t)pkt[4] << 24) | ((uint32_t)pkt[5] << 16) |
                        ((uint32_t)pkt[6] << 8) | pkt[7];
    int marker = (pkt[1] >> 7) & 1;

    /* Header, CSRC list, extension, and trailing padding are all outside the payload. Padding in
     * particular was missed by the previous implementation, and its bytes were copied into the
     * NAL as though they were video. */
    size_t off = 12 + 4u * (size_t)(pkt[0] & 0x0F);
    if (pkt[0] & 0x10) {
        if (off + 4 > len) return;
        off += 4 + 4 * (((size_t)pkt[off + 2] << 8) | pkt[off + 3]);
    }
    size_t end = len;
    if (pkt[0] & 0x20) {
        size_t pad = pkt[len - 1];
        if (pad == 0 || pad + off > end) return;
        end -= pad;
    }
    if (off >= end || end - off > RP_RA_MAX_PAYLOAD) return;
    size_t plen = end - off;

    /* Extend the 16-bit sequence number across wraps. */
    if (!a->have_max_seq) { a->max_seq = seq; a->have_max_seq = 1; }
    else if ((uint16_t)(seq - a->max_seq) < 0x8000u) {
        if (seq < a->max_seq) a->cycles += 0x10000u;
        a->max_seq = seq;
    } else if ((uint16_t)(a->max_seq - seq) > 0x8000u) {
        a->cycles += 0x10000u;
        a->max_seq = seq;
    }
    int32_t ext = (int32_t)(a->cycles | seq);

    if (!a->have_awaiting) { a->awaiting = ext; a->have_awaiting = 1; }

    /* Older than what we have already emitted: it arrived, but too late to be of any use. Counted
     * apart from loss, because reading loss alone would call this a clean stream. */
    if (ext < a->awaiting) { a->late++; return; }

    int i = 0;
    while (i < a->queue_len && a->queue[i].ext_seq < ext) i++;
    if (i < a->queue_len && a->queue[i].ext_seq == ext) { a->duplicates++; return; }

    if (a->queue_len == a->queue_cap) {
        /* Full. The head is the packet everything is waiting for and it is not coming. */
        declare_lost(a);
        while (i > 0 && (i > a->queue_len || a->queue[i - 1].ext_seq >= ext)) i--;
        if (a->queue_len == a->queue_cap) return;   /* still no room; drop rather than corrupt */
        if (i > a->queue_len) i = a->queue_len;
    }

    memmove(a->queue + i + 1, a->queue + i, (size_t)(a->queue_len - i) * sizeof a->queue[0]);
    rp_ra_packet *pk = &a->queue[i];
    pk->ext_seq = ext;
    pk->rtp_time = rtp_time;
    pk->arrived_us = now_us;
    pk->marker = (uint8_t)marker;
    pk->len = (uint16_t)plen;
    memcpy(pk->data, pkt + off, plen);
    a->queue_len++;
    a->received++;

    drain(a);
    rp_ra_tick(a, now_us);
}
