/*
 * Tests for the portable video path. Synthetic cases always run; if a real recording is passed
 * on the command line it is parsed too, which is the test that matters most — the synthetic
 * cases only prove the code does what I think, real device output proves what I think is right.
 *
 *   ./test_rp_video_stream [file.h265] [file.h264]
 */
#include "rp_video_stream.h"
#include "rp_geometry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("  FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); \
                   printf("\n"); failures++; } \
} while (0)

typedef struct {
    int    param_calls;
    size_t param_sets;
    int    au_calls;
    int    keyframes;
    size_t last_au_nals;
    size_t last_packed;
} counters;

static void on_params(void *ctx, const rp_nal *sets, size_t count, rp_codec codec)
{
    (void)codec;
    counters *c = (counters *)ctx;
    c->param_calls++;
    c->param_sets = count;
    for (size_t i = 0; i < count; i++)
        if (!sets[i].data || !sets[i].size) { printf("  FAIL empty parameter set\n"); failures++; }
}

static void on_au(void *ctx, const rp_nal *nals, size_t count, int is_keyframe)
{
    counters *c = (counters *)ctx;
    c->au_calls++;
    c->last_au_nals = count;
    if (is_keyframe) c->keyframes++;
    /* Both packings must round-trip to the same length. */
    size_t need_lp = rp_pack_length_prefixed(nals, count, NULL, 0);
    size_t need_ab = rp_pack_annexb(nals, count, NULL, 0);
    if (need_lp != need_ab) { printf("  FAIL packing sizes differ\n"); failures++; }
    uint8_t *buf = (uint8_t *)malloc(need_lp);
    c->last_packed = rp_pack_length_prefixed(nals, count, buf, need_lp);
    if (c->last_packed != need_lp) { printf("  FAIL short pack\n"); failures++; }
    free(buf);
}

/* Build one Annex-B NAL with the given codec-specific type. */
static size_t make_nal(uint8_t *out, int type, rp_codec codec, int first_slice, size_t payload)
{
    size_t i = 0;
    out[i++] = 0; out[i++] = 0; out[i++] = 0; out[i++] = 1;
    if (codec == RP_CODEC_H264) {
        out[i++] = (uint8_t)(type & 0x1F);
    } else {
        out[i++] = (uint8_t)((type & 0x3F) << 1);
        out[i++] = 1;
    }
    out[i++] = first_slice ? 0x80 : 0x00;        /* first-slice flag lives here */
    for (size_t p = 0; p < payload; p++) out[i++] = (uint8_t)(0xA0 + p);
    return i;
}

static void test_classification(void)
{
    CHECK(rp_nal_is_parameter_set(32, RP_CODEC_HEVC), "HEVC VPS");
    CHECK(rp_nal_is_parameter_set(34, RP_CODEC_HEVC), "HEVC PPS");
    CHECK(!rp_nal_is_parameter_set(7, RP_CODEC_HEVC), "7 is not an HEVC parameter set");
    CHECK(rp_nal_is_parameter_set(7, RP_CODEC_H264), "H.264 SPS");
    CHECK(rp_nal_is_parameter_set(8, RP_CODEC_H264), "H.264 PPS");
    CHECK(rp_nal_is_keyframe(19, RP_CODEC_HEVC), "HEVC IDR_W_RADL");
    CHECK(rp_nal_is_keyframe(5, RP_CODEC_H264), "H.264 IDR");
    CHECK(!rp_nal_is_keyframe(1, RP_CODEC_H264), "H.264 non-IDR is not a keyframe");
    CHECK(!rp_nal_is_keyframe(5, RP_CODEC_HEVC), "type 5 is not an HEVC keyframe");
    printf("  classification: both codecs' NAL semantics — OK\n");
}

static void test_keyframe_gate(rp_codec codec, const char *name)
{
    counters c; memset(&c, 0, sizeof c);
    rp_video_stream *s = rp_video_stream_create(codec);
    rp_video_stream_set_callbacks(s, &c, on_params, on_au);

    uint8_t buf[512];
    size_t n;
    int ps[3], ps_count, vcl_p, vcl_key;
    if (codec == RP_CODEC_H264) {
        ps[0] = 7; ps[1] = 8; ps_count = 2; vcl_p = 1; vcl_key = 5;
    } else {
        ps[0] = 32; ps[1] = 33; ps[2] = 34; ps_count = 3; vcl_p = 1; vcl_key = 19;
    }

    /* Parameter sets, then P-frames with no keyframe: everything must be dropped.
     *
     * Note the timing: a NAL is only complete once the NEXT start code arrives, so the last
     * parameter set is not announced until a following NAL (or flush) turns up. That is fine on a
     * live stream, where frames follow immediately — but it is why this check comes after the
     * P-frames rather than straight after the parameter sets. */
    for (int i = 0; i < ps_count; i++) {
        n = make_nal(buf, ps[i], codec, 0, 8);
        rp_video_stream_feed(s, buf, n);
    }
    for (int i = 0; i < 5; i++) {
        n = make_nal(buf, vcl_p, codec, 1, 16);
        rp_video_stream_feed(s, buf, n);
    }
    rp_video_stream_flush(s);
    CHECK(c.param_calls == 1, "%s: parameter sets announced once, got %d", name, c.param_calls);
    CHECK(c.param_sets == (size_t)ps_count, "%s: %zu sets", name, c.param_sets);
    CHECK(c.au_calls == 0, "%s: no access unit before a keyframe, got %d", name, c.au_calls);
    CHECK(rp_video_stream_is_awaiting_keyframe(s), "%s: still awaiting", name);
    CHECK(rp_video_stream_stats(s).frames_before_keyframe == 5,
          "%s: counted 5 dropped, got %llu", name,
          (unsigned long long)rp_video_stream_stats(s).frames_before_keyframe);

    /* Keyframe opens the gate; following P-frames now flow. */
    n = make_nal(buf, vcl_key, codec, 1, 16);
    rp_video_stream_feed(s, buf, n);
    n = make_nal(buf, vcl_p, codec, 1, 16);
    rp_video_stream_feed(s, buf, n);
    rp_video_stream_flush(s);
    CHECK(c.au_calls == 2, "%s: two access units after the keyframe, got %d", name, c.au_calls);
    CHECK(c.keyframes == 1, "%s: one keyframe, got %d", name, c.keyframes);
    CHECK(!rp_video_stream_is_awaiting_keyframe(s), "%s: gate open", name);

    /* Re-arming (what a decode failure does) drops frames again. */
    rp_video_stream_await_keyframe(s);
    int before = c.au_calls;
    n = make_nal(buf, vcl_p, codec, 1, 16);
    rp_video_stream_feed(s, buf, n);
    rp_video_stream_flush(s);
    CHECK(c.au_calls == before, "%s: re-armed gate drops again", name);

    rp_video_stream_destroy(s);
    printf("  keyframe gate (%s): drop, open, re-arm — OK\n", name);
}

static void test_multi_slice_grouping(void)
{
    counters c; memset(&c, 0, sizeof c);
    rp_video_stream *s = rp_video_stream_create(RP_CODEC_HEVC);
    rp_video_stream_set_callbacks(s, &c, on_params, on_au);

    uint8_t buf[512];
    for (int t = 32; t <= 34; t++) {
        size_t n = make_nal(buf, t, RP_CODEC_HEVC, 0, 8);
        rp_video_stream_feed(s, buf, n);
    }
    /* keyframe first slice + a continuation slice = ONE picture */
    size_t n = make_nal(buf, 19, RP_CODEC_HEVC, 1, 16); rp_video_stream_feed(s, buf, n);
    n = make_nal(buf, 19, RP_CODEC_HEVC, 0, 16);        rp_video_stream_feed(s, buf, n);
    /* next first slice starts a new picture */
    n = make_nal(buf, 1, RP_CODEC_HEVC, 1, 16);         rp_video_stream_feed(s, buf, n);
    rp_video_stream_flush(s);

    CHECK(c.au_calls == 2, "two pictures from three slices, got %d", c.au_calls);
    CHECK(c.last_au_nals == 1, "second picture has 1 NAL, got %zu", c.last_au_nals);
    rp_video_stream_destroy(s);
    printf("  access units: multi-slice pictures grouped as one — OK\n");
}

static void test_chunk_boundaries(void)
{
    /* The same bytes fed one at a time must produce identical results: the parser has to cope
     * with a start code split across reads, which is the normal case on a socket. */
    uint8_t stream[2048];
    size_t len = 0;
    for (int t = 32; t <= 34; t++) len += make_nal(stream + len, t, RP_CODEC_HEVC, 0, 8);
    len += make_nal(stream + len, 19, RP_CODEC_HEVC, 1, 40);
    for (int i = 0; i < 6; i++) len += make_nal(stream + len, 1, RP_CODEC_HEVC, 1, 40);

    counters whole; memset(&whole, 0, sizeof whole);
    rp_video_stream *a = rp_video_stream_create(RP_CODEC_HEVC);
    rp_video_stream_set_callbacks(a, &whole, on_params, on_au);
    rp_video_stream_feed(a, stream, len);
    rp_video_stream_flush(a);

    counters drip; memset(&drip, 0, sizeof drip);
    rp_video_stream *b = rp_video_stream_create(RP_CODEC_HEVC);
    rp_video_stream_set_callbacks(b, &drip, on_params, on_au);
    for (size_t i = 0; i < len; i++) rp_video_stream_feed(b, stream + i, 1);
    rp_video_stream_flush(b);

    CHECK(whole.au_calls == drip.au_calls,
          "byte-at-a-time matches: %d vs %d", whole.au_calls, drip.au_calls);
    CHECK(whole.param_calls == drip.param_calls, "parameter sets match");
    CHECK(drip.au_calls == 7, "7 pictures, got %d", drip.au_calls);
    rp_video_stream_destroy(a);
    rp_video_stream_destroy(b);
    printf("  chunking: byte-at-a-time == one big feed (%d pictures) — OK\n", drip.au_calls);
}

static void test_real_file(const char *path, rp_codec codec, const char *name)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("  skip %s (%s not readable)\n", name, path); return; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) {
        printf("  skip %s (read failed)\n", name); free(data); fclose(f); return;
    }
    fclose(f);

    counters c; memset(&c, 0, sizeof c);
    rp_video_stream *s = rp_video_stream_create(codec);
    rp_video_stream_set_callbacks(s, &c, on_params, on_au);
    /* Feed in socket-sized chunks, as the app would. */
    const size_t chunk = 4096;
    for (long off = 0; off < size; off += (long)chunk) {
        size_t n = (size_t)(size - off) < chunk ? (size_t)(size - off) : chunk;
        rp_video_stream_feed(s, data + off, n);
    }
    rp_video_stream_flush(s);

    rp_video_stats st = rp_video_stream_stats(s);
    printf("  %s: %d pictures, %d keyframes, %llu NALs, %ld bytes\n",
           name, c.au_calls, c.keyframes, (unsigned long long)st.nals_seen, size);
    CHECK(c.param_calls >= 1, "%s: parameter sets found", name);
    CHECK(c.au_calls > 100, "%s: decoded a plausible number of pictures (%d)", name, c.au_calls);
    CHECK(c.keyframes >= 1, "%s: at least one keyframe", name);
    rp_video_stream_destroy(s);
    free(data);
}

static int nearly(double a, double b) { double d = a - b; return d < 1e-9 && d > -1e-9; }

static void test_geometry(void)
{
    /* The real numbers from the device: a 1170x2532 screen inside a 1184x2576 coded frame. */
    rp_size video  = { 1184.0, 2576.0 };
    rp_size device = { 1170.0, 2532.0 };

    rp_size f = rp_visible_fraction(video, device);
    CHECK(f.w < 1.0 && f.h < 1.0, "padding means the visible fraction is under 1");
    CHECK(nearly(f.w, 1170.0 / 1184.0), "width fraction");

    /* An unknown device size must show everything rather than crop by guesswork. */
    rp_size none = { 0.0, 0.0 };
    rp_size all = rp_visible_fraction(video, none);
    CHECK(nearly(all.w, 1.0) && nearly(all.h, 1.0), "unknown device size shows the whole frame");

    /* Aspect-fit into a wider-than-tall window letterboxes horizontally. */
    rp_size view = { 800.0, 900.0 };
    rp_rect screen = rp_fit_rect(view, device);
    CHECK(screen.h <= 900.0 + 1e-9 && screen.w <= 800.0 + 1e-9, "fits inside the view");
    CHECK(nearly(screen.w / screen.h, device.w / device.h), "preserves the device aspect ratio");
    CHECK(screen.x > 0.0, "centred horizontally, so there are side bars");

    /* Centre of the screen rectangle is the centre of the device, in either convention. */
    double fx = -1, fy = -1;
    CHECK(rp_normalize_point(screen.x + screen.w / 2, screen.y + screen.h / 2,
                             screen, 1, &fx, &fy), "centre is inside");
    CHECK(nearly(fx, 0.5) && nearly(fy, 0.5), "centre maps to 0.5,0.5 got %f,%f", fx, fy);

    /* y inverts for a bottom-left origin: the top of the screen is fy=0. */
    CHECK(rp_normalize_point(screen.x + 1, screen.y + screen.h - 0.0001, screen, 0, &fx, &fy),
          "top edge inside (bottom-left origin)");
    CHECK(fy < 0.001, "bottom-left origin: top of screen is fy~0, got %f", fy);
    CHECK(rp_normalize_point(screen.x + 1, screen.y + 0.0001, screen, 1, &fx, &fy),
          "top edge inside (top-left origin)");
    CHECK(fy < 0.001, "top-left origin: top of screen is fy~0, got %f", fy);

    /* A click on the letterbox is not a tap. */
    CHECK(!rp_normalize_point(screen.x - 5, screen.y + 10, screen, 1, &fx, &fy),
          "outside the screen rectangle is rejected");

    /* The layer frame is bigger than the clip, pushing the padding out of view. */
    rp_rect layer = rp_video_layer_frame(screen, video, device, 1);
    CHECK(layer.w > screen.w && layer.h > screen.h, "layer overhangs so padding is clipped");
    CHECK(nearly(layer.y, 0.0), "top-left origin anchors the top edge at 0");
    rp_rect flipped = rp_video_layer_frame(screen, video, device, 0);
    CHECK(flipped.y < 0.0, "bottom-left origin anchors the top edge negatively");

    /* HID conversion clamps, because the report masks to 16 bits. */
    unsigned hx = 0, hy = 0;
    rp_fractions_to_hid(0.5, 0.5, &hx, &hy);
    CHECK(hx == 32767 && hy == 32767, "0.5 -> 32767, got %u,%u", hx, hy);
    rp_fractions_to_hid(5.0, -1.0, &hx, &hy);
    CHECK(hx == 65535 && hy == 0, "out of range clamps, got %u,%u", hx, hy);

    double px = 0, py = 0;
    rp_fractions_to_pixels(0.5, 0.5, device, &px, &py);
    CHECK(nearly(px, 585.0) && nearly(py, 1266.0), "centre in device pixels, got %f,%f", px, py);
    printf("  geometry: crop fraction, letterbox fit, y convention, HID clamp — OK\n");
}

int main(int argc, char **argv)
{
    printf("core/rp_video_stream tests\n");
    test_classification();
    test_keyframe_gate(RP_CODEC_HEVC, "hevc");
    test_keyframe_gate(RP_CODEC_H264, "h264");
    test_multi_slice_grouping();
    test_chunk_boundaries();
    test_geometry();
    if (argc > 1) test_real_file(argv[1], RP_CODEC_HEVC, "real HEVC recording");
    if (argc > 2) test_real_file(argv[2], RP_CODEC_H264, "real H.264 recording");

    if (failures) { printf("%d check(s) FAILED\n", failures); return 1; }
    printf("all checks passed\n");
    return 0;
}
