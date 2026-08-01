// pcapreplay — run the shipped depacketizer over a captured session, offline.
//
// Why this exists. Every attempt to find the rendering fault has needed a live
// device, which means every run had different content, different timing and no
// way to repeat a result. A pcap is a fixed input: the same bytes, every time,
// as many times as we like. And logs/devicehub.pcap is the best possible fixed
// input, because Device Hub displayed those exact packets perfectly while they
// were being captured. So a clean decode is known to be achievable from them,
// which makes "keep changing the assembler until the picture is right" a search
// with an end rather than an open-ended one.
//
// It runs core/rp_rtp.c or core/rp_rtp_assembler.c — the code we actually ship,
// not a reimplementation. That matters here more than usual: the handoff records
// that our C and our Python agreed with each other only because they shared the
// same omission, so a second opinion is worth nothing unless it comes from
// somewhere else entirely. This gives the shipped code a deterministic input;
// ffmpeg then gives the second opinion on the output.
//
//   pcapreplay <pcap> <out.h265> [--assembler legacy|miracast] [--pt N] [--stats]
//
// The capture carries two RTP streams — video and audio — and feeding audio into
// the video depacketizer would shred it. Streams are separated by SSRC and
// payload type, and the video one is chosen by volume, never by payload number:
// this device sends HEVC under payload type 100, which is the number we
// advertise for AVC.

#include "../../core/rp_rtp.h"
#include "../../core/rp_rtp_assembler.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_STREAMS 16

typedef struct {
    uint32_t ssrc;
    int      pt;
    uint64_t packets, bytes;
} stream_info;

static stream_info streams[MAX_STREAMS];
static int nstreams;

static FILE *out;
static uint64_t nals_out, bytes_out, frames_out;

static void note_stream(uint32_t ssrc, int pt, size_t len) {
    for (int i = 0; i < nstreams; i++)
        if (streams[i].ssrc == ssrc && streams[i].pt == pt) {
            streams[i].packets++;
            streams[i].bytes += len;
            return;
        }
    if (nstreams < MAX_STREAMS) {
        streams[nstreams].ssrc = ssrc;
        streams[nstreams].pt = pt;
        streams[nstreams].packets = 1;
        streams[nstreams].bytes = len;
        nstreams++;
    }
}

static void write_nal(void *ctx, const uint8_t *nal, size_t len) {
    (void)ctx;
    if (!len) return;                       // frame-boundary marker, no payload
    static const uint8_t sc[4] = {0, 0, 0, 1};
    fwrite(sc, 1, 4, out);
    fwrite(nal, 1, len, out);
    nals_out++;
    bytes_out += len;
}

static void note_frame(void *ctx) { (void)ctx; frames_out++; }

// ------------------------------------------------------------------ pcap

typedef struct { uint8_t *data; size_t len; } packet;

static packet *packets;
static size_t npackets;

static uint32_t rd32(const uint8_t *p, int be) {
    return be ? ((uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3])
              : ((uint32_t)p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0]);
}

static int load_pcap(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    uint8_t hdr[24];
    if (fread(hdr, 1, 24, f) != 24) { fclose(f); return -1; }
    int be;
    if (!memcmp(hdr, "\xd4\xc3\xb2\xa1", 4))      be = 0;
    else if (!memcmp(hdr, "\xa1\xb2\xc3\xd4", 4)) be = 1;
    else { fprintf(stderr, "not a classic pcap\n"); fclose(f); return -1; }
    uint32_t link = rd32(hdr + 20, be);
    if (link != 0) fprintf(stderr, "warning: linktype %u, expected 0 (DLT_NULL)\n", link);

    size_t cap = 4096;
    packets = malloc(cap * sizeof *packets);
    uint8_t buf[65536];
    for (;;) {
        uint8_t ph[16];
        if (fread(ph, 1, 16, f) != 16) break;
        uint32_t caplen = rd32(ph + 8, be);
        if (caplen > sizeof buf) break;
        if (fread(buf, 1, caplen, f) != caplen) break;
        if (caplen < 4) continue;

        // DLT_NULL: 4-byte address-family header, then IPv6 or IPv4.
        uint32_t af = rd32(buf, be);
        const uint8_t *ip = buf + 4;
        size_t iplen = caplen - 4;
        const uint8_t *udp = NULL;
        size_t udplen = 0;
        if (af == 30 && iplen >= 40 && ip[6] == 17) {
            udp = ip + 40;
            udplen = iplen - 40;
        } else if (af == 2 && iplen >= 20 && ip[9] == 17) {
            size_t ihl = (ip[0] & 0xF) * 4;
            if (iplen > ihl) { udp = ip + ihl; udplen = iplen - ihl; }
        }
        if (!udp || udplen < 8) continue;
        size_t plen = ((size_t)udp[4] << 8 | udp[5]);
        if (plen < 8 || plen > udplen) plen = udplen;
        const uint8_t *pay = udp + 8;
        size_t paylen = plen - 8;
        if (!paylen) continue;

        if (npackets == cap) {
            cap *= 2;
            packets = realloc(packets, cap * sizeof *packets);
        }
        packets[npackets].data = malloc(paylen);
        memcpy(packets[npackets].data, pay, paylen);
        packets[npackets].len = paylen;
        npackets++;
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <pcap> <out.h265> "
                        "[--assembler legacy|miracast] [--pt N] [--stats]\n", argv[0]);
        return 2;
    }
    const char *pcap_path = argv[1], *out_path = argv[2];
    const char *which = "legacy";
    int want_pt = -1, stats = 0;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--assembler") && i + 1 < argc) which = argv[++i];
        else if (!strcmp(argv[i], "--pt") && i + 1 < argc) want_pt = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stats")) stats = 1;
    }

    if (load_pcap(pcap_path) < 0) return 1;
    fprintf(stderr, "%s: %zu UDP datagrams\n", pcap_path, npackets);

    // Census first: which SSRC/payload-type pairs are RTP, and how big is each.
    for (size_t i = 0; i < npackets; i++) {
        const uint8_t *p = packets[i].data;
        size_t n = packets[i].len;
        if (n < 12 || (p[0] >> 6) != 2) continue;
        if (rp_rtp_is_rtcp(p, n)) continue;
        note_stream(rd32(p + 8, 1), p[1] & 0x7F, n);
    }
    fprintf(stderr, "RTP streams found:\n");
    int best = -1;
    for (int i = 0; i < nstreams; i++) {
        fprintf(stderr, "  ssrc 0x%08x  pt %3d  %8" PRIu64 " packets  %10" PRIu64 " bytes\n",
                streams[i].ssrc, streams[i].pt, streams[i].packets, streams[i].bytes);
        if (want_pt >= 0) {
            if (streams[i].pt == want_pt) best = i;
        } else if (best < 0 || streams[i].bytes > streams[best].bytes) {
            best = i;
        }
    }
    if (best < 0) { fprintf(stderr, "no RTP stream selected\n"); return 1; }
    fprintf(stderr, "selected: ssrc 0x%08x pt %d (by %s)\n",
            streams[best].ssrc, streams[best].pt,
            want_pt >= 0 ? "requested payload type" : "volume, not payload number");

    out = fopen(out_path, "wb");
    if (!out) { perror(out_path); return 1; }

    uint64_t fed = 0;
    if (!strcmp(which, "miracast")) {
        static rp_rtp_assembler a;
        static rp_ra_packet queue[1024];
        static uint8_t nal_storage[RP_RTP_MAX_NAL];
        if (rp_ra_init(&a, RP_RA_CODEC_HEVC, queue, 1024,
                       nal_storage, sizeof nal_storage,
                       write_nal, note_frame, NULL) != 0) {
            fprintf(stderr, "rp_ra_init failed\n");
            return 1;
        }
        for (size_t i = 0; i < npackets; i++) {
            const uint8_t *p = packets[i].data;
            size_t n = packets[i].len;
            if (n < 12 || (p[0] >> 6) != 2 || rp_ra_is_rtcp(p, n)) continue;
            if (rd32(p + 8, 1) != streams[best].ssrc) continue;
            rp_ra_feed(&a, p, n, (uint64_t)i * 1000);
            fed++;
        }
        rp_ra_tick(&a, (uint64_t)(npackets + 100000) * 1000);
        if (stats)
            fprintf(stderr, "assembler(miracast): received %" PRIu64 " lost %" PRIu64
                            " late %" PRIu64 "\n",
                    (uint64_t)a.received, (uint64_t)a.lost, (uint64_t)a.late);
    } else {
        static rp_rtp_session s;
        static uint8_t storage[RP_RTP_REORDER_WINDOW * 1500 + RP_RTP_MAX_NAL];
        if (rp_rtp_init(&s, RP_RTP_CODEC_HEVC, storage, sizeof storage, write_nal, NULL) != 0) {
            fprintf(stderr, "rp_rtp_init failed\n");
            return 1;
        }
        for (size_t i = 0; i < npackets; i++) {
            const uint8_t *p = packets[i].data;
            size_t n = packets[i].len;
            if (n < 12 || (p[0] >> 6) != 2 || rp_rtp_is_rtcp(p, n)) continue;
            if (rd32(p + 8, 1) != streams[best].ssrc) continue;
            int marker = 0;
            rp_rtp_header(p, n, NULL, NULL, &marker, NULL);
            rp_rtp_feed(&s, p, n);
            if (marker) frames_out++;
            fed++;
        }
        rp_rtp_flush(&s);
        if (stats)
            fprintf(stderr, "depacketizer(legacy): received %" PRIu64 " lost %" PRIu64
                            " late %" PRIu64 " dup %" PRIu64 " malformed %" PRIu64
                            " truncated %" PRIu64 "\n",
                    s.received, s.lost, s.late, s.duplicates, s.malformed, s.truncated);
    }

    fclose(out);
    fprintf(stderr, "fed %" PRIu64 " packets -> %" PRIu64 " NALs, %" PRIu64 " frame markers, "
                    "%" PRIu64 " payload bytes -> %s\n",
            fed, nals_out, frames_out, bytes_out, out_path);
    return 0;
}
