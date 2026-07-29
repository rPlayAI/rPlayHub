/*
 * rp_rtp_assembler.h — RTP reassembly, ported from the Miracast receiver.
 *
 * Original: frameworks/av/media/libstagefright/wifi-display/{source,sink}/RTPReceiver.{h,cpp}
 *           and RTPAssembler.cpp, Copyright 2013 The Android Open Source Project,
 *           Apache License 2.0.
 * Adapted from the DisplayNote Miracast receiver (reference/DisplayNoteMiracastReceiver),
 * which is the licensed, shipping derivative of that code, and extended here for HEVC.
 *
 * Vendored as source rather than taken as a dependency, the same way ts_packetizer.cc was
 * vendored into Cafari — where adopting this same lineage removed audio glitches that had
 * survived every fix attempted against the local implementation.
 *
 * It differs from the hand-written reassembly in rp_rtp.c in three ways that matter:
 *
 *   1. A gap is waited out in TIME, not in buffer space. A missing packet is given
 *      RP_RA_LOST_AFTER_US (100 ms, the value the shipping receiver uses) to show up before the
 *      stream steps over it. rp_rtp.c waits for its reorder window to FILL instead, which at this
 *      device's bitrate is a few milliseconds — so a packet merely reordered by the tunnel is
 *      abandoned as lost, and its data discarded, while every counter still reads zero loss.
 *
 *   2. Declaring a packet lost raises a discontinuity, which resets the assembler. Fragments
 *      therefore never span a gap. Without this, the fragment after a hole is appended to the
 *      fragment before it and the result is a NAL that is structurally valid and silently wrong —
 *      indistinguishable downstream from a good frame.
 *
 *   3. An access unit ends when the RTP marker bit says so, and only then.
 *
 * The caller owns the socket and the clock: feed it packets and tell it the time. It performs no
 * I/O, allocates nothing, and is therefore testable against a capture with no device present.
 */
#ifndef RP_RTP_ASSEMBLER_H
#define RP_RTP_ASSEMBLER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How long a gap is given before the missing packet is declared lost and the stream moves on.
 * 100 ms is the shipping value. It is a latency/quality trade: too short and reordered packets
 * are thrown away, too long and a real loss stalls the picture. */
#define RP_RA_LOST_AFTER_US   100000

/* Packets held while waiting for a gap to fill. At ~3 Mbit/s this is far more than 100 ms worth,
 * so the timer above is what decides, which is the point. */
#define RP_RA_MAX_PACKETS     256
#define RP_RA_MAX_PAYLOAD     1500
/* Largest access unit we will reassemble. */
#define RP_RA_MAX_NAL         (4u * 1024u * 1024u)

typedef enum {
    RP_RA_CODEC_AUTO = 0,
    RP_RA_CODEC_HEVC,
    RP_RA_CODEC_H264,
} rp_ra_codec;

/* One complete NAL, without a start code, in decode order. */
typedef void (*rp_ra_nal_fn)(void *ctx, const uint8_t *nal, size_t len);
/* The access unit just delivered is complete (RTP marker bit). */
typedef void (*rp_ra_frame_fn)(void *ctx);

typedef struct {
    int32_t  ext_seq;                 /* extended sequence number */
    uint32_t rtp_time;
    uint64_t arrived_us;
    uint16_t len;
    uint8_t  marker;
    uint8_t  used;
    uint8_t  data[RP_RA_MAX_PAYLOAD];
} rp_ra_packet;

typedef struct {
    rp_ra_codec codec;
    int codec_locked;

    rp_ra_nal_fn   on_nal;
    rp_ra_frame_fn on_frame;
    void *ctx;

    /* Reorder queue, kept sorted by extended sequence number. */
    rp_ra_packet *queue;
    int queue_cap;
    int queue_len;

    int32_t  awaiting;                /* next extended sequence number to emit */
    int      have_awaiting;
    uint32_t cycles;                  /* high half of the extended sequence number */
    uint16_t max_seq;
    int      have_max_seq;

    /* Fragment in flight. */
    uint8_t *nal;
    size_t   nal_cap;
    size_t   nal_len;
    int      in_fragment;
    uint8_t  fu_indicator, fu_type;

    /* Counters. Every one of these is a packet or unit that was NOT delivered intact, and they
     * are kept apart because they mean different things: `lost` never arrived, `late` arrived
     * after we gave up on it, `malformed` arrived and did not add up. */
    uint64_t received, lost, late, duplicates, malformed, discontinuities, frames;
} rp_rtp_assembler;

/* `queue` must hold at least `queue_cap` entries; `nal_storage` at least `nal_cap` bytes.
 * Storage is caller-provided so this file never allocates. */
int rp_ra_init(rp_rtp_assembler *a, rp_ra_codec codec,
               rp_ra_packet *queue, int queue_cap,
               uint8_t *nal_storage, size_t nal_cap,
               rp_ra_nal_fn on_nal, rp_ra_frame_fn on_frame, void *ctx);

/* Feed one UDP datagram. RTCP is ignored here; test with rp_ra_is_rtcp first if you care. */
void rp_ra_feed(rp_rtp_assembler *a, const uint8_t *pkt, size_t len, uint64_t now_us);

/* Call regularly (a receive timeout is the natural place). This is what enforces the 100 ms
 * deadline when no further packets arrive to drive it — without it, a gap at the end of a burst
 * would hold the picture until the next packet, however long that takes. */
void rp_ra_tick(rp_rtp_assembler *a, uint64_t now_us);

/* True for a packet whose payload type, after masking, is in the RTCP range (RFC 5761). */
int rp_ra_is_rtcp(const uint8_t *pkt, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* RP_RTP_ASSEMBLER_H */
