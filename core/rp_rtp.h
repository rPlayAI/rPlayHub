/*
 * rp_rtp.h — RTP in, Annex-B NAL units out.
 *
 * Three jobs that have to happen in this order: strip the RTP header, put packets back into
 * sequence, and reassemble the fragments into NAL units.
 *
 * Each has a trap that cost real time in the Python:
 *
 *  - RTCP shares the port with RTP. Its packet-type byte is 200-204, but the marker bit has
 *    already been masked off by the time you look, so it arrives as 72-76. Comparing against
 *    200-204 never matches and feeds every RTCP packet into the video depacketizer.
 *  - Every packet from this device carries an extension header. Skipping it wrongly shifts the
 *    payload and nothing parses.
 *  - The codec cannot be inferred from the payload type. This device sends HEVC under payload
 *    type 100, which is the number we advertise for AVC. Trusting the number switched the
 *    depacketizer to H.264 and shredded a working stream; the payload structure is the only
 *    reliable signal.
 *
 * Verified offline against a real captured session: fed the packets from Apple's own Device Hub
 * capture, this produces a byte-identical Annex-B stream to the reference.
 */
#ifndef RP_RTP_H
#define RP_RTP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { RP_RTP_CODEC_HEVC = 0, RP_RTP_CODEC_H264 = 1 } rp_rtp_codec;

/* Emitted for each complete NAL unit, without a start code. */
typedef void (*rp_rtp_nal_fn)(void *ctx, const uint8_t *nal, size_t len);

#define RP_RTP_REORDER_WINDOW 256
#define RP_RTP_MAX_NAL        (1u << 21)   /* a keyframe at this resolution is ~120 kB */

typedef struct {
    rp_rtp_codec codec;
    int          codec_locked;      /* set once the payload structure has told us */

    /* Reordering. RTP arrives out of order often enough that depacketizing in arrival order
     * corrupts fragmented frames. */
    struct {
        uint16_t seq;
        uint16_t len;
        uint8_t *data;
    } slots[RP_RTP_REORDER_WINDOW];
    int      have_next;
    uint16_t next_seq;
    int      buffered;

    /* Fragment accumulator for the NAL currently being reassembled. */
    uint8_t *fu;
    size_t   fu_len, fu_cap;

    rp_rtp_nal_fn on_nal;
    void         *ctx;

    /* Counters, which are how "the device is sending a poor stream" is told apart from "the
     * network is dropping packets" -- two problems with completely different fixes. */
    uint64_t received, lost, late, duplicates;
    /* Frames whose declared length disagreed with the bytes actually present, and fragmented
     * NALs abandoned because they exceeded the reassembly buffer. Both used to be silent. */
    uint64_t malformed, truncated;
    /* Identity of the fragmented NAL in flight, so continuations can be checked against it. */
    uint8_t fu_indicator, fu_type;
} rp_rtp_session;

/* `storage` must hold RP_RTP_REORDER_WINDOW * 1500 bytes plus RP_RTP_MAX_NAL, and outlive the
 * session. Nothing here allocates. */
int  rp_rtp_init(rp_rtp_session *s, rp_rtp_codec codec, uint8_t *storage, size_t storage_len,
                 rp_rtp_nal_fn on_nal, void *ctx);

/* Feed one datagram exactly as it came off the socket. RTCP is recognised and ignored here;
 * the caller handles it separately. Returns 1 if the packet was video, 0 if it was RTCP. */
int  rp_rtp_feed(rp_rtp_session *s, const uint8_t *pkt, size_t len);

/* Release anything still held, at end of stream. */
void rp_rtp_flush(rp_rtp_session *s);

/* True when the packet is RTCP rather than RTP, using the range RFC 5761 reserves. */
int  rp_rtp_is_rtcp(const uint8_t *pkt, size_t len);

/* Which codec the payload structure indicates, or -1 if inconclusive. */
int  rp_rtp_sniff_codec(const uint8_t *payload, size_t len);

/* Header fields the caller needs: the marker bit ends a frame, and the timestamp identifies it
 * for LTR acknowledgement. Returns 0 on success. */
int  rp_rtp_header(const uint8_t *pkt, size_t len,
                   uint16_t *seq, uint32_t *timestamp, int *marker, int *payload_type);

#ifdef __cplusplus
}
#endif

#endif /* RP_RTP_H */
