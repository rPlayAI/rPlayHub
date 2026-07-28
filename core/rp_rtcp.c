#include "rp_rtcp.h"

#include <string.h>

#define PT_SR   200
#define PT_RR   201
#define PT_PSFB 206
#define PT_APP  204
#define FMT_PLI 1
#define FMT_FIR 4

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

void rp_rtcp_init(rp_rtcp_session *s, uint32_t ssrc)
{
    memset(s, 0, sizeof *s);
    s->ssrc = ssrc;
}

void rp_rtcp_note_rtp(rp_rtcp_session *s, const uint8_t *pkt, size_t len)
{
    if (len < 12) return;
    uint16_t seq = (uint16_t)((pkt[2] << 8) | pkt[3]);
    if (s->received && seq < (uint16_t)(s->highest_seq & 0xFFFF) - 0x4000) s->cycles++;
    s->highest_seq = (s->cycles << 16) | seq;
    s->their_ssrc = ((uint32_t)pkt[8] << 24) | ((uint32_t)pkt[9] << 16) |
                    ((uint32_t)pkt[10] << 8) | pkt[11];
    s->received++;
}

void rp_rtcp_note_rtcp(rp_rtcp_session *s, const uint8_t *pkt, size_t len, uint64_t now_ms)
{
    if (len < 16) return;
    /* Only a Sender Report carries the timestamp a Receiver Report has to echo. It arrives as 72
     * here, not 200, because the marker bit has already been masked away. */
    if ((pkt[1] & 0x7F) != (PT_SR & 0x7F)) return;
    s->last_sr_middle = ((uint32_t)pkt[10] << 24) | ((uint32_t)pkt[11] << 16) |
                        ((uint32_t)pkt[12] << 8) | pkt[13];
    s->last_sr_at_ms = now_ms;
}

size_t rp_rtcp_build_rr(rp_rtcp_session *s, uint64_t now_ms, uint8_t *out, size_t cap)
{
    if (!s->their_ssrc || cap < 32) return 0;

    uint32_t expected = s->highest_seq + 1;
    uint32_t lost = expected > s->received ? (uint32_t)(expected - s->received) : 0;
    uint32_t expected_interval = expected - (uint32_t)s->expected_prior;
    uint32_t received_interval = (uint32_t)(s->received - s->received_prior);
    s->expected_prior = expected;
    s->received_prior = s->received;
    uint32_t lost_interval = expected_interval - received_interval;
    uint8_t fraction = 0;
    if (expected_interval > 0 && (int32_t)lost_interval > 0) {
        uint32_t f = (lost_interval << 8) / expected_interval;
        fraction = f > 255 ? 255 : (uint8_t)f;
    }
    uint32_t dlsr = 0;
    if (s->last_sr_at_ms) dlsr = (uint32_t)(((now_ms - s->last_sr_at_ms) * 65536) / 1000);

    out[0] = 0x81;                      /* version 2, one report block */
    out[1] = PT_RR;
    out[2] = 0; out[3] = 7;             /* length in 32-bit words minus one */
    put32(out + 4, s->ssrc);
    put32(out + 8, s->their_ssrc);
    out[12] = fraction;
    out[13] = (uint8_t)(lost >> 16); out[14] = (uint8_t)(lost >> 8); out[15] = (uint8_t)lost;
    put32(out + 16, s->highest_seq);
    put32(out + 20, 0);                 /* jitter: not computed */
    put32(out + 24, s->last_sr_middle);
    put32(out + 28, dlsr);
    s->rr_sent++;
    return 32;
}

size_t rp_rtcp_build_pli(rp_rtcp_session *s, uint8_t *out, size_t cap)
{
    if (!s->their_ssrc || cap < 12) return 0;
    out[0] = 0x80 | FMT_PLI;
    out[1] = PT_PSFB;
    out[2] = 0; out[3] = 2;
    put32(out + 4, s->ssrc);
    put32(out + 8, s->their_ssrc);
    s->pli_sent++;
    return 12;
}

size_t rp_rtcp_build_fir(rp_rtcp_session *s, uint8_t *out, size_t cap)
{
    if (!s->their_ssrc || cap < 20) return 0;
    out[0] = 0x80 | FMT_FIR;
    out[1] = PT_PSFB;
    out[2] = 0; out[3] = 4;
    put32(out + 4, s->ssrc);
    put32(out + 8, 0);                  /* unused for FIR */
    put32(out + 12, s->their_ssrc);
    out[16] = s->fir_seq++;
    out[17] = out[18] = out[19] = 0;
    s->fir_sent++;
    return 20;
}

size_t rp_rtcp_build_ltr_ack(rp_rtcp_session *s, uint32_t rtp_timestamp,
                             uint8_t *out, size_t cap)
{
    if (cap < 16) return 0;
    out[0] = 0x80;
    out[1] = PT_APP;
    out[2] = 0; out[3] = 3;             /* 16 bytes total */
    put32(out + 4, s->ssrc);
    put32(out + 8, 5);                  /* the "name" field, numeric here rather than ASCII */
    put32(out + 12, rtp_timestamp);
    s->ltr_acked++;
    return 16;
}
