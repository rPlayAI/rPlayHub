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
    uint64_t received;
    uint64_t expected_prior, received_prior;

    uint32_t last_sr_middle;    /* middle 32 bits of the NTP stamp in their last Sender Report */
    uint64_t last_sr_at_ms;

    uint64_t rr_sent, pli_sent, fir_sent, ltr_acked;
    uint8_t  fir_seq;
} rp_rtcp_session;

void rp_rtcp_init(rp_rtcp_session *s, uint32_t ssrc);

/* Observe traffic, so the reports we send describe reality. */
void rp_rtcp_note_rtp(rp_rtcp_session *s, const uint8_t *pkt, size_t len);
void rp_rtcp_note_rtcp(rp_rtcp_session *s, const uint8_t *pkt, size_t len, uint64_t now_ms);

/* Build packets. Each returns the length written, or 0 if there is nothing to send yet (which is
 * the case until we know their SSRC). */
size_t rp_rtcp_build_rr(rp_rtcp_session *s, uint64_t now_ms, uint8_t *out, size_t cap);
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

#ifdef __cplusplus
}
#endif

#endif /* RP_RTCP_H */
