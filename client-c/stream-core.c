/* stream-core.c — see stream-core.h. Moved verbatim from rplay-view.c (whose comments carry the
 * provenance: parser ported from AnnexBParser in HEVCStream.swift, trailer from
 * HEVCStream.parseActiveRectTrailer). */
#include "stream-core.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
typedef intptr_t ssize_t;
#define close closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint64_t now_ms(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static int init = 0;
    if (!init) {
        QueryPerformanceFrequency(&freq);
        init = 1;
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (uint64_t)((counter.QuadPart * 1000) / freq.QuadPart);
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)(t.tv_nsec / 1000000);
#endif
}

/* ------------------------------------------------------------------ codec model
 *
 * HEVC has a 2-byte NAL header with the type in bits 1-6; H.264 a 1-byte header, bits 0-4. */

int g_h264;
static inline int nal_type(const uint8_t *nal)      { return g_h264 ? (nal[0] & 0x1F) : ((nal[0] >> 1) & 0x3F); }
static inline int is_param_set(int t)               { return g_h264 ? (t == 7 || t == 8) : (t >= 32 && t <= 34); }
static inline int is_vcl(int t)                     { return g_h264 ? (t >= 1 && t <= 5) : t < 32; }
static inline int is_keyframe(int t)                { return g_h264 ? t == 5 : (t >= 16 && t <= 23); }
static inline int is_aud(int t)                     { return g_h264 ? (t == 9) : (t == 35); }
static inline int header_len(void)                  { return g_h264 ? 1 : 2; }

/* ------------------------------------------------------------------ Annex-B splitter
 *
 * Splits a byte stream into NAL units, tolerating reads that land anywhere — including inside a
 * start code. Everything from the last start code onward stays buffered until the next start code
 * proves the NAL complete. Access Unit Delimiters (AUD) are complete upon arrival and emitted
 * immediately to present frames without buffering lag. */

void annexb_feed(annexb_parser *p, const uint8_t *chunk, size_t n, nal_fn on_nal, void *ctx)
{
    if (p->len + n > p->cap) {
        size_t want = p->len + n;
        p->cap = p->cap ? p->cap : 1 << 16;
        while (p->cap < want) p->cap *= 2;
        p->buf = realloc(p->buf, p->cap);
        if (!p->buf) { fprintf(stderr, "out of memory\n"); exit(1); }
    }
    memcpy(p->buf + p->len, chunk, n);
    p->len += n;
    if (p->len < 4) return;

    size_t emit_from = (size_t)-1;   /* start of the previous (now complete) NAL's start code */
    size_t i = 0, kept_from = 0;
    int have_any = 0;
    while (i + 3 <= p->len) {
        int code = 0;
        if (p->buf[i] == 0 && p->buf[i + 1] == 0) {
            if (p->buf[i + 2] == 1) code = 3;
            else if (i + 4 <= p->len && p->buf[i + 2] == 0 && p->buf[i + 3] == 1) code = 4;
        }
        if (!code) { i++; continue; }
        if (emit_from != (size_t)-1) {
            size_t s = emit_from;
            size_t code_len = (p->buf[s + 2] == 1) ? 3 : 4;
            size_t e = i;
            while (e > s + code_len && p->buf[e - 1] == 0) e--;   /* trailing_zero_8bits */
            if (e > s + code_len) on_nal(ctx, p->buf + s + code_len, e - (s + code_len));
        }
        emit_from = i;
        kept_from = i;
        have_any = 1;
        i += code;
    }
    if (have_any) {
        memmove(p->buf, p->buf + kept_from, p->len - kept_from);
        p->len -= kept_from;
        /* If the remaining buffered NAL is a complete AUD, emit it immediately
         * so the Access Unit is flushed without waiting for the next frame. */
        if (p->len >= 4 && p->buf[0] == 0 && p->buf[1] == 0) {
            size_t c_len = p->buf[2] == 1 ? 3 : (p->buf[2] == 0 && p->buf[3] == 1 ? 4 : 0);
            if (c_len && p->len >= c_len + 1) {
                int t = nal_type(p->buf + c_len);
                if (is_aud(t)) {
                    size_t aud_len = g_h264 ? 2 : 3;
                    if (p->len >= c_len + aud_len) {
                        on_nal(ctx, p->buf + c_len, aud_len);
                        p->len = 0;
                    }
                }
            }
        }
    } else if (p->len > 1 << 20) {
        /* No start code in a megabyte: not our stream. Keep a tail so a split code survives. */
        memmove(p->buf, p->buf + p->len - 3, 3);
        p->len = 3;
    }
}

/* End of input: the buffered tail is a complete NAL now — nothing further will prove it so.
 * Only file playback needs this; on a live socket the close IS the end of input. */
void annexb_finish(annexb_parser *p, nal_fn on_nal, void *ctx)
{
    if (p->len < 4 || p->buf[0] != 0 || p->buf[1] != 0) return;
    size_t code_len = p->buf[2] == 1 ? 3 : (p->buf[2] == 0 && p->buf[3] == 1) ? 4 : 0;
    if (!code_len) return;
    size_t e = p->len;
    while (e > code_len && p->buf[e - 1] == 0) e--;
    if (e > code_len) on_nal(ctx, p->buf + code_len, e - code_len);
    p->len = 0;
}

/* ------------------------------------------------------------------ active-rect trailer
 *
 * Matching the known tiers plus a zero byte over only the last 24 bytes is what keeps
 * entropy-coded slice data from matching by accident: five fixed bytes is far too specific to hit
 * by chance, and a false positive would truncate real slice data. */
static int parse_active_rect_trailer(const uint8_t *nal, size_t n, int *w, int *h, size_t *cut)
{
    static const int tiers[][2] = { {1184, 2576}, {1088, 1920}, {720, 1280} };
    if (n <= 24) return 0;
    for (size_t t = 0; t < sizeof tiers / sizeof tiers[0]; t++) {
        const uint8_t pat[4] = { (uint8_t)(tiers[t][0] >> 8), (uint8_t)tiers[t][0],
                                 (uint8_t)(tiers[t][1] >> 8), (uint8_t)tiers[t][1] };
        long lo = (long)n - 24;
        if (lo < 1) lo = 1;
        for (long i = (long)n - 5; i >= lo; i--) {
            if (nal[i] == pat[0] && nal[i + 1] == pat[1] &&
                nal[i + 2] == pat[2] && nal[i + 3] == pat[3] && nal[i + 4] == 0) {
                *w = tiers[t][0];
                *h = tiers[t][1];
                *cut = (size_t)i;
                return 1;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ access-unit assembly
 *
 * NALs are appended to one growing Annex-B buffer (start codes restored) and the whole thing is
 * submitted as one packet when the picture ends. A new picture starts when its first slice does:
 * HEVC signals it with first_slice_segment_in_pic_flag, H.264 with first_mb_in_slice == 0, and
 * both reduce to the top bit of the byte after the NAL header.
 *
 * Keyframe gating is not an optimization. Parameter sets alone are not enough to start decoding:
 * with one IDR per session, every P-frame before an IRAP references pictures the decoder never
 * saw, and feeding it those produces a black window with no error — the macOS app learned this
 * on its first live run. */

static void au_append(stream_state *s, const uint8_t *nal, size_t len)
{
    if (s->au_len + len + 4 > s->au_cap) {
        s->au_cap = s->au_cap ? s->au_cap : 1 << 16;
        while (s->au_cap < s->au_len + len + 4) s->au_cap *= 2;
        s->au = realloc(s->au, s->au_cap);
        if (!s->au) { fprintf(stderr, "out of memory\n"); exit(1); }
    }
    static const uint8_t start[4] = { 0, 0, 0, 1 };
    memcpy(s->au + s->au_len, start, 4);
    memcpy(s->au + s->au_len + 4, nal, len);
    s->au_len += len + 4;
}

static void decode_au(stream_state *s, const uint8_t *data, size_t len)
{
    AVPacket *pkt = av_packet_alloc();
    if (!pkt || av_new_packet(pkt, (int)len) != 0) { av_packet_free(&pkt); return; }
    memcpy(pkt->data, data, len);
    int rc = avcodec_send_packet(s->dec, pkt);
    av_packet_free(&pkt);
    if (rc != 0 && rc != AVERROR(EAGAIN)) {
        s->decode_errors++;
        char err[64];
        av_strerror(rc, err, sizeof err);
        fprintf(stderr, "decode: %s\n", err);
    }
    AVFrame *f = av_frame_alloc();
    while (f && avcodec_receive_frame(s->dec, f) == 0) {
        s->frames_decoded++;
        if (s->on_frame) s->on_frame(f, s->active_w, s->active_h);
        av_frame_unref(f);
    }
    av_frame_free(&f);
}

void flush_au(stream_state *s)
{
    if (!s->au_len) return;

    /* Read the active size off the trailer — the only signal that the encoder downshifted —
     * but leave the bytes in place: the RVRA-patched decoder finds the trailer on the packet
     * tail to know when to resample its references, and to a stock decoder it is spec-invisible
     * (it sits past rbsp_slice_trailing_bits). Stripping it here starved the patch. */
    if (s->last_vcl_len) {
        int w, h;
        size_t cut;
        if (parse_active_rect_trailer(s->au + s->last_vcl_off, s->last_vcl_len, &w, &h, &cut)) {
            s->trailers++;
            s->active_w = w;
            s->active_h = h;
        }
    }
    if (s->last_vcl_len) s->frames_submitted++;   /* parameter-set-only packets are not frames */
    decode_au(s, s->au, s->au_len);
    s->au_len = 0;
    s->last_vcl_len = 0;
}

void handle_nal(void *ctx, const uint8_t *nal, size_t len)
{
    stream_state *s = ctx;
    if (len <= (size_t)header_len()) return;
    s->nals++;
    s->last_nal_ms = now_ms();
    int t = nal_type(nal);

    if (is_param_set(t)) {
        flush_au(s);
        au_append(s, nal, len);   /* in-band is fine: libavcodec picks parameter sets out of the packet */
        return;
    }
    if (is_aud(t)) {
        flush_au(s);
        return;
    }
    if (!is_vcl(t)) return;       /* SEI, end-of-sequence — nothing to display */

    int first_slice = (nal[header_len()] & 0x80) != 0;
    if (first_slice) flush_au(s);

    if (is_keyframe(t)) {
        s->awaiting_keyframe = 0;
    } else if (s->awaiting_keyframe) {
        s->frames_before_keyframe++;
        return;
    }
    s->last_vcl_off = s->au_len + 4;
    s->last_vcl_len = len;
    au_append(s, nal, len);
}

/* ------------------------------------------------------------------ transport */

int tcp_connect(const char *host, int port)
{
#ifdef _WIN32
    static int wsa_init = 0;
    if (!wsa_init) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        wsa_init = 1;
    }
#endif
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        fprintf(stderr, "%s: not an IPv4 address (spike limitation)\n", host);
        return -1;
    }
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
#ifdef _WIN32
        int err = WSAGetLastError();
        if (err != WSAECONNREFUSED) {
            fprintf(stderr, "connect %s:%d: error %d\n", host, port, err);
        }
#else
        if (errno != ECONNREFUSED) {
            fprintf(stderr, "connect %s:%d: %s\n", host, port, strerror(errno));
        }
#endif
        close(fd);
        return -1;
    }
    return fd;
}

/* Ask 9876 which codec the device negotiated. Best effort: on any failure the answer is "hevc",
 * which is what the device picks when offered both. One line of JSON out, one line back; the
 * value is found with strstr because the reply is our own engine's flat JSON, not the internet's. */
int stream_says_h264(const char *host, int api_port)
{
    int fd = tcp_connect(host, api_port);
    if (fd < 0) return 0;
#ifdef _WIN32
    DWORD tv = 2000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
#else
    struct timeval tv = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
    const char req[] = "{\"id\":1,\"method\":\"stream_info\"}\n";
    if (send(fd, req, sizeof req - 1, 0) != (ssize_t)(sizeof req - 1)) { close(fd); return 0; }
    char reply[2048];
    ssize_t got = 0, n;
    while (got < (ssize_t)sizeof reply - 1 &&
           (n = recv(fd, reply + got, (int)(sizeof reply - 1 - got), 0)) > 0) {
        got += n;
        if (memchr(reply, '\n', (size_t)got)) break;
    }
    close(fd);
    reply[got > 0 ? got : 0] = 0;
    return strstr(reply, "\"codec\":\"h264\"") != NULL;
}
