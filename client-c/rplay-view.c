/*
 * rplay-view.c — the portable thin client: TCP 9877 → ffmpeg software decode → SDL2.
 *
 * This is the Linux-port answer to the macOS app's HEVCStream.swift + VideoDecoder.swift +
 * AVSampleBufferDisplayLayer, in one file with two dependencies (libavcodec, SDL2), both of which
 * exist everywhere. The engine's side of the contract is app/api/PROTOCOL.md: newline-delimited
 * JSON control on 9876, and on 9877 a bare Annex-B byte stream — no container, no timestamps,
 * cached VPS/SPS/PPS re-sent to a joining viewer, and ONE IDR per session, so no access unit may
 * be dropped before decode (frame skipping, if ever needed, must happen after).
 *
 * RVRA (doc/RVRA-AND-PORTABILITY.md): under motion the encoder downshifts the coded picture
 * into the top-left of the same frame and appends [w:u16be][h:u16be][00...][session tag] to
 * the slice NAL. Decoding that correctly needs REFERENCE RESAMPLING, which standard HEVC does
 * not have — so this client must be linked against the RVRA-patched ffmpeg in deps/ffmpeg
 * (scripts/build-ffmpeg-rvra.sh; the Makefile picks it up automatically), and it arms the
 * patch via RPLAY_RVRA=1. The macOS app keeps using VideoToolbox with the private RVRA
 * properties; this decoder is the same picture for every other platform — worst frame 39 dB
 * against the VideoToolbox ground truth, visually indistinguishable. Linked against a stock
 * ffmpeg it still runs, but pictures garble from the first downshift under motion.
 *
 * The trailer does double duty: the patched decoder reads it off the packet tail to know when
 * to resample (so it must NOT be stripped before decode — it is spec-invisible), and this
 * client parses it for the display crop. Parser ported from HEVCStream.swift.
 *
 * Modes:
 *   rplay-view [-s HOST] [-p PORT]     live view (default 127.0.0.1:9877; codec asked over 9876)
 *   rplay-view -f capture.h265 [-r N]  play a capture, paced at N fps (default 60)
 *   --check                            headless: decode everything, print stats, no window
 *   --codec hevc|h264                  override what stream_info said (or for files)
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>

#include <SDL.h>

static uint64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)(t.tv_nsec / 1000000);
}

/* ------------------------------------------------------------------ Annex-B splitter
 *
 * Splits a byte stream into NAL units, tolerating reads that land anywhere — including inside a
 * start code. Everything from the last start code onward stays buffered until the next start code
 * proves the NAL complete. Ported from AnnexBParser in HEVCStream.swift. */

typedef struct {
    uint8_t *buf;
    size_t   len, cap;
} annexb_parser;

typedef void (*nal_fn)(void *ctx, const uint8_t *nal, size_t len);

static void annexb_feed(annexb_parser *p, const uint8_t *chunk, size_t n, nal_fn on_nal, void *ctx)
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
    } else if (p->len > 1 << 20) {
        /* No start code in a megabyte: not our stream. Keep a tail so a split code survives. */
        memmove(p->buf, p->buf + p->len - 3, 3);
        p->len = 3;
    }
}

/* End of input: the buffered tail is a complete NAL now — nothing further will prove it so.
 * Only file playback needs this; on a live socket the close IS the end of input. */
static void annexb_finish(annexb_parser *p, nal_fn on_nal, void *ctx)
{
    if (p->len < 4 || p->buf[0] != 0 || p->buf[1] != 0) return;
    size_t code_len = p->buf[2] == 1 ? 3 : (p->buf[2] == 0 && p->buf[3] == 1) ? 4 : 0;
    if (!code_len) return;
    size_t e = p->len;
    while (e > code_len && p->buf[e - 1] == 0) e--;
    if (e > code_len) on_nal(ctx, p->buf + code_len, e - code_len);
    p->len = 0;
}

/* ------------------------------------------------------------------ codec model
 *
 * HEVC has a 2-byte NAL header with the type in bits 1-6; H.264 a 1-byte header, bits 0-4.
 * Same reductions HEVCStream.swift uses. */

static int g_h264;
static int nal_type(const uint8_t *nal)      { return g_h264 ? (nal[0] & 0x1F) : ((nal[0] >> 1) & 0x3F); }
static int is_param_set(int t)               { return g_h264 ? (t == 7 || t == 8) : (t >= 32 && t <= 34); }
static int is_vcl(int t)                     { return g_h264 ? (t >= 1 && t <= 5) : t < 32; }
static int is_keyframe(int t)                { return g_h264 ? t == 5 : (t >= 16 && t <= 23); }
static int header_len(void)                  { return g_h264 ? 1 : 2; }

/* ------------------------------------------------------------------ active-rect trailer
 *
 * Ported from HEVCStream.parseActiveRectTrailer. Matching the known tiers plus a zero byte over
 * only the last 24 bytes is what keeps entropy-coded slice data from matching by accident: five
 * fixed bytes is far too specific to hit by chance, and a false positive would truncate real
 * slice data. */
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

typedef struct {
    uint8_t *au;                 /* the pending access unit, Annex-B */
    size_t   au_len, au_cap;
    size_t   last_vcl_off;       /* offset of the last VCL NAL's payload, for trailer stripping */
    size_t   last_vcl_len;
    int      awaiting_keyframe;
    uint64_t last_nal_ms;        /* for the idle flush */

    int      active_w, active_h; /* what the trailer last said; 0 until seen */
    uint64_t nals, frames_submitted, frames_before_keyframe, trailers;

    AVCodecContext *dec;
    void (*on_frame)(AVFrame *f, int active_w, int active_h);
    uint64_t frames_decoded, decode_errors;
} stream_state;

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

static void flush_au(stream_state *s)
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

static void handle_nal(void *ctx, const uint8_t *nal, size_t len)
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
    if (!is_vcl(t)) return;       /* SEI, AUD, end-of-sequence — nothing to display */

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

/* ------------------------------------------------------------------ display (SDL2)
 *
 * One streaming IYUV texture at the coded size; the active rect is a source crop, so the
 * downshifted frames upscale to the same window for free. The window keeps the active rect's
 * aspect and letterboxes the rest. */

typedef struct {
    SDL_Window   *win;
    SDL_Renderer *ren;
    SDL_Texture  *tex;
    int tex_w, tex_h;
} display;

static display g_disp;
static int g_quit, g_headless;

static void show_frame(AVFrame *f, int active_w, int active_h)
{
    display *d = &g_disp;
    if (g_headless) return;
    if (!d->win) {
        /* First frame: now the coded size is known. A 1184x2576 portrait frame is taller than
         * any desktop, so open at 40% and let the user resize. */
        SDL_Init(SDL_INIT_VIDEO);
        int ww = f->width * 2 / 5, wh = f->height * 2 / 5;
        d->win = SDL_CreateWindow("rplay-view", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  ww, wh, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
        d->ren = d->win ? SDL_CreateRenderer(d->win, -1, SDL_RENDERER_PRESENTVSYNC) : NULL;
        if (d->win && !d->ren) d->ren = SDL_CreateRenderer(d->win, -1, 0);
        if (!d->ren) {
            fprintf(stderr, "no display (%s); decoding without one\n", SDL_GetError());
            g_headless = 1;
            return;
        }
    }
    if (!d->tex || d->tex_w != f->width || d->tex_h != f->height) {
        if (d->tex) SDL_DestroyTexture(d->tex);
        d->tex = SDL_CreateTexture(d->ren, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING,
                                   f->width, f->height);
        d->tex_w = f->width;
        d->tex_h = f->height;
    }
    SDL_UpdateYUVTexture(d->tex, NULL, f->data[0], f->linesize[0],
                         f->data[1], f->linesize[1], f->data[2], f->linesize[2]);

    SDL_Rect src = { 0, 0, active_w ? active_w : f->width, active_h ? active_h : f->height };
    if (src.w > f->width) src.w = f->width;
    if (src.h > f->height) src.h = f->height;

    /* The destination keeps the CODED frame's aspect, not the active rect's. The downshift tiers
     * (1088x1920, 720x1280) are 16:9-ish while the screen is ~0.46, because RVRA resamples the
     * whole picture with independent x/y factors -- a downshifted frame is an anamorphic squeeze
     * of the full screen. Stretching the crop back to the coded aspect restores proportions, and
     * a tier change alters only sharpness, never the window geometry. */
    int ww, wh;
    SDL_GetRendererOutputSize(d->ren, &ww, &wh);
    SDL_Rect dst;
    if ((int64_t)ww * f->height > (int64_t)wh * f->width) {   /* window wider than the picture: pillarbox */
        dst.w = wh * f->width / f->height; dst.h = wh; dst.x = (ww - dst.w) / 2; dst.y = 0;
    } else {
        dst.w = ww; dst.h = ww * f->height / f->width; dst.x = 0; dst.y = (wh - dst.h) / 2;
    }
    SDL_SetRenderDrawColor(d->ren, 0, 0, 0, 255);
    SDL_RenderClear(d->ren);
    SDL_RenderCopy(d->ren, d->tex, &src, &dst);
    SDL_RenderPresent(d->ren);
}

/* --dump: write every decoded frame as y4m (full range, full coded size), so the client's own
 * end-to-end path — parser, access units, patched decoder — can be scored against the
 * VideoToolbox ground truth with scripts/compare-decodes.py. */
static FILE *g_dump;
static int g_dump_started;

static void dump_frame(AVFrame *f, int active_w, int active_h)
{
    (void)active_w; (void)active_h;
    if (!g_dump_started) {
        g_dump_started = 1;
        fprintf(g_dump, "YUV4MPEG2 W%d H%d F60:1 Ip A1:1 C420jpeg XYSCSS=420JPEG"
                        " XCOLORRANGE=FULL\n", f->width, f->height);
    }
    fprintf(g_dump, "FRAME\n");
    for (int p = 0; p < 3; p++) {
        int w = p ? (f->width + 1) / 2 : f->width;
        int h = p ? (f->height + 1) / 2 : f->height;
        for (int y = 0; y < h; y++)
            fwrite(f->data[p] + (ptrdiff_t)y * f->linesize[p], 1, w, g_dump);
    }
}

static void poll_events(void)
{
    if (!g_disp.win) return;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT ||
            (e.type == SDL_KEYDOWN &&
             (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_q)))
            g_quit = 1;
    }
}

/* ------------------------------------------------------------------ transport */

static int tcp_connect(const char *host, int port)
{
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        fprintf(stderr, "%s: not an IPv4 address (spike limitation)\n", host);
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        fprintf(stderr, "connect %s:%d: %s\n", host, port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* Ask 9876 which codec the device negotiated. Best effort: on any failure the answer is "hevc",
 * which is what the device picks when offered both. One line of JSON out, one line back; the
 * value is found with strstr because the reply is our own engine's flat JSON, not the internet's. */
static int stream_says_h264(const char *host, int api_port)
{
    int fd = tcp_connect(host, api_port);
    if (fd < 0) return 0;
    struct timeval tv = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    const char req[] = "{\"id\":1,\"method\":\"stream_info\"}\n";
    if (send(fd, req, sizeof req - 1, 0) != (ssize_t)(sizeof req - 1)) { close(fd); return 0; }
    char reply[2048];
    ssize_t got = 0, n;
    while (got < (ssize_t)sizeof reply - 1 &&
           (n = recv(fd, reply + got, sizeof reply - 1 - got, 0)) > 0) {
        got += n;
        if (memchr(reply, '\n', (size_t)got)) break;
    }
    close(fd);
    reply[got > 0 ? got : 0] = 0;
    return strstr(reply, "\"codec\":\"h264\"") != NULL;
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1", *file = NULL, *codec_arg = NULL, *dump_path = NULL;
    int port = 9877, check = 0;
    double fps = 60;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) host = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) file = argv[++i];
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) fps = atof(argv[++i]);
        else if (!strcmp(argv[i], "--codec") && i + 1 < argc) codec_arg = argv[++i];
        else if (!strcmp(argv[i], "--check")) check = 1;
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) { dump_path = argv[++i]; check = 1; }
        else if (argv[i][0] != '-') host = argv[i];
        else {
            fprintf(stderr, "usage: rplay-view [-s host] [-p port] [-f file.h265] [-r fps]"
                            " [--codec hevc|h264] [--check] [--dump out.y4m]\n");
            return 2;
        }
    }

    if (codec_arg) g_h264 = !strcmp(codec_arg, "h264");
    else if (!file) g_h264 = stream_says_h264(host, 9876);

    /* Arm RVRA reference resampling in the linked libavcodec (a no-op on stock ffmpeg, which
     * just never reads the variable). Respect an explicit setting, so RPLAY_RVRA=0 disables. */
    if (!g_h264)
        setenv("RPLAY_RVRA", "1", 0);

    stream_state s = { 0 };
    s.awaiting_keyframe = 1;
    if (dump_path) {
        g_dump = fopen(dump_path, "wb");
        if (!g_dump) { fprintf(stderr, "%s: %s\n", dump_path, strerror(errno)); return 1; }
        s.on_frame = dump_frame;
    } else {
        s.on_frame = check ? NULL : show_frame;
    }

    const AVCodec *codec = avcodec_find_decoder(g_h264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC);
    if (!codec) { fprintf(stderr, "libavcodec has no %s decoder\n", g_h264 ? "h264" : "hevc"); return 1; }
    s.dec = avcodec_alloc_context3(codec);
    /* One thread, deliberately. Frame-threading adds a frame of latency per thread, and screen
     * content decodes at 13x realtime on a single thread (measured; see LINUX-PORT-HANDOFF.md) —
     * latency is the scarce resource here, not throughput. */
    s.dec->thread_count = 1;
    if (avcodec_open2(s.dec, codec, NULL) != 0) { fprintf(stderr, "cannot open decoder\n"); return 1; }

    FILE *in = NULL;
    int fd = -1;
    if (file) {
        in = fopen(file, "rb");
        if (!in) { fprintf(stderr, "%s: %s\n", file, strerror(errno)); return 1; }
    } else {
        fd = tcp_connect(host, port);
        if (fd < 0) return 1;
        /* Short receive timeout, so the event loop keeps running and an idle-complete picture
         * can be released: the newest frame would otherwise wait for the NEXT picture's first
         * slice, which after motion stops never comes. Same 20 ms rule as the macOS app. */
        struct timeval tv = { 0, 100 * 1000 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        fprintf(stderr, "connected to %s:%d (%s)\n", host, port, g_h264 ? "h264" : "hevc");
    }

    annexb_parser parser = { 0 };
    uint8_t chunk[65536];
    uint64_t frame_interval_ms = fps > 0 ? (uint64_t)(1000.0 / fps) : 0;

    while (!g_quit) {
        if (in) {
            size_t n = fread(chunk, 1, sizeof chunk, in);
            if (n == 0) break;
            uint64_t before = s.frames_submitted;
            annexb_feed(&parser, chunk, n, handle_nal, &s);
            if (!check && frame_interval_ms) {
                uint64_t submitted = s.frames_submitted - before;
                for (uint64_t k = 0; k < submitted && !g_quit; k++) {
                    poll_events();
                    SDL_Delay((Uint32)frame_interval_ms);
                }
            }
        } else {
            ssize_t n = recv(fd, chunk, sizeof chunk, 0);
            if (n > 0) {
                annexb_feed(&parser, chunk, (size_t)n, handle_nal, &s);
            } else if (n == 0) {
                fprintf(stderr, "stream closed\n");
                break;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                if (s.au_len && now_ms() - s.last_nal_ms >= 20) flush_au(&s);
            } else {
                fprintf(stderr, "recv: %s\n", strerror(errno));
                break;
            }
        }
        poll_events();
    }
    annexb_finish(&parser, handle_nal, &s);
    flush_au(&s);

    /* Drain the decoder so the tail of a capture is counted. */
    avcodec_send_packet(s.dec, NULL);
    AVFrame *f = av_frame_alloc();
    while (f && avcodec_receive_frame(s.dec, f) == 0) {
        s.frames_decoded++;
        if (s.on_frame) s.on_frame(f, s.active_w, s.active_h);
        av_frame_unref(f);
    }
    av_frame_free(&f);

    fprintf(stderr, "nals %llu, frames submitted %llu, decoded %llu, decode errors %llu, "
                    "trailers %llu, dropped before keyframe %llu, last active rect %dx%d\n",
            (unsigned long long)s.nals, (unsigned long long)s.frames_submitted,
            (unsigned long long)s.frames_decoded, (unsigned long long)s.decode_errors,
            (unsigned long long)s.trailers, (unsigned long long)s.frames_before_keyframe,
            s.active_w, s.active_h);

    if (g_dump) fclose(g_dump);
    if (in) fclose(in);
    if (fd >= 0) close(fd);
    avcodec_free_context(&s.dec);
    if (g_disp.tex) SDL_DestroyTexture(g_disp.tex);
    if (g_disp.ren) SDL_DestroyRenderer(g_disp.ren);
    if (g_disp.win) { SDL_DestroyWindow(g_disp.win); SDL_Quit(); }
    free(parser.buf);
    free(s.au);
    return s.decode_errors ? 1 : 0;
}
