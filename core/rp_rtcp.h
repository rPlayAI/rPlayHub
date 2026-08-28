/*
 * rp_rtcp.h — the receiver side of RTCP, which is what keeps this stream alive and repairable.
 *
 * Not optional and not a refinement. Measured on a real device:
 *
 *  - Without Receiver Reports roughly once a second, the device stops sending video after about
 *    20 seconds. Its own answer asks for exactly that: RTCPSendInterval 1.0, RTCPTimeoutInterval
 *    20.0.
 *  - Every packet here must carry the SAME SSRC that the offer announced in field 5.1, which the
 *    device echoes back as RemoteSSRC. Feedback from any other source is silently discarded. We
 *    generated the two independently for a long time, and the result was that LTR acks, PLI and
 *    FIR all appeared to be ignored -- three mechanisms broken by one wrong number, each failing
 *    in a way that looked like a separate protocol mystery.
 *  - With the SSRC right, a PLI produces a keyframe after a single request. That matters because
 *    the device otherwise sends exactly one IDR per session: without a way to ask for another,
 *    any corruption persists until the session ends.
 *
 * Apple sends no PLI or FIR at all -- it relies on LTR acknowledgement alone, and its captures
 * contain one IDR in fifteen seconds. Asking periodically is a deliberate difference.
 */
#ifndef RP_RTCP_H
#define RP_RTCP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t ssrc;          /* ours: MUST equal the offer's field 5.1 */
    uint32_t their_ssrc;    /* learned from the first RTP packet */

    uint32_t highest_seq;
    uint32_t cycles;
    uint32_t base_seq;      /* the first sequence number seen: the device starts at a random seq,
                             * and an RR whose expected-count ignores that claims tens of thousands
                             * of lost packets -- which the device rejects, and 20 s later it stops
                             * sending (RTCPTimeoutInterval) */
    uint64_t received;
    uint64_t expected_prior, received_prior;

    uint32_t last_sr_middle;    /* middle 32 bits of the NTP stamp in their last Sender Report */
    uint64_t last_sr_at_ms;

    uint64_t rr_sent, pli_sent, fir_sent, ltr_acked, rctl_sent;
    uint8_t  fir_seq;

    /* What RCTL reports, and nothing else needs: the media time of the newest video frame, when
     * the newest video packet arrived, and the media time the session started from. */
    uint32_t first_rtp_ts, last_rtp_ts;
    int      have_first_rtp_ts;
    uint64_t last_rtp_at_ms;
} rp_rtcp_session;

void rp_rtcp_init(rp_rtcp_session *s, uint32_t ssrc);

/* Observe traffic, so the reports we send describe reality. `now_ms` is when the packet arrived;
 * RCTL reports the gap since the last one, which nothing else needs. */
void rp_rtcp_note_rtp(rp_rtcp_session *s, const uint8_t *pkt, size_t len, uint64_t now_ms);
void rp_rtcp_note_rtcp(rp_rtcp_session *s, const uint8_t *pkt, size_t len, uint64_t now_ms);

/* Build packets. Each returns the length written, or 0 if there is nothing to send yet (which is
 * the case until we know their SSRC). */
size_t rp_rtcp_build_rr(rp_rtcp_session *s, uint64_t now_ms, uint8_t *out, size_t cap);
size_t rp_rtcp_build_sr(rp_rtcp_session *s, uint8_t *out, size_t cap);
size_t rp_rtcp_build_pli(rp_rtcp_session *s, uint8_t *out, size_t cap);
size_t rp_rtcp_build_fir(rp_rtcp_session *s, uint8_t *out, size_t cap);

/* Acknowledge a frame we received intact, by its RTP timestamp.
 *
 * This is how this device recovers from loss; it never sends a second IDR of its own accord. The
 * wire format was read directly off a capture of Device Hub: an RTCP APP packet, 16 bytes, with
 * 0x00000005 in the name field rather than ASCII.
 *
 *     80 cc 00 03 | <our SSRC> | 00 00 00 05 | <acknowledged RTP timestamp>
 */
size_t rp_rtcp_build_ltr_ack(rp_rtcp_session *s, uint32_t rtp_timestamp,
                             uint8_t *out, size_t cap);

/* Rate-control feedback. This is the one thing Device Hub sends that we never have.
 *
 * The device answers `RateAdaptationEnabled=True`, and Device Hub then sends RCTL about twenty
 * times a second for the life of the session while never sending a single PLI. We do the opposite.
 * The layout below was recovered from logs/devicehub.pcap by correlating each field against the
 * video RTP in the same capture; see doc/DEVICEHUB-CAPTURE-FINDINGS.md, "RCTL, decoded".
 *
 *   80 cc 00 07 | <our SSRC> | 'RCTL' | 85 00 00 04
 *     +16 u16  media time of the newest frame received, in 256-tick units of the 24 kHz clock
 *     +18 u32  zero
 *     +22 u16  milliseconds since the last video packet arrived
 *     +24 u16  a local clock in 1/1024 s, wrapping
 *     +26 u16  UNIDENTIFIED -- sent as zero, which is what Device Hub's first packets carry
 *     +28 u16  cumulative video RTP packets received
 *     +30 u16  target bitrate, in units of 100 bps (60000 = the negotiated 6 Mbps ceiling)
 *
 * Sent alone in its own datagram, never compounded with SR/RR/SDES.
 *
 * `target_bps` is what to ask the encoder for. Zero is what Device Hub sends before it has decided,
 * and is safe.
 */
size_t rp_rtcp_build_rctl(rp_rtcp_session *s, uint64_t now_ms, uint32_t target_bps,
                          uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* RP_RTCP_H */
