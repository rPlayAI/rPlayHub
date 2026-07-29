#include "rp_rtp.h"

#include <string.h>

#define HEVC_AP  48
#define HEVC_FU  49
#define AVC_STAP_A 24
#define AVC_FU_A   28
#define SLOT_BYTES 1500

int rp_rtp_is_rtcp(const uint8_t *pkt, size_t len)
{
    if (len < 2) return 1;
    /* The marker bit has already been masked off here, so RTCP's 200-204 appears as 72-76.
     * RFC 5761 reserves 64-95 for RTCP when it shares a port with RTP, which is the range to
     * test. Checking for 200-204 after masking can never match -- that bug fed every RTCP
     * packet into the video depacketizer. */
    int pt = pkt[1] & 0x7F;
    return pt >= 64 && pt <= 95;
}

int rp_rtp_header(const uint8_t *pkt, size_t len,
                  uint16_t *seq, uint32_t *timestamp, int *marker, int *payload_type)
{
    if (len < 12) return -1;
    if (seq) *seq = (uint16_t)((pkt[2] << 8) | pkt[3]);
    if (timestamp) *timestamp = ((uint32_t)pkt[4] << 24) | ((uint32_t)pkt[5] << 16) |
                                ((uint32_t)pkt[6] << 8) | pkt[7];
    if (marker) *marker = (pkt[1] >> 7) & 1;
    if (payload_type) *payload_type = pkt[1] & 0x7F;
    return 0;
}

/* Strip the header and any extension, returning the payload. */
static const uint8_t *rtp_payload(const uint8_t *pkt, size_t len, size_t *out_len)
{
    if (len < 12) return NULL;
    size_t off = 12 + 4u * (size_t)(pkt[0] & 0x0F);          /* CSRC list */
    if (pkt[0] & 0x10) {                                      /* extension present */
        if (off + 4 > len) return NULL;
        size_t words = ((size_t)pkt[off + 2] << 8) | pkt[off + 3];
        off += 4 + 4 * words;
    }
    if (off >= len) return NULL;
    size_t end = len;
    /* Padding: the P bit means the last byte counts the padding bytes, all of which are part of
     * the packet but NOT part of the payload. Without this they are copied into the NAL as if
     * they were video, and a decoder handed those extra bytes produces exactly the kind of
     * corruption that leaves the packet counters showing no loss at all. */
    if (pkt[0] & 0x20) {
        size_t pad = pkt[len - 1];
        if (pad == 0 || pad > end - off) return NULL;   /* malformed; drop the packet */
        end -= pad;
    }
    if (off >= end) return NULL;
    *out_len = end - off;
    return pkt + off;
}

int rp_rtp_sniff_codec(const uint8_t *payload, size_t len)
{
    if (len < 2) return -1;
    int hevc_type = (payload[0] >> 1) & 0x3F;
    int avc_type = payload[0] & 0x1F;
    if (hevc_type == HEVC_AP || hevc_type == HEVC_FU) return RP_RTP_CODEC_HEVC;
    if (avc_type == AVC_STAP_A || avc_type == AVC_FU_A) return RP_RTP_CODEC_H264;
    return -1;
}

int rp_rtp_init(rp_rtp_session *s, rp_rtp_codec codec, uint8_t *storage, size_t storage_len,
                rp_rtp_nal_fn on_nal, void *ctx)
{
    size_t need = (size_t)RP_RTP_REORDER_WINDOW * SLOT_BYTES + RP_RTP_MAX_NAL;
    if (storage_len < need) return -1;
    memset(s, 0, sizeof *s);
    s->codec = codec;
    s->on_nal = on_nal;
    s->ctx = ctx;
    for (int i = 0; i < RP_RTP_REORDER_WINDOW; i++) {
        s->slots[i].data = storage + (size_t)i * SLOT_BYTES;
        s->slots[i].len = 0;
        s->slots[i].seq = 0;
    }
    s->fu = storage + (size_t)RP_RTP_REORDER_WINDOW * SLOT_BYTES;
    s->fu_cap = RP_RTP_MAX_NAL;
    return 0;
}

static void emit(rp_rtp_session *s, const uint8_t *nal, size_t len)
{
    if (len && s->on_nal) s->on_nal(s->ctx, nal, len);
}

/* RFC 7798 for HEVC, RFC 6184 for H.264. */
static void depacketize(rp_rtp_session *s, const uint8_t *p, size_t n)
{
    if (n < 2) return;

    if (s->codec == RP_RTP_CODEC_HEVC) {
        int type = (p[0] >> 1) & 0x3F;
        if (type == HEVC_AP) {
            size_t i = 2;
            while (i + 2 <= n) {
                size_t size = ((size_t)p[i] << 8) | p[i + 1];
                i += 2;
                /* The aggregation packet states each NAL's length itself, so this is the one
                 * place the wire tells us what the size should be and we can check it. A NAL
                 * claiming more bytes than the packet holds means we have misparsed something
                 * upstream; counting it turns a silent discard into evidence. */
                if (i + size > n) { s->malformed++; return; }
                emit(s, p + i, size);
                i += size;
            }
            if (i != n) s->malformed++;   /* bytes left over that no length accounted for */
        } else if (type == HEVC_FU) {
            if (n < 3) return;
            uint8_t fuh = p[2];
            if (fuh & 0x80) {          /* start: rebuild the two-byte NAL header */
                s->fu_len = 0;
                if (s->fu_cap < 2) return;
                s->fu[0] = (uint8_t)((p[0] & 0x81) | (((fuh & 0x3F)) << 1));
                s->fu[1] = p[1];
                s->fu_len = 2;
            }
            size_t add = n - 3;
            if (s->fu_len && s->fu_len + add <= s->fu_cap) {
                memcpy(s->fu + s->fu_len, p + 3, add);
                s->fu_len += add;
            } else if (s->fu_len) {
                /* Does not fit. Previously the copy was skipped but fu_len was kept, so the end
                 * marker emitted a NAL missing its middle -- structurally valid, silently wrong,
                 * and indistinguishable downstream from a correctly received frame. Abandon the
                 * unit instead: a dropped NAL is visible in the counters, a truncated one is not. */
                s->truncated++;
                s->fu_len = 0;
            }
            if ((fuh & 0x40) && s->fu_len) {   /* end */
                emit(s, s->fu, s->fu_len);
                s->fu_len = 0;
            }
        } else {
            emit(s, p, n);
        }
        return;
    }

    /* H.264 */
    int type = p[0] & 0x1F;
    if (type == AVC_STAP_A) {
        size_t i = 1;
        while (i + 2 <= n) {
            size_t size = ((size_t)p[i] << 8) | p[i + 1];
            i += 2;
            if (i + size > n) { s->malformed++; return; }
            emit(s, p + i, size);
            i += size;
        }
    } else if (type == AVC_FU_A) {
        if (n < 2) return;
        uint8_t fuh = p[1];
        if (fuh & 0x80) {
            s->fu_len = 0;
            if (s->fu_cap < 1) return;
            s->fu[0] = (uint8_t)((p[0] & 0xE0) | (fuh & 0x1F));
            s->fu_len = 1;
        }
        size_t add = n - 2;
        if (s->fu_len && s->fu_len + add <= s->fu_cap) {
            memcpy(s->fu + s->fu_len, p + 2, add);
            s->fu_len += add;
        }
        if ((fuh & 0x40) && s->fu_len) {
            emit(s, s->fu, s->fu_len);
            s->fu_len = 0;
        }
    } else {
        emit(s, p, n);
    }
}

static int slot_of(uint16_t seq) { return seq % RP_RTP_REORDER_WINDOW; }

/* Release everything that has arrived in order, starting at next_seq. */
static void drain(rp_rtp_session *s)
{
    for (;;) {
        int i = slot_of(s->next_seq);
        if (!s->slots[i].len || s->slots[i].seq != s->next_seq) return;
        depacketize(s, s->slots[i].data, s->slots[i].len);
        s->slots[i].len = 0;
        s->buffered--;
        s->next_seq = (uint16_t)(s->next_seq + 1);
    }
}

int rp_rtp_feed(rp_rtp_session *s, const uint8_t *pkt, size_t len)
{
    if (rp_rtp_is_rtcp(pkt, len)) return 0;

    size_t plen = 0;
    const uint8_t *payload = rtp_payload(pkt, len, &plen);
    if (!payload || !plen || plen > SLOT_BYTES) return 1;

    uint16_t seq;
    if (rp_rtp_header(pkt, len, &seq, NULL, NULL, NULL) != 0) return 1;

    /* The payload structure decides the codec, never the payload type number. */
    if (!s->codec_locked) {
        int detected = rp_rtp_sniff_codec(payload, plen);
        if (detected >= 0) { s->codec = (rp_rtp_codec)detected; s->codec_locked = 1; }
    }

    if (!s->have_next) { s->next_seq = seq; s->have_next = 1; }

    /* Behind the window: already emitted, so this arrived too late to use. */
    if ((uint16_t)(seq - s->next_seq) > 0x8000u) { s->late++; return 1; }

    int i = slot_of(seq);
    if (s->slots[i].len && s->slots[i].seq == seq) { s->duplicates++; return 1; }
    if (s->slots[i].len) {
        /* The slot is held by an older packet we are still waiting behind. Give up on it: it is
         * not going to arrive, and holding the whole stream for it is worse than a gap. */
        s->lost += (uint16_t)(seq - s->next_seq);
        s->next_seq = seq;
        s->slots[i].len = 0;
        s->buffered--;
    }
    memcpy(s->slots[i].data, payload, plen);
    s->slots[i].seq = seq;
    s->slots[i].len = (uint16_t)plen;
    s->buffered++;
    s->received++;

    drain(s);

    /* If the buffer fills, the packet we are waiting for is never coming. Step over it and count
     * it, rather than losing the whole stream to one hole. */
    while (s->buffered >= RP_RTP_REORDER_WINDOW - 1) {
        uint16_t probe = s->next_seq;
        int found = 0;
        for (int k = 0; k < RP_RTP_REORDER_WINDOW; k++) {
            uint16_t cand = (uint16_t)(probe + k);
            int ci = slot_of(cand);
            if (s->slots[ci].len && s->slots[ci].seq == cand) {
                s->lost += k;
                s->next_seq = cand;
                found = 1;
                break;
            }
        }
        if (!found) break;
        drain(s);
    }
    return 1;
}

void rp_rtp_flush(rp_rtp_session *s)
{
    /* Release in order, skipping gaps: at end of stream there is nothing left to wait for. */
    for (int k = 0; k < RP_RTP_REORDER_WINDOW && s->buffered > 0; k++) {
        uint16_t cand = (uint16_t)(s->next_seq + k);
        int ci = slot_of(cand);
        if (s->slots[ci].len && s->slots[ci].seq == cand) {
            depacketize(s, s->slots[ci].data, s->slots[ci].len);
            s->slots[ci].len = 0;
            s->buffered--;
        }
    }
    s->fu_len = 0;
}
