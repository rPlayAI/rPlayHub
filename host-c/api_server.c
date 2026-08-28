#include "api_server.h"

#include <plist/plist.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>

#include "../core/rp_coredevice.h"
#include "media.h"
#include "../core/rp_xpc.h"
#include "ddi.h"
#include "usernet.h"

/* Big enough for the largest reply we ask for, which is a full-screen PNG.
 *
 * rp_rxpc_recv splits this in half, one accumulator per HTTP/2 stream, so the usable size for a
 * single reply is half of this. A 1170x2532 screenshot has run past 4 MB, so 32 MB leaves real
 * headroom rather than the next round number up. */
#define SVC_REASSEMBLY (32u << 20)

static api_session *g_session;

/* The live stream and its viewers. Declared here because stream_info reports on them and is
 * defined above the fan-out that owns them. */
static media_session *g_media;
static int viewer_count;
/* NALs dropped because a viewer could not keep up. Declared here because
 * stream_info reports it and is defined above the fan-out that maintains it. */
static uint64_t viewer_drops_total;

/* ------------------------------------------------------------------ tiny JSON helpers
 *
 * Deliberately minimal rather than a JSON library. The requests this serves are a handful of
 * flat objects with known keys, and every byte of them comes from our own app; a parser that
 * handles exactly that and refuses anything else is less code AND a smaller attack surface than
 * a general one.
 */
static int json_string_field(const char *json, const char *key, char *out, size_t out_cap)
{
    char pattern[64];
    snprintf(pattern, sizeof pattern, "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return -1;
    p = strchr(p + strlen(pattern), ':');
    if (!p) return -1;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_cap) out[i++] = *p++;
    out[i] = 0;
    return 0;
}

static long json_number_field(const char *json, const char *key, long fallback)
{
    char pattern[64];
    snprintf(pattern, sizeof pattern, "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return fallback;
    p = strchr(p + strlen(pattern), ':');
    if (!p) return fallback;
    return strtol(p + 1, NULL, 10);
}

/* A fractional field. Separate from json_number_field because these are the only non-integer
 * values in the protocol and parsing them as longs silently truncates every tap to 0 or 1. */
static double json_fraction(const char *json, const char *key, double fallback)
{
    char pattern[64];
    snprintf(pattern, sizeof pattern, "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return fallback;
    p = strchr(p + strlen(pattern), ':');
    if (!p) return fallback;
    return atof(p + 1);
}

/* Escapes a string for embedding as PCDATA in the classic plists sent over the shim channels --
 * bundle ids, paths and profile identifiers are our own JSON fields, but a stray '&' or '<' would
 * otherwise corrupt the XML the device parses. */
static void xml_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 6 < cap; p++) {
        switch (*p) {
        case '&': memcpy(out + o, "&amp;", 5);  o += 5; break;
        case '<': memcpy(out + o, "&lt;", 4);   o += 4; break;
        case '>': memcpy(out + o, "&gt;", 4);   o += 4; break;
        case '"': memcpy(out + o, "&quot;", 6); o += 6; break;
        default:  out[o++] = (char)*p;
        }
    }
    out[o] = 0;
}

static void send_line(int fd, const char *fmt, ...)
{
    char buf[8192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    buf[n++] = '\n';
    ssize_t off = 0;
    while (off < n) {
        ssize_t w = send(fd, buf + off, (size_t)(n - off), 0);
        if (w <= 0) return;
        off += w;
    }
}

static void reply_error(int fd, long id, const char *code, const char *message)
{
    send_line(fd, "{\"id\":%ld,\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
              id, code, message);
}


static long svc_read(void *ctx, void *buf, size_t len)  { return tun_read(*(int *)ctx, buf, len); }
static long svc_write(void *ctx, const void *buf, size_t len) { return tun_write(*(int *)ctx, buf, len); }

/* ------------------------------------------------------------------ talking to a service
 *
 * Every coredevice.* feature lives on its own RSD-discovered port inside the tunnel, so a call
 * means: open a socket there, run the RemoteXPC opening exchange, send, read. Sessions are opened
 * per call rather than held: the services are stateless for our purposes, and a socket that has
 * been idle across a device sleep is a worse failure than a connect.
 */
typedef struct {
    int fd;
    rp_rxpc_session s;
    uint8_t *reassembly;
    uint8_t *raw;
} svc_conn;

/* Buffers are per connection, not shared: each client runs on its own thread, so a single static
 * reassembly buffer would let two concurrent calls scribble over each other's replies. */
static int svc_open(svc_conn *c, const char *addr, long port)
{
    c->fd = -1;
    /* 8 MB, not 1 MB. rp_rxpc_recv splits this in half (one accumulator per HTTP/2 stream), and
     * a screenshot of a 1170x2532 screen is a multi-megabyte PNG -- it simply did not fit, and
     * the overflow path discarded it without a word. */
    c->reassembly = malloc(SVC_REASSEMBLY);
    c->raw = malloc(1 << 16);
    if (!c->reassembly || !c->raw) { free(c->reassembly); free(c->raw); return -1; }

    /* Connect through the tunnel (kernel socket, or lwIP when userspace mode is on). */
    c->fd = tun_connect(addr, (int)port, 3);
    if (c->fd < 0) goto fail;

    rp_rxpc_io io = { svc_read, svc_write, &c->fd };
    rp_rxpc_init(&c->s, io, c->reassembly, SVC_REASSEMBLY, c->raw, 1 << 16);
    if (rp_rxpc_handshake(&c->s) != 0) goto fail;
    return 0;

fail:
    if (c->fd >= 0) tun_close(c->fd);
    c->fd = -1;
    free(c->reassembly); c->reassembly = NULL;
    free(c->raw); c->raw = NULL;
    return -1;
}

static void svc_close(svc_conn *c)
{
    if (c->fd >= 0) tun_close(c->fd);
    c->fd = -1;
    free(c->reassembly); c->reassembly = NULL;
    free(c->raw); c->raw = NULL;
}

/* A UUID string, which the envelope needs two of. Not cryptographic -- the device only echoes
 * them -- so a counter plus the clock is enough and keeps this dependency-free. */
static void make_uuid(char out[37])
{
    static unsigned long counter;
    unsigned long a = (unsigned long)time(NULL), b = ++counter;
    snprintf(out, 37, "%08lx-%04lx-4%03lx-8%03lx-%012lx",
             a & 0xFFFFFFFF, (b >> 16) & 0xFFFF, b & 0xFFF, (a >> 8) & 0xFFF,
             (a ^ (b << 8)) & 0xFFFFFFFFFFFFUL);
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64_encode(const uint8_t *in, size_t n, char *out, size_t cap)
{
    size_t need = ((n + 2) / 3) * 4;
    if (need + 1 > cap) return 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? B64[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? B64[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

/* ------------------------------------------------------------------ screenshot */

static void method_screenshot(int fd, long id)
{
    const api_session *sess = g_session;
    if (!sess->screenshot_port) {
        reply_error(fd, id, "unavailable", "the device did not offer screencaptureservice");
        return;
    }
    svc_conn c;
    if (svc_open(&c, sess->tunnel_addr, sess->screenshot_port) != 0) {
        reply_error(fd, id, "unavailable", "cannot reach screencaptureservice");
        return;
    }

    uint8_t input[128];
    rp_xpc_writer w;
    rp_xpc_writer_init(&w, input, sizeof input);
    rp_xpc_dict_begin(&w);
    rp_xpc_key(&w, "displayUniqueID");
    rp_xpc_null(&w);
    rp_xpc_set_string(&w, "requestedFormat", "png");
    rp_xpc_dict_end(&w);

    char ua[37], ub[37];
    make_uuid(ua); make_uuid(ub);
    rp_xpc_obj out, reply;
    memset(&reply, 0, sizeof reply);
    int rc = rp_cd_invoke(&c.s, RP_CD_FEATURE_SCREENSHOT, RP_CD_ACTION_SCREENSHOT,
                          input, w.len, ua, ub, &out, &reply);
    if (rc != 0) {
        const char *stage = rc == RP_CD_ERR_BUILD ? "could not build the request"
                          : rc == RP_CD_ERR_SEND ? "could not send the request"
                          : rc == RP_CD_ERR_NO_REPLY ? "the service never answered"
                          : "the answer carried no CoreDevice.output";
        fprintf(stderr, "  screenshot failed: %s\n", stage);
        /* Say what came back. Every failure on this path so far has been diagnosed by the reply
         * we were throwing away. */
        if (reply.data) {
            const char *k = NULL;
            rp_xpc_obj v;
            size_t cursor = 0;
            fprintf(stderr, "  screenshot reply keys:");
            while (rp_xpc_dict_next(&reply, &cursor, &k, &v) == 0 && cursor <= 12) {
                fprintf(stderr, " %s", k);
                const char *sv = NULL;
                if (rp_xpc_get_string(&v, &sv) == 0) fprintf(stderr, "=%.80s", sv);
            }
            fprintf(stderr, "\n");
        } else {
            fprintf(stderr, "  screenshot: no reply object at all\n");
        }
        int over = c.s.overflowed;
        size_t needed = c.s.overflow_needed;
        svc_close(&c);
        if (over) {
            char msg[160];
            snprintf(msg, sizeof msg,
                     "the screenshot needed at least %zu bytes but the buffer holds %u",
                     needed, SVC_REASSEMBLY / 2);
            reply_error(fd, id, "device_error", msg);
        } else {
            reply_error(fd, id, "device_error", stage);
        }
        return;
    }

    rp_xpc_obj img;
    const uint8_t *bytes = NULL;
    size_t n = 0;
    if (rp_xpc_dict_get(&out, "image", &img) != 0 || rp_xpc_get_data(&img, &bytes, &n) != 0) {
        svc_close(&c);
        reply_error(fd, id, "device_error", "no image in the reply");
        return;
    }

    /* PNG dimensions come straight out of the IHDR, which is always the first chunk. */
    int wpx = 0, hpx = 0;
    if (n >= 24 && !memcmp(bytes, "\x89PNG\r\n\x1a\n", 8)) {
        wpx = (bytes[16] << 24) | (bytes[17] << 16) | (bytes[18] << 8) | bytes[19];
        hpx = (bytes[20] << 24) | (bytes[21] << 16) | (bytes[22] << 8) | bytes[23];
        /* The screenshot is the one place the TRUE screen size shows up (the video is coded with
         * 16-pixel alignment padding, 1184x2576 for a 1170x2532 screen). Remember it: clients map
         * clicks and crop the mirror against list_devices' screen_width/height. */
        if (wpx > 0 && hpx > 0 && g_session) {
            g_session->screen_w = wpx;
            g_session->screen_h = hpx;
        }
    }

    /* Heap, not static: two clients screenshotting at once would share one buffer. */
    size_t b64cap = ((n + 2) / 3) * 4 + 1;
    char *b64 = malloc(b64cap);
    size_t bn = b64 ? b64_encode(bytes, n, b64, b64cap) : 0;
    svc_close(&c);
    if (!bn) { free(b64); reply_error(fd, id, "internal_error", "could not encode the screenshot"); return; }

    /* Written directly rather than through send_line: the payload is megabytes and would not fit
     * that function's stack buffer. */
    char head[256];
    int hn = snprintf(head, sizeof head,
                      "{\"id\":%ld,\"ok\":true,\"result\":{\"format\":\"png\",\"width\":%d,"
                      "\"height\":%d,\"image_b64\":\"", id, wpx, hpx);
    send(fd, head, (size_t)hn, 0);
    size_t off = 0;
    while (off < bn) {
        ssize_t wr = send(fd, b64 + off, bn - off, 0);
        if (wr <= 0) break;
        off += (size_t)wr;
    }
    free(b64);
    send(fd, "\"}}\n", 4, 0);
}

/* ------------------------------------------------------------------ touch */

#define HID_FEATURE       "com.apple.coredevice.feature.remote.universalhidservice"
#define HID_MAIN_SURFACE  257
#define TS_REPORT_ID      0x09
#define TS_STATE_CONTACT  0xC2
#define TS_STATE_RELEASE  0x02

/* The 58-byte mainTouchscreen report. x and y are normalised across the screen as UInt16, so the
 * caller never needs the device's pixel dimensions. */
static void touch_report(uint8_t out[58], uint8_t state, uint16_t x, uint16_t y)
{
    memset(out, 0, 58);
    out[0] = TS_REPORT_ID; out[1] = 0x01; out[2] = 0x05; out[3] = state;
    out[4] = (uint8_t)(x & 0xFF); out[5] = (uint8_t)(x >> 8);
    out[6] = (uint8_t)(y & 0xFF); out[7] = (uint8_t)(y >> 8);
    out[40] = 0x02;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t stamp = ((uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec)
                     & ((1ULL << 48) - 1);
    for (int i = 0; i < 6; i++) out[44 + i] = (uint8_t)(stamp >> (8 * i));
}

static int hid_send(rp_rxpc_session *s, const uint8_t report[58])
{
    uint8_t body[512];
    rp_xpc_writer w;
    rp_xpc_writer_init(&w, body, sizeof body);
    rp_xpc_dict_begin(&w);
    rp_xpc_set_string(&w, "featureIdentifier", HID_FEATURE);
    rp_xpc_set_string(&w, "messageType", "Request");
    rp_xpc_key(&w, "payload");
    rp_xpc_dict_begin(&w);
    rp_xpc_key(&w, "send");
    rp_xpc_dict_begin(&w);
    rp_xpc_key(&w, "_0");
    rp_xpc_data(&w, report, 58);
    rp_xpc_set_uint64(&w, "_1", HID_MAIN_SURFACE);
    rp_xpc_dict_end(&w);
    rp_xpc_dict_end(&w);
    rp_xpc_dict_end(&w);
    if (w.overflow) return -1;
    /* Fire and forget: touch is a stream of samples and waiting for a reply on each one would
     * pace the gesture to the round-trip time. */
    return rp_rxpc_send(s, body, w.len, 0);
}

/* Hardware-style buttons, expressed as the gestures that actually perform them.
 *
 * The device advertises a "mainScreenButtons" HID surface (_ServiceID 1026) alongside the
 * touchscreen, but its report format is not known and guessing at one risks sending the phone
 * something arbitrary. On every iPhone this code targets, Home and App Switcher ARE gestures --
 * a swipe up from the bottom edge, short for one and held for the other -- so they go through
 * the touchscreen surface whose format is verified. Verified live: an edge swipe from an open
 * app returns to the home screen.
 *
 * Buttons that are genuinely not gestures (lock, volume, Siri) are refused rather than faked. */
static void method_button(int fd, long id, const char *line)
{
    const api_session *sess = g_session;
    if (!sess->hid_port) {
        reply_error(fd, id, "unavailable", "the device did not offer universalhidservice");
        return;
    }
    char button[32] = {0};
    if (json_string_field(line, "button", button, sizeof button) != 0 || !button[0]) {
        reply_error(fd, id, "bad_request", "button is required");
        return;
    }

    /* Both start at the bottom edge; the switcher travels further and dwells. */
    double to_y;
    long duration;
    if (!strcmp(button, "home")) {
        to_y = 0.50; duration = 160;
    } else if (!strcmp(button, "app_switcher")) {
        to_y = 0.35; duration = 420;
    } else {
        reply_error(fd, id, "not_implemented",
                    "only home and app_switcher are gestures; lock, volume and siri need the "
                    "mainScreenButtons HID report format, which is not decoded yet");
        return;
    }

    svc_conn c;
    if (svc_open(&c, sess->tunnel_addr, sess->hid_port) != 0) {
        reply_error(fd, id, "unavailable", "cannot reach universalhidservice");
        return;
    }

    uint8_t report[58];
    const double from_y = 0.995, x = 0.5;
    const int steps = 20;
    for (int i = 0; i <= steps; i++) {
        double t = (double)i / (double)steps;
        uint16_t px = (uint16_t)(x * 65535.0);
        uint16_t py = (uint16_t)((from_y + (to_y - from_y) * t) * 65535.0);
        touch_report(report, TS_STATE_CONTACT, px, py);
        if (hid_send(&c.s, report) != 0) break;
        struct timespec sl = {0, (long)(duration * 1000000L / (steps + 1))};
        nanosleep(&sl, NULL);
    }
    touch_report(report, TS_STATE_RELEASE, (uint16_t)(x * 65535.0), (uint16_t)(to_y * 65535.0));
    hid_send(&c.s, report);
    svc_close(&c);

    send_line(fd, "{\"id\":%ld,\"ok\":true,\"result\":{}}", id);
}

static void method_touch(int fd, long id, const char *line, int is_swipe)
{
    const api_session *sess = g_session;
    if (!sess->hid_port) {
        reply_error(fd, id, "unavailable", "the device did not offer universalhidservice");
        return;
    }
    /* Fractions of the screen, so none of this needs the device's pixel dimensions.
     * A tap sends fx/fy; a swipe sends fx0/fy0 and fx1/fy1. */
    double fx = json_fraction(line, "fx", -1), fy = json_fraction(line, "fy", -1);
    double fx1 = fx, fy1 = fy;
    fx  = json_fraction(line, "fx0", fx);
    fy  = json_fraction(line, "fy0", fy);
    fx1 = json_fraction(line, "fx1", fx);
    fy1 = json_fraction(line, "fy1", fy);

    if (fx < 0 || fx > 1 || fy < 0 || fy > 1) {
        reply_error(fd, id, "bad_request", "fx and fy must be fractions between 0 and 1");
        return;
    }
    long duration = json_number_field(line, "duration_ms", is_swipe ? 300 : 60);

    svc_conn c;
    if (svc_open(&c, sess->tunnel_addr, sess->hid_port) != 0) {
        reply_error(fd, id, "unavailable", "cannot reach universalhidservice");
        return;
    }

    uint8_t report[58];
    int steps = is_swipe ? 20 : (int)(duration / 12);
    if (steps < 1) steps = 1;
    for (int i = 0; i <= steps; i++) {
        double t = (double)i / (double)steps;
        uint16_t x = (uint16_t)((fx + (fx1 - fx) * t) * 65535.0);
        uint16_t y = (uint16_t)((fy + (fy1 - fy) * t) * 65535.0);
        touch_report(report, TS_STATE_CONTACT, x, y);
        if (hid_send(&c.s, report) != 0) break;
        struct timespec sl = {0, (long)(duration * 1000000L / (steps + 1))};
        nanosleep(&sl, NULL);
    }
    touch_report(report, TS_STATE_RELEASE,
                 (uint16_t)(fx1 * 65535.0), (uint16_t)(fy1 * 65535.0));
    hid_send(&c.s, report);
    svc_close(&c);

    send_line(fd, "{\"id\":%ld,\"ok\":true,\"result\":{}}", id);
}

/* ------------------------------------------------------------------ classic shim services
 *
 * `*.shim.remote` services (diagnostics_relay, syslog_relay, installation_proxy ...) are NOT
 * XPC-over-HTTP2 like the ones svc_open speaks: each is a classic lockdown-style channel reached
 * by plain TCP through the tunnel, answering u32-be-length-prefixed XML plists -- the framing
 * host/lockdown.py uses over usbmux, documented since libimobiledevice's first clients. Before
 * the service protocol starts, the shim wants an RSDCheckin exchange; skipping it reads as "the
 * device ignores us" (it accepts the connection and closes it on the first real request). The
 * wire was proven against the live phone by host/diagnostics_relay.py before this was written.
 */

/* One connection per call. These requests are stateless, and a socket held across a device
 * restart is exactly the stale-session shape bug #7 punishes; nothing here is worth reusing. */
static int relay_open(const api_session *sess, long port, int read_timeout_s)
{
    (void)read_timeout_s;   /* tun_connect applies its own timeout; lwIP has no per-socket knob */
    return tun_connect(sess->tunnel_addr, (int)port, 3);
}

static int recvn(int fd, uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        long r = tun_read(fd, buf + got, n - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

/* Send one plist body (already framed by the caller) and read one reply. Replies may be XML or
 * binary plists; both carry ASCII field names verbatim, so substring matching below works on
 * either -- the same trick the Python prototype relies on via plistlib's auto-detection. */
static int relay_send(int fd, const char *xml, size_t n)
{
    uint8_t head[4] = { (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n };
    if (tun_write(fd, head, 4) != 4 || tun_write(fd, xml, n) != (ssize_t)n) return -1;
    return 0;
}

static int relay_recv(int fd, char *reply, size_t reply_cap)
{
    uint8_t len[4];
    if (recvn(fd, len, 4) != 0) return -1;
    size_t body = ((size_t)len[0] << 24) | ((size_t)len[1] << 16) | ((size_t)len[2] << 8) | len[3];
    if (body == 0 || body > reply_cap - 1) return -1;
    if (recvn(fd, (uint8_t *)reply, body) != 0) return -1;
    reply[body] = 0;
    return 0;
}

/* `extra` is zero or more already-formatted <key>..</key><value/> pairs added to the dict. */
static int relay_send_request(int fd, const char *request_name, const char *extra,
                              char *reply, size_t reply_cap)
{
    char xml[1024];
    int n = snprintf(xml, sizeof xml,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n<dict>\n\t<key>Request</key>\n\t<string>%s</string>\n%s</dict>\n</plist>\n",
        request_name, extra ? extra : "");
    if (n <= 0 || (size_t)n >= sizeof xml) return -1;
    if (relay_send(fd, xml, (size_t)n) != 0) return -1;
    return reply ? relay_recv(fd, reply, reply_cap) : 0;
}

/* RSDCheckin preamble: two plist exchanges (RSDCheckin -> echo, then an unsolicited StartService)
 * before the shim listens to the service protocol. Label and ProtocolVersion are what the proven
 * Python prototype sends; the device echoes Label back. Returns NULL on success, else a message. */
static const char *relay_checkin(int s)
{
    char reply[4096];
    if (relay_send_request(s, "RSDCheckin",
                           "\t<key>Label</key>\n\t<string>rplay-hub</string>\n"
                           "\t<key>ProtocolVersion</key>\n\t<string>2</string>\n",
                           reply, sizeof reply) != 0 || !strstr(reply, "RSDCheckin"))
        return "RSDCheckin: no/bad reply from device";
    if (relay_recv(s, reply, sizeof reply) != 0 || !strstr(reply, "StartService"))
        return "RSDCheckin: no StartService confirmation";
    if (strstr(reply, "<key>Error</key>"))
        return "RSDCheckin: StartService refused";
    return NULL;
}

/* ------------------------------------------------------------------ device power (diagnostics_relay)
 *
 * Restart / Shutdown / Sleep are the cheapest real features this daemon can grow: no negotiation,
 * one request each, reply carries {"Status": "Success"}.
 */
static void method_device_action(int fd, long id, const char *line)
{
    const api_session *sess = g_session;
    if (!sess->diag_port) {
        reply_error(fd, id, "unavailable", "the device did not offer diagnostics_relay");
        return;
    }
    char action[16] = {0};
    if (json_string_field(line, "action", action, sizeof action) != 0 || !action[0]) {
        reply_error(fd, id, "bad_request", "action is required: restart | shutdown | sleep");
        return;
    }
    const char *request = !strcmp(action, "restart")  ? "Restart"
                        : !strcmp(action, "shutdown") ? "Shutdown"
                        : !strcmp(action, "sleep")    ? "Sleep" : NULL;
    if (!request) {
        reply_error(fd, id, "bad_request", "action must be restart, shutdown or sleep");
        return;
    }

    /* Restart takes iOS a while to answer; shutdown may answer and drop mid-reply as the device
     * powers off. A longer read timeout turns neither into a hang. */
    int s = relay_open(sess, sess->diag_port, 15);
    if (s < 0) {
        reply_error(fd, id, "unavailable",
                    "cannot reach diagnostics_relay through the tunnel -- if stream_info is dead "
                    "too, this is the stale-session bug (#7), not the relay");
        return;
    }
    const char *err = relay_checkin(s);
    if (err) { close(s); reply_error(fd, id, "internal_error", err); return; }

    char reply[4096];
    if (relay_send_request(s, request, NULL, reply, sizeof reply) != 0) {
        close(s);
        reply_error(fd, id, "internal_error",
                    "no reply from diagnostics_relay; on shutdown the device may power off "
                    "mid-reply -- check whether the action took effect anyway");
        return;
    }
    close(s);

    /* The action reply's only field is Status; "Success" appears verbatim in XML and binary. */
    if (strstr(reply, "Success") && !strstr(reply, "Failure"))
        send_line(fd, "{\"id\":%ld,\"ok\":true,\"result\":{\"action\":\"%s\",\"status\":\"Success\"}}",
                  id, action);
    else {
        char msg[160];
        snprintf(msg, sizeof msg, "device answered %.100s", reply[0] ? reply : "with an empty reply");
        reply_error(fd, id, "device_error", msg);
    }
}

/* ------------------------------------------------------------------ JSON output buffer
 *
 * The app-list answers are open-ended (hundreds of apps, each a dictionary whose keys we do not
 * fix in advance), so they are built in a growable buffer and written whole, unlike the short
 * fixed replies send_line formats on the stack.
 */
typedef struct { char *p; size_t len, cap; int oom; } strbuf;

static void sb_put(strbuf *b, const char *s, size_t n)
{
    if (b->oom) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->len + n + 1) cap *= 2;
        char *np = realloc(b->p, cap);
        if (!np) { b->oom = 1; return; }
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void sb_puts(strbuf *b, const char *s) { sb_put(b, s, strlen(s)); }

static void sb_printf(strbuf *b, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) sb_put(b, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

static void sb_json_string(strbuf *b, const char *s)
{
    sb_put(b, "\"", 1);
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') { char e[2] = { '\\', (char)ch }; sb_put(b, e, 2); }
        else if (ch == '\n') sb_puts(b, "\\n");
        else if (ch == '\r') sb_puts(b, "\\r");
        else if (ch == '\t') sb_puts(b, "\\t");
        else if (ch < 0x20) sb_printf(b, "\\u%04x", ch);
        else sb_put(b, (const char *)&ch, 1);
    }
    sb_put(b, "\"", 1);
}

/* Render an XPC object as JSON, recursively. Types with no JSON shape (data, uuid, date) are
 * rendered as a short placeholder string rather than dropped, so a key that is present on the
 * device is present in our answer too and nobody has to wonder whether the reader skipped it. */
static void sb_xpc(strbuf *b, const rp_xpc_obj *o, int depth)
{
    if (depth > 12) { sb_puts(b, "null"); return; }
    switch (rp_xpc_obj_type(o)) {
    case RP_XPC_STRING: { const char *s = ""; rp_xpc_get_string(o, &s); sb_json_string(b, s); return; }
    case RP_XPC_BOOL:   { bool v = false; rp_xpc_get_bool(o, &v); sb_puts(b, v ? "true" : "false"); return; }
    case RP_XPC_INT64:  { uint64_t v = 0; rp_xpc_get_uint64(o, &v); sb_printf(b, "%lld", (long long)v); return; }
    case RP_XPC_UINT64: { uint64_t v = 0; rp_xpc_get_uint64(o, &v); sb_printf(b, "%llu", (unsigned long long)v); return; }
    case RP_XPC_DOUBLE: { double d; memcpy(&d, o->data + 4, 8); sb_printf(b, "%g", d); return; }
    case RP_XPC_DATE:   { uint64_t v = 0; rp_xpc_get_uint64(o, &v); sb_printf(b, "%lld", (long long)v); return; }
    case RP_XPC_DATA:   { const uint8_t *p; size_t n = 0; rp_xpc_get_data(o, &p, &n); sb_printf(b, "\"<%zu bytes>\"", n); return; }
    case RP_XPC_UUID:   sb_puts(b, "\"<uuid>\""); return;
    case RP_XPC_ARRAY: {
        sb_puts(b, "[");
        size_t cur = 0;
        rp_xpc_obj v;
        int first = 1;
        while (rp_xpc_array_next(o, &cur, &v) == 0) {
            if (!first) sb_puts(b, ",");
            first = 0;
            sb_xpc(b, &v, depth + 1);
        }
        sb_puts(b, "]");
        return;
    }
    case RP_XPC_DICT: {
        sb_puts(b, "{");
        size_t cur = 0;
        const char *k;
        rp_xpc_obj v;
        int first = 1;
        while (rp_xpc_dict_next(o, &cur, &k, &v) == 0) {
            if (!first) sb_puts(b, ",");
            first = 0;
            sb_json_string(b, k);
            sb_puts(b, ":");
            sb_xpc(b, &v, depth + 1);
        }
        sb_puts(b, "}");
        return;
    }
    default: sb_puts(b, "null"); return;
    }
}

static void send_all(int fd, const char *p, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t w = send(fd, p + off, n - off, 0);
        if (w <= 0) return;
        off += (size_t)w;
    }
}

/* ------------------------------------------------------------------ apps (coredevice.appservice)
 *
 * Same CoreDevice envelope as screenshots, on a different RSD port; the feature identifiers and
 * input shapes are what Xcode's devicectl sends (proven in host/appservice.py). Replies are
 * handed to the client as JSON shaped exactly like CoreDevice.output -- listapps answers an
 * array of app dictionaries (bundleIdentifier, name, version, isFirstParty, ...), listprocesses
 * an array of {processIdentifier, executable}, launch the new process's identifier.
 */
static int app_invoke(int fd, long id, const char *feature, const uint8_t *input, size_t n,
                      const char *what)
{
    const api_session *sess = g_session;
    if (!sess->app_port) {
        reply_error(fd, id, "unavailable", "the device did not offer coredevice.appservice");
        return -1;
    }
    svc_conn c;
    if (svc_open(&c, sess->tunnel_addr, sess->app_port) != 0) {
        reply_error(fd, id, "unavailable", "cannot reach coredevice.appservice");
        return -1;
    }
    char ua[37], ub[37];
    make_uuid(ua); make_uuid(ub);
    rp_xpc_obj out, reply;
    memset(&reply, 0, sizeof reply);
    int rc = rp_cd_invoke(&c.s, feature, NULL, input, n, ua, ub, &out, &reply);
    if (rc != 0) {
        /* CoreDevice.error carries a readable description; surface it instead of a stage name. */
        strbuf b = {0};
        sb_printf(&b, "{\"id\":%ld,\"ok\":false,\"error\":{\"code\":\"device_error\",\"message\":", id);
        rp_xpc_obj errobj;
        if (reply.data && rp_xpc_dict_get(&reply, "CoreDevice.error", &errobj) == 0) {
            strbuf e = {0};
            sb_printf(&e, "%s failed: ", what);
            sb_xpc(&e, &errobj, 0);
            sb_json_string(&b, e.p ? e.p : "");
            free(e.p);
        } else {
            char msg[128];
            snprintf(msg, sizeof msg, "%s failed: %s", what,
                     rc == RP_CD_ERR_NO_REPLY ? "the service never answered"
                                              : "the answer carried no CoreDevice.output");
            sb_json_string(&b, msg);
        }
        sb_puts(&b, "}}\n");
        if (b.p) send_all(fd, b.p, b.len);
        free(b.p);
        svc_close(&c);
        return -1;
    }
    strbuf b = {0};
    sb_printf(&b, "{\"id\":%ld,\"ok\":true,\"result\":", id);
    sb_xpc(&b, &out, 0);
    sb_puts(&b, "}\n");
    if (b.p && !b.oom) send_all(fd, b.p, b.len);
    else reply_error(fd, id, "internal_error", "out of memory rendering the reply");
    free(b.p);
    svc_close(&c);
    return 0;
}

/* Receive one framed plist into a heap buffer sized from its header. The Browse answers below
 * run to hundreds of kilobytes, well past relay_recv's stack-sized reply. */
static uint8_t *relay_recv_alloc(int fd, size_t *n_out)
{
    uint8_t len[4];
    if (recvn(fd, len, 4) != 0) return NULL;
    size_t body = ((size_t)len[0] << 24) | ((size_t)len[1] << 16) | ((size_t)len[2] << 8) | len[3];
    if (body == 0 || body > (16u << 20)) return NULL;
    uint8_t *buf = malloc(body);
    if (!buf) return NULL;
    if (recvn(fd, buf, body) != 0) { free(buf); return NULL; }
    *n_out = body;
    return buf;
}

static void sb_plstr(strbuf *b, plist_t v)
{
    if (!v || plist_get_node_type(v) != PLIST_STRING) { sb_puts(b, "\"\""); return; }
    char *s = NULL;
    plist_get_string_val(v, &s);
    sb_json_string(b, s ? s : "");
    free(s);
}

/* ------------------------------------------------------------------ app list (installation_proxy)
 *
 * coredevice.appservice's listapps validated our request and then never answered on iOS 26.5
 * (an all-false request answers [] instantly; anything that would enumerate goes silent for
 * minutes -- host/appservice.py reproduces it). installation_proxy's classic Browse answers the
 * same question in 0.2 s with 320 apps, so the list comes from there; launch and terminate stay
 * on appservice, which works. The reply is shaped like listapps would have been, so the app
 * does not care which service answered: bundleIdentifier, name, version, isFirstParty,
 * isDeveloper, isAppClip.
 *
 * isDeveloper is get-task-allow out of the app's Entitlements -- the flag Xcode sets true on a
 * development-signed build (to let a debugger attach) and false on an App Store or ad-hoc one.
 * Confirmed against Device Hub's own Apps > Developer filter: it shows only the get-task-allow
 * apps (three sideloaded test builds, all the same signer), not every third-party app the way
 * isFirstParty alone would -- that field is "not an Apple app", a much wider net than Device
 * Hub's actual "Developer" bucket.
 */
static void method_list_apps(int fd, long id, const char *line)
{
    (void)line;
    const api_session *sess = g_session;
    if (!sess->instproxy_port) {
        reply_error(fd, id, "unavailable", "the device did not offer installation_proxy");
        return;
    }
    int s = relay_open(sess, sess->instproxy_port, 30);
    if (s < 0) { reply_error(fd, id, "unavailable", "cannot reach installation_proxy through the tunnel"); return; }
    const char *err = relay_checkin(s);
    if (err) { close(s); reply_error(fd, id, "internal_error", err); return; }

    static const char browse[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n<dict>\n"
        "\t<key>Command</key>\n\t<string>Browse</string>\n"
        "\t<key>ClientOptions</key>\n\t<dict>\n"
        "\t\t<key>ApplicationType</key>\n\t\t<string>Any</string>\n"
        "\t\t<key>ReturnAttributes</key>\n\t\t<array>\n"
        "\t\t\t<string>CFBundleIdentifier</string>\n"
        "\t\t\t<string>CFBundleDisplayName</string>\n"
        "\t\t\t<string>CFBundleName</string>\n"
        "\t\t\t<string>CFBundleShortVersionString</string>\n"
        "\t\t\t<string>ApplicationType</string>\n"
        "\t\t\t<string>Entitlements</string>\n"
        "\t\t\t<string>IsAppClip</string>\n"
        "\t\t</array>\n\t</dict>\n</dict>\n</plist>\n";
    if (relay_send(s, browse, sizeof browse - 1) != 0) {
        close(s);
        reply_error(fd, id, "internal_error", "could not send Browse");
        return;
    }

    /* Browse streams: several {Status: BrowsingApplications, CurrentList: [...]} then Complete. */
    strbuf b = {0};
    sb_printf(&b, "{\"id\":%ld,\"ok\":true,\"result\":[", id);
    int first = 1, done = 0, failed = 0;
    while (!done && !failed) {
        size_t n = 0;
        uint8_t *raw = relay_recv_alloc(s, &n);
        if (!raw) { failed = 1; break; }
        plist_t pl = NULL;
        plist_from_memory((const char *)raw, (uint32_t)n, &pl, NULL);
        if (!pl || plist_get_node_type(pl) != PLIST_DICT) { failed = 1; free(raw); if (pl) plist_free(pl); break; }
        plist_t status = plist_dict_get_item(pl, "Status");
        if (plist_dict_get_item(pl, "Error")) failed = 1;
        if (status) { char *sv = NULL; plist_get_string_val(status, &sv); if (sv && !strcmp(sv, "Complete")) done = 1; free(sv); }
        plist_t list = plist_dict_get_item(pl, "CurrentList");
        if (list && plist_get_node_type(list) == PLIST_ARRAY) {
            for (uint32_t i = 0; i < plist_array_get_size(list); i++) {
                plist_t app = plist_array_get_item(list, i);
                if (plist_get_node_type(app) != PLIST_DICT) continue;
                plist_t bid = plist_dict_get_item(app, "CFBundleIdentifier");
                if (!bid) continue;
                plist_t name = plist_dict_get_item(app, "CFBundleDisplayName");
                if (!name) name = plist_dict_get_item(app, "CFBundleName");
                if (!name) name = bid;
                plist_t ver = plist_dict_get_item(app, "CFBundleShortVersionString");
                plist_t type = plist_dict_get_item(app, "ApplicationType");
                int user = 0;
                if (type) { char *tv = NULL; plist_get_string_val(type, &tv); if (tv && !strcmp(tv, "User")) user = 1; free(tv); }
                plist_t ent = plist_dict_get_item(app, "Entitlements");
                plist_t gta = (ent && plist_get_node_type(ent) == PLIST_DICT)
                            ? plist_dict_get_item(ent, "get-task-allow") : NULL;
                uint8_t developer = 0;
                if (gta && plist_get_node_type(gta) == PLIST_BOOLEAN) plist_get_bool_val(gta, &developer);
                plist_t clip = plist_dict_get_item(app, "IsAppClip");
                uint8_t appClip = 0;
                if (clip && plist_get_node_type(clip) == PLIST_BOOLEAN) plist_get_bool_val(clip, &appClip);
                sb_puts(&b, first ? "{" : ",{");
                first = 0;
                sb_puts(&b, "\"bundleIdentifier\":"); sb_plstr(&b, bid);
                sb_puts(&b, ",\"name\":");             sb_plstr(&b, name);
                sb_puts(&b, ",\"version\":");          sb_plstr(&b, ver);
                sb_printf(&b, ",\"isFirstParty\":%s", user ? "false" : "true");
                sb_printf(&b, ",\"isDeveloper\":%s", developer ? "true" : "false");
                sb_printf(&b, ",\"isAppClip\":%s}", appClip ? "true" : "false");
            }
        }
        plist_free(pl);
        free(raw);
    }
    close(s);
    if (failed || b.oom) {
        free(b.p);
        reply_error(fd, id, "device_error", "installation_proxy Browse did not complete");
        return;
    }
    sb_puts(&b, "]}\n");
    send_all(fd, b.p, b.len);
    free(b.p);
}

static void method_list_processes(int fd, long id)
{
    app_invoke(fd, id, "com.apple.coredevice.feature.listprocesses", NULL, 0, "listprocesses");
}

static void method_launch_app(int fd, long id, const char *line)
{
    char bid[256] = {0};
    if (json_string_field(line, "bundle_id", bid, sizeof bid) != 0 || !bid[0]) {
        reply_error(fd, id, "bad_request", "bundle_id is required");
        return;
    }
    /* platformSpecificOptions is a binary plist of an empty dictionary -- devicectl always sends
     * one and the service rejects the request without it. */
    static const uint8_t empty_bplist[42] = {
        0x62,0x70,0x6c,0x69,0x73,0x74,0x30,0x30,0xd0,0x08,0x00,0x00,0x00,0x00,0x00,0x00,
        0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x09 };
    uint8_t input[1024];
    rp_xpc_writer w;
    rp_xpc_writer_init(&w, input, sizeof input);
    rp_xpc_dict_begin(&w);
      rp_xpc_key(&w, "applicationSpecifier");
      rp_xpc_dict_begin(&w);
        rp_xpc_key(&w, "bundleIdentifier");
        rp_xpc_dict_begin(&w);
          rp_xpc_set_string(&w, "_0", bid);
        rp_xpc_dict_end(&w);
      rp_xpc_dict_end(&w);
      rp_xpc_key(&w, "options");
      rp_xpc_dict_begin(&w);
        rp_xpc_key(&w, "arguments");
        rp_xpc_array_begin(&w);
        rp_xpc_array_end(&w);
        rp_xpc_key(&w, "environmentVariables");
        rp_xpc_dict_begin(&w);
        rp_xpc_dict_end(&w);
        rp_xpc_set_bool(&w, "standardIOUsesPseudoterminals", true);
        rp_xpc_set_bool(&w, "startStopped", false);
        rp_xpc_set_bool(&w, "terminateExisting", true);
        rp_xpc_key(&w, "user");
        rp_xpc_dict_begin(&w);
          rp_xpc_set_bool(&w, "active", true);
        rp_xpc_dict_end(&w);
        rp_xpc_key(&w, "platformSpecificOptions");
        rp_xpc_data(&w, empty_bplist, sizeof empty_bplist);
      rp_xpc_dict_end(&w);
      rp_xpc_key(&w, "standardIOIdentifiers");
      rp_xpc_dict_begin(&w);
      rp_xpc_dict_end(&w);
    rp_xpc_dict_end(&w);
    if (w.overflow) { reply_error(fd, id, "internal_error", "launch request too large"); return; }
    app_invoke(fd, id, "com.apple.coredevice.feature.launchapplication", input, w.len, "launch");
}

static void method_terminate_app(int fd, long id, const char *line)
{
    long pid = json_number_field(line, "pid", 0);
    if (pid <= 0) { reply_error(fd, id, "bad_request", "pid is required"); return; }
    long sig = json_number_field(line, "signal", 9);
    uint8_t input[128];
    rp_xpc_writer w;
    rp_xpc_writer_init(&w, input, sizeof input);
    rp_xpc_dict_begin(&w);
      rp_xpc_key(&w, "process");
      rp_xpc_dict_begin(&w);
        rp_xpc_set_int64(&w, "processIdentifier", pid);
      rp_xpc_dict_end(&w);
      rp_xpc_set_int64(&w, "signal", sig);
    rp_xpc_dict_end(&w);
    app_invoke(fd, id, "com.apple.coredevice.feature.sendsignaltoprocess", input, w.len, "terminate");
}

/* ------------------------------------------------------------------ profiles (misagent, MCInstall)
 *
 * Device Hub's third inspector tab. Provisioning profiles come from misagent as CMS blobs with
 * the XML plist embedded verbatim, so the plist is cut out between "<?xml" and "</plist>" and
 * parsed -- what host/deviceinfo-era prototypes did in Python. Configuration profiles come from
 * MCInstall's GetProfileList as metadata keyed by identifier. Both proven live 2026-08-23.
 */
static void sb_pl_field(strbuf *b, const char *jkey, plist_t d, const char *key, int first)
{
    plist_t v = plist_dict_get_item(d, key);
    sb_puts(b, first ? "" : ",");
    sb_json_string(b, jkey);
    sb_puts(b, ":");
    if (!v) { sb_puts(b, "\"\""); return; }
    switch (plist_get_node_type(v)) {
    case PLIST_STRING: sb_plstr(b, v); return;
    case PLIST_DATE: {
        /* plist dates are seconds since 2001-01-01 UTC; +978307200 to Unix. ISO-8601 for the app. */
        int64_t sec = 0; plist_get_unix_date_val(v, &sec);
        time_t u = (time_t)sec;
        char tmp[32]; strftime(tmp, sizeof tmp, "%Y-%m-%dT%H:%M:%SZ", gmtime(&u));
        sb_json_string(b, tmp);
        return;
    }
    case PLIST_BOOLEAN: { uint8_t bv = 0; plist_get_bool_val(v, &bv); sb_puts(b, bv ? "true" : "false"); return; }
    case PLIST_UINT: { uint64_t nn = 0; plist_get_uint_val(v, &nn); sb_printf(b, "%llu", (unsigned long long)nn); return; }
    case PLIST_REAL: { double dd = 0; plist_get_real_val(v, &dd); sb_printf(b, "%g", dd); return; }
    default: sb_puts(b, "\"\""); return;
    }
}

static void method_list_profiles(int fd, long id)
{
    const api_session *sess = g_session;
    strbuf b = {0};
    sb_printf(&b, "{\"id\":%ld,\"ok\":true,\"result\":{\"provisioning\":[", id);
    int first = 1;
    const char *err = NULL;

    if (sess->misagent_port) {
        int s = relay_open(sess, sess->misagent_port, 15);
        if (s < 0 || (err = relay_checkin(s)) != NULL) { if (s >= 0) close(s); if (!err) err = "cannot reach misagent"; }
        else {
            static const char req[] =
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n<dict>\n"
                "\t<key>MessageType</key>\n\t<string>CopyAll</string>\n"
                "\t<key>ProfileType</key>\n\t<string>Provisioning</string>\n</dict>\n</plist>\n";
            size_t n = 0;
            uint8_t *raw = relay_send(s, req, sizeof req - 1) == 0 ? relay_recv_alloc(s, &n) : NULL;
            close(s);
            plist_t pl = NULL;
            if (raw) plist_from_memory((const char *)raw, (uint32_t)n, &pl, NULL);
            plist_t payload = (pl && plist_get_node_type(pl) == PLIST_DICT) ? plist_dict_get_item(pl, "Payload") : NULL;
            for (uint32_t i = 0; payload && plist_get_node_type(payload) == PLIST_ARRAY
                                && i < plist_array_get_size(payload); i++) {
                plist_t blob = plist_array_get_item(payload, i);
                if (plist_get_node_type(blob) != PLIST_DATA) continue;
                uint64_t len = 0;
                const char *bytes = plist_get_data_ptr(blob, &len);
                /* memmem-free scan: the CMS wrapper is binary, the plist inside is text. */
                const char *start = NULL, *stop = NULL;
                for (size_t k = 0; bytes && k + 8 < len; k++) {
                    if (!start && !memcmp(bytes + k, "<?xml", 5)) start = bytes + k;
                    if (start && !memcmp(bytes + k, "</plist>", 8)) { stop = bytes + k + 8; break; }
                }
                if (!start || !stop) continue;
                plist_t prof = NULL;
                plist_from_memory(start, (uint32_t)(stop - start), &prof, NULL);
                if (!prof) continue;
                if (plist_get_node_type(prof) == PLIST_DICT) {
                    sb_puts(&b, first ? "{" : ",{");
                    first = 0;
                    sb_pl_field(&b, "name", prof, "Name", 1);
                    sb_pl_field(&b, "app_id_name", prof, "AppIDName", 0);
                    sb_pl_field(&b, "team", prof, "TeamName", 0);
                    sb_pl_field(&b, "uuid", prof, "UUID", 0);
                    sb_pl_field(&b, "expires", prof, "ExpirationDate", 0);
                    sb_pl_field(&b, "created", prof, "CreationDate", 0);
                    plist_t devs = plist_dict_get_item(prof, "ProvisionedDevices");
                    sb_printf(&b, ",\"devices\":%u}", devs && plist_get_node_type(devs) == PLIST_ARRAY ? plist_array_get_size(devs) : 0u);
                }
                plist_free(prof);
            }
            if (pl) plist_free(pl);
            free(raw);
        }
    }
    sb_puts(&b, "],\"configuration\":[");
    first = 1;
    if (sess->mcinstall_port) {
        int s = relay_open(sess, sess->mcinstall_port, 15);
        const char *e2 = NULL;
        if (s < 0 || (e2 = relay_checkin(s)) != NULL) { if (s >= 0) close(s); if (!err) err = e2 ? e2 : "cannot reach MCInstall"; }
        else {
            static const char req[] =
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n<dict>\n"
                "\t<key>RequestType</key>\n\t<string>GetProfileList</string>\n</dict>\n</plist>\n";
            size_t n = 0;
            uint8_t *raw = relay_send(s, req, sizeof req - 1) == 0 ? relay_recv_alloc(s, &n) : NULL;
            close(s);
            plist_t pl = NULL;
            if (raw) plist_from_memory((const char *)raw, (uint32_t)n, &pl, NULL);
            if (pl && plist_get_node_type(pl) == PLIST_DICT) {
                plist_t ids = plist_dict_get_item(pl, "OrderedIdentifiers");
                plist_t meta = plist_dict_get_item(pl, "ProfileMetadata");
                for (uint32_t i = 0; ids && plist_get_node_type(ids) == PLIST_ARRAY && i < plist_array_get_size(ids); i++) {
                    plist_t ident = plist_array_get_item(ids, i);
                    char *ident_s = NULL;
                    plist_get_string_val(ident, &ident_s);
                    plist_t m = (meta && plist_get_node_type(meta) == PLIST_DICT && ident_s) ? plist_dict_get_item(meta, ident_s) : NULL;
                    sb_puts(&b, first ? "{" : ",{");
                    first = 0;
                    sb_puts(&b, "\"identifier\":"); sb_plstr(&b, ident);
                    if (m && plist_get_node_type(m) == PLIST_DICT) {
                        sb_pl_field(&b, "name", m, "PayloadDisplayName", 0);
                        sb_pl_field(&b, "organization", m, "PayloadOrganization", 0);
                        sb_pl_field(&b, "description", m, "PayloadDescription", 0);
                        sb_pl_field(&b, "uuid", m, "PayloadUUID", 0);
                    }
                    sb_puts(&b, "}");
                    free(ident_s);
                }
            }
            if (pl) plist_free(pl);
            free(raw);
        }
    }
    sb_puts(&b, "]");
    if (err) { sb_puts(&b, ",\"error\":"); sb_json_string(&b, err); }
    sb_puts(&b, "}}\n");
    if (b.p && !b.oom) send_all(fd, b.p, b.len);
    else reply_error(fd, id, "internal_error", "out of memory rendering profiles");
    free(b.p);
}

/* ------------------------------------------------------------------ install/remove profile (misagent, MCInstall)
 *
 * Device Hub's Profiles-tab `+`/`-`, the counterpart to install/uninstall above. A provisioning
 * profile (.mobileprovision) goes to misagent as its raw CMS-signed bytes; a configuration profile
 * (.mobileconfig) goes to MCInstall as its raw plist bytes -- neither needs us to parse or re-sign
 * the payload, only forward it. Which service gets it is decided by file extension, since that is
 * exactly what distinguishes the two kinds on disk. Removal takes the id the corresponding half of
 * list_profiles already returns: the provisioning profile's `uuid`, or the configuration profile's
 * `identifier`.
 */
static uint8_t *read_local_file(const char *path, size_t *out_n, const char **err)
{
    FILE *f = fopen(path, "rb");
    if (!f) { *err = "could not open the local file"; return NULL; }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize <= 0 || fsize > (64 << 20)) { fclose(f); *err = "file is empty or over 64 MB"; return NULL; }
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)fsize);
    if (!data || fread(data, 1, (size_t)fsize, f) != (size_t)fsize) {
        free(data); fclose(f); *err = "could not read the local file"; return NULL;
    }
    fclose(f);
    *out_n = (size_t)fsize;
    return data;
}

/* misagent/MCInstall are request/reply, not streaming like installation_proxy -- one plist out,
 * one back. Success is a Status of 0 (misagent's numeric convention) or "Acknowledged"/"Success"
 * (MCInstall's); anything else, including no Status at all, is surfaced as an error. */
static void relay_plist_run(int fd, long id, long port, const uint8_t *xml, size_t xml_len, const char *what)
{
    const api_session *sess = g_session;
    if (!port) { reply_error(fd, id, "unavailable", "the device did not offer that service"); return; }
    int s = relay_open(sess, port, 15);
    if (s < 0) { reply_error(fd, id, "unavailable", "cannot reach the service through the tunnel"); return; }
    const char *err = relay_checkin(s);
    if (err) { close(s); reply_error(fd, id, "internal_error", err); return; }
    if (relay_send(s, (const char *)xml, xml_len) != 0) {
        close(s);
        reply_error(fd, id, "internal_error", "could not send request");
        return;
    }
    size_t n = 0;
    uint8_t *raw = relay_recv_alloc(s, &n);
    close(s);
    if (!raw) { reply_error(fd, id, "device_error", "no reply from the device"); return; }
    plist_t pl = NULL;
    plist_from_memory((const char *)raw, (uint32_t)n, &pl, NULL);
    free(raw);
    if (!pl || plist_get_node_type(pl) != PLIST_DICT) {
        if (pl) plist_free(pl);
        reply_error(fd, id, "device_error", "malformed reply");
        return;
    }

    plist_t status = plist_dict_get_item(pl, "Status");
    int ok = 0;
    char status_str[64] = "";
    if (status) {
        plist_type t = plist_get_node_type(status);
        if (t == PLIST_STRING) {
            char *sv = NULL; plist_get_string_val(status, &sv);
            if (sv) { snprintf(status_str, sizeof status_str, "%s", sv); ok = !strcmp(sv, "Acknowledged") || !strcmp(sv, "Success"); }
            free(sv);
        } else if (t == PLIST_UINT || t == PLIST_BOOLEAN) {
            uint64_t sv = 0; plist_get_uint_val(status, &sv);
            snprintf(status_str, sizeof status_str, "%llu", (unsigned long long)sv);
            ok = sv == 0;
        }
    }
    strbuf b = {0};
    if (ok) {
        sb_printf(&b, "{\"id\":%ld,\"ok\":true,\"result\":{\"status\":", id);
        sb_json_string(&b, status_str[0] ? status_str : "ok");
        sb_puts(&b, "}}\n");
    } else {
        char msg[128];
        snprintf(msg, sizeof msg, "%s failed%s%s", what, status_str[0] ? ": " : "", status_str);
        sb_printf(&b, "{\"id\":%ld,\"ok\":false,\"error\":{\"code\":\"device_error\",\"message\":", id);
        sb_json_string(&b, msg);
        sb_puts(&b, "}}\n");
    }
    if (b.p && !b.oom) send_all(fd, b.p, b.len);
    free(b.p);
    plist_free(pl);
}

static void method_install_profile(int fd, long id, const char *line)
{
    const api_session *sess = g_session;
    char path[1024] = "";
    json_string_field(line, "path", path, sizeof path);
    if (!path[0]) { reply_error(fd, id, "bad_request", "path is required (a local .mobileprovision or .mobileconfig file)"); return; }

    const char *ext = strrchr(path, '.');
    int provisioning = ext && !strcasecmp(ext, ".mobileprovision");
    int configuration = ext && !strcasecmp(ext, ".mobileconfig");
    if (!provisioning && !configuration) {
        reply_error(fd, id, "bad_request", "path must end in .mobileprovision or .mobileconfig");
        return;
    }

    size_t n = 0; const char *err = NULL;
    uint8_t *data = read_local_file(path, &n, &err);
    if (!data) { reply_error(fd, id, "bad_request", err); return; }

    size_t b64cap = ((n + 2) / 3) * 4 + 1;
    char *b64 = malloc(b64cap);
    size_t bn = b64 ? b64_encode(data, n, b64, b64cap) : 0;
    free(data);
    if (!bn && n) { free(b64); reply_error(fd, id, "internal_error", "could not encode the profile"); return; }

    strbuf b = {0};
    sb_puts(&b, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n<dict>\n");
    if (provisioning) sb_puts(&b, "\t<key>MessageType</key>\n\t<string>Install</string>\n\t<key>Profile</key>\n\t<data>");
    else              sb_puts(&b, "\t<key>RequestType</key>\n\t<string>InstallProfile</string>\n\t<key>Payload</key>\n\t<data>");
    if (bn) sb_put(&b, b64, bn);
    free(b64);
    sb_puts(&b, "</data>\n</dict>\n</plist>\n");
    if (b.oom || !b.p) { free(b.p); reply_error(fd, id, "internal_error", "out of memory building the request"); return; }

    relay_plist_run(fd, id, provisioning ? sess->misagent_port : sess->mcinstall_port,
                     (const uint8_t *)b.p, b.len, "install");
    free(b.p);
}

static void method_remove_profile(int fd, long id, const char *line)
{
    const api_session *sess = g_session;
    char type[16] = "";
    json_string_field(line, "type", type, sizeof type);
    int provisioning = !strcmp(type, "provisioning");
    int configuration = !strcmp(type, "configuration");
    if (!provisioning && !configuration) {
        reply_error(fd, id, "bad_request", "type must be provisioning or configuration");
        return;
    }

    strbuf b = {0};
    if (provisioning) {
        char uuid[64] = "", uuid_esc[128];
        if (json_string_field(line, "uuid", uuid, sizeof uuid) != 0 || !uuid[0]) {
            reply_error(fd, id, "bad_request", "uuid is required for a provisioning profile");
            return;
        }
        xml_escape(uuid, uuid_esc, sizeof uuid_esc);
        sb_printf(&b,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n<dict>\n"
            "\t<key>MessageType</key>\n\t<string>Remove</string>\n"
            "\t<key>ProfileID</key>\n\t<string>%s</string>\n</dict>\n</plist>\n", uuid_esc);
    } else {
        char ident[256] = "", ident_esc[300];
        if (json_string_field(line, "identifier", ident, sizeof ident) != 0 || !ident[0]) {
            reply_error(fd, id, "bad_request", "identifier is required for a configuration profile");
            return;
        }
        xml_escape(ident, ident_esc, sizeof ident_esc);
        sb_printf(&b,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n<dict>\n"
            "\t<key>RequestType</key>\n\t<string>RemoveProfile</string>\n"
            "\t<key>ProfileIdentifier</key>\n\t<string>%s</string>\n</dict>\n</plist>\n", ident_esc);
    }
    if (b.oom || !b.p) { free(b.p); reply_error(fd, id, "internal_error", "out of memory building the request"); return; }

    relay_plist_run(fd, id, provisioning ? sess->misagent_port : sess->mcinstall_port,
                     (const uint8_t *)b.p, b.len, "remove");
    free(b.p);
}

/* ------------------------------------------------------------------ files (AFC)
 *
 * Apple File Conduit, spoken by afc.shim.remote (the Media partition) and by
 * crashreportcopymobile.shim.remote (rooted at the crash-report directory). Binary packets after
 * the RSDCheckin: "CFA6LPAA", entire_len, this_len, packet_num, operation (all u64 LE), payload.
 * Proven by host/afc.py on both services (2026-08-23); reference libimobiledevice afc.c.
 */
#define AFC_OP_STATUS        0x01
#define AFC_OP_READ_DIR      0x03
#define AFC_OP_DATA          0x02
#define AFC_OP_MAKE_DIR      0x09
#define AFC_OP_GET_FILE_INFO 0x0A
#define AFC_OP_FILE_OPEN     0x0D
#define AFC_OP_FILE_OPEN_RES 0x0E
#define AFC_OP_FILE_READ     0x0F
#define AFC_OP_FILE_WRITE    0x10
#define AFC_OP_FILE_CLOSE    0x14
#define AFC_FOPEN_WRONLY     3  /* truncate + create -- what a fresh app upload needs */

typedef struct { int fd; uint64_t seq; } afc_conn;

static void put_u64le(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint64_t get_u64le(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }

static int afc_open(afc_conn *c, const api_session *sess, long port, const char **err)
{
    c->seq = 0;
    c->fd = relay_open(sess, port, 30);
    if (c->fd < 0) { *err = "cannot reach the AFC service through the tunnel"; return -1; }
    const char *e = relay_checkin(c->fd);
    if (e) { close(c->fd); c->fd = -1; *err = e; return -1; }
    return 0;
}

static int afc_send(afc_conn *c, uint64_t op, const uint8_t *payload, size_t n)
{
    uint8_t hdr[40];
    memcpy(hdr, "CFA6LPAA", 8);
    put_u64le(hdr + 8, 40 + n);
    put_u64le(hdr + 16, 40 + n);
    put_u64le(hdr + 24, c->seq++);
    put_u64le(hdr + 32, op);
    if (send(c->fd, hdr, 40, 0) != 40) return -1;
    if (n && send(c->fd, payload, n, 0) != (ssize_t)n) return -1;
    return 0;
}

/* Receive one packet; *body is malloc'd (may be NULL when empty). Returns the op, or -1. On a
 * STATUS packet the AFC error code is returned through *status (0 = success). */
static long afc_recv(afc_conn *c, uint8_t **body, size_t *n, uint64_t *status)
{
    uint8_t hdr[40];
    if (recvn(c->fd, hdr, 40) != 0 || memcmp(hdr, "CFA6LPAA", 8) != 0) return -1;
    uint64_t entire = get_u64le(hdr + 8), op = get_u64le(hdr + 32);
    if (entire < 40 || entire - 40 > (64u << 20)) return -1;
    *n = (size_t)(entire - 40);
    *body = *n ? malloc(*n) : NULL;
    if (*n && (!*body || recvn(c->fd, *body, *n) != 0)) { free(*body); *body = NULL; return -1; }
    *status = 0;
    if (op == AFC_OP_STATUS && *n >= 8) *status = get_u64le(*body);
    return (long)op;
}

/* NUL-separated strings in `body` -> JSON; `pairs` renders key/value alternation as an object. */
static int afc_readdir(afc_conn *c, const char *path, strbuf *b, int with_stat);

static int afc_stat(afc_conn *c, const char *path, long long *size, long long *mtime_ns, char *kind, size_t kind_cap)
{
    size_t plen = strlen(path) + 1;
    if (afc_send(c, AFC_OP_GET_FILE_INFO, (const uint8_t *)path, plen) != 0) return -1;
    uint8_t *body = NULL; size_t n = 0; uint64_t st = 0;
    long op = afc_recv(c, &body, &n, &st);
    if (op != AFC_OP_DATA) { free(body); return -1; }
    *size = 0; *mtime_ns = 0; kind[0] = 0;
    size_t i = 0;
    while (i < n) {
        const char *k = (const char *)body + i; size_t kl = strnlen(k, n - i); i += kl + 1;
        if (i >= n) break;
        const char *v = (const char *)body + i; size_t vl = strnlen(v, n - i); i += vl + 1;
        if (!strcmp(k, "st_size")) *size = atoll(v);
        else if (!strcmp(k, "st_mtime")) *mtime_ns = atoll(v);
        else if (!strcmp(k, "st_ifmt")) snprintf(kind, kind_cap, "%s", v);
    }
    free(body);
    return 0;
}

static int afc_readdir(afc_conn *c, const char *path, strbuf *b, int with_stat)
{
    size_t plen = strlen(path) + 1;
    if (afc_send(c, AFC_OP_READ_DIR, (const uint8_t *)path, plen) != 0) return -1;
    uint8_t *body = NULL; size_t n = 0; uint64_t st = 0;
    long op = afc_recv(c, &body, &n, &st);
    if (op != AFC_OP_DATA) { free(body); return st ? (int)st : -1; }
    sb_puts(b, "[");
    int first = 1;
    size_t i = 0;
    while (i < n) {
        const char *name = (const char *)body + i; size_t nl = strnlen(name, n - i); i += nl + 1;
        if (!nl || !strcmp(name, ".") || !strcmp(name, "..")) continue;
        sb_puts(b, first ? "{" : ",{");
        first = 0;
        sb_puts(b, "\"name\":"); sb_json_string(b, name);
        if (with_stat) {
            char full[1024];
            snprintf(full, sizeof full, "%s%s%s", path, path[strlen(path) - 1] == '/' ? "" : "/", name);
            long long size = 0, mtime = 0; char kind[32] = "";
            if (afc_stat(c, full, &size, &mtime, kind, sizeof kind) == 0)
                sb_printf(b, ",\"size\":%lld,\"mtime\":%lld,\"is_dir\":%s", size, mtime / 1000000000LL,
                          !strcmp(kind, "S_IFDIR") ? "true" : "false");
        }
        sb_puts(b, "}");
    }
    sb_puts(b, "]");
    free(body);
    return 0;
}

/* Read a whole file into a malloc'd buffer. Capped at 64 MB: this serves crash reports and the
 * occasional media file, not a backup. */
static uint8_t *afc_read_file(afc_conn *c, const char *path, size_t *out_n)
{
    long long size = 0, mtime = 0; char kind[32];
    if (afc_stat(c, path, &size, &mtime, kind, sizeof kind) != 0 || size < 0 || size > (64 << 20)) return NULL;
    size_t plen = strlen(path) + 1;
    uint8_t *req = malloc(8 + plen);
    put_u64le(req, 1);                         /* AFC_FOPEN_RDONLY */
    memcpy(req + 8, path, plen);
    int rc = afc_send(c, AFC_OP_FILE_OPEN, req, 8 + plen);
    free(req);
    if (rc != 0) return NULL;
    uint8_t *body = NULL; size_t n = 0; uint64_t st = 0;
    if (afc_recv(c, &body, &n, &st) != AFC_OP_FILE_OPEN_RES || n < 8) { free(body); return NULL; }
    uint64_t handle = get_u64le(body);
    free(body);

    uint8_t *out = malloc((size_t)size + 1);
    size_t got = 0;
    int ok = out != NULL;
    while (ok && got < (size_t)size) {
        uint8_t rr[16];
        put_u64le(rr, handle);
        put_u64le(rr + 8, (uint64_t)((size_t)size - got < (1u << 20) ? (size_t)size - got : (1u << 20)));
        if (afc_send(c, AFC_OP_FILE_READ, rr, 16) != 0) { ok = 0; break; }
        long op = afc_recv(c, &body, &n, &st);
        if (op != AFC_OP_DATA || !n) { free(body); break; }
        memcpy(out + got, body, n);
        got += n;
        free(body);
    }
    uint8_t cl[8];
    put_u64le(cl, handle);
    if (afc_send(c, AFC_OP_FILE_CLOSE, cl, 8) == 0) { afc_recv(c, &body, &n, &st); free(body); }
    if (!ok) { free(out); return NULL; }
    *out_n = got;
    return out;
}

/* Creates a directory; ok if it already exists (status 8 -- reused for the PublicStaging install
 * dir, which after the first install always exists already). */
static int afc_make_dir(afc_conn *c, const char *path)
{
    size_t plen = strlen(path) + 1;
    if (afc_send(c, AFC_OP_MAKE_DIR, (const uint8_t *)path, plen) != 0) return -1;
    uint8_t *body = NULL; size_t n = 0; uint64_t st = 0;
    long op = afc_recv(c, &body, &n, &st);
    free(body);
    return op == AFC_OP_STATUS ? (int)st : -1;
}

/* Writes a whole file in <=1 MB chunks -- afc_read_file's read side uses the same cap. Used to
 * stage an .ipa into /PublicStaging before installation_proxy's Install command. */
static int afc_write_file(afc_conn *c, const char *path, const uint8_t *data, size_t n)
{
    size_t plen = strlen(path) + 1;
    uint8_t *req = malloc(8 + plen);
    if (!req) return -1;
    put_u64le(req, AFC_FOPEN_WRONLY);
    memcpy(req + 8, path, plen);
    int rc = afc_send(c, AFC_OP_FILE_OPEN, req, 8 + plen);
    free(req);
    if (rc != 0) return -1;
    uint8_t *body = NULL; size_t bn = 0; uint64_t st = 0;
    if (afc_recv(c, &body, &bn, &st) != AFC_OP_FILE_OPEN_RES || bn < 8) { free(body); return -1; }
    uint64_t handle = get_u64le(body);
    free(body);

    int ok = 1;
    size_t off = 0;
    while (ok && off < n) {
        size_t chunk = n - off < (1u << 20) ? n - off : (1u << 20);
        uint8_t *pkt = malloc(8 + chunk);
        if (!pkt) { ok = 0; break; }
        put_u64le(pkt, handle);
        memcpy(pkt + 8, data + off, chunk);
        rc = afc_send(c, AFC_OP_FILE_WRITE, pkt, 8 + chunk);
        free(pkt);
        if (rc != 0) { ok = 0; break; }
        if (afc_recv(c, &body, &bn, &st) != AFC_OP_STATUS || st != 0) { free(body); ok = 0; break; }
        free(body);
        off += chunk;
    }
    uint8_t cl[8];
    put_u64le(cl, handle);
    if (afc_send(c, AFC_OP_FILE_CLOSE, cl, 8) == 0) { afc_recv(c, &body, &bn, &st); free(body); }
    return ok ? 0 : -1;
}

/* ------------------------------------------------------------------ install/uninstall (installation_proxy, AFC)
 *
 * Device Hub's Apps-tab `+`/`-`. Install stages the package into /PublicStaging over AFC (the same
 * Media-rooted afc_port list_dir/read_file use -- PublicStaging lives at its root) and then sends
 * installation_proxy's classic Install command; uninstall needs no staging, just the bundle id.
 * Both stream {Status: ...} events exactly like Browse (method_list_apps above); the run ends on a
 * Status of Complete or an Error dict.
 */
static int instproxy_run(int fd, long id, const char *xml, size_t xml_len, const char *what)
{
    const api_session *sess = g_session;
    if (!sess->instproxy_port) {
        reply_error(fd, id, "unavailable", "the device did not offer installation_proxy");
        return -1;
    }
    int s = relay_open(sess, sess->instproxy_port, 30);
    if (s < 0) { reply_error(fd, id, "unavailable", "cannot reach installation_proxy through the tunnel"); return -1; }
    const char *err = relay_checkin(s);
    if (err) { close(s); reply_error(fd, id, "internal_error", err); return -1; }
    if (relay_send(s, xml, xml_len) != 0) {
        close(s);
        reply_error(fd, id, "internal_error", "could not send request");
        return -1;
    }

    int done = 0, failed = 0;
    char last_status[128] = {0}, err_desc[256] = {0};
    while (!done && !failed) {
        size_t n = 0;
        uint8_t *raw = relay_recv_alloc(s, &n);
        if (!raw) { failed = 1; break; }
        plist_t pl = NULL;
        plist_from_memory((const char *)raw, (uint32_t)n, &pl, NULL);
        free(raw);
        if (!pl || plist_get_node_type(pl) != PLIST_DICT) { failed = 1; if (pl) plist_free(pl); break; }
        plist_t error = plist_dict_get_item(pl, "Error");
        if (error) {
            failed = 1;
            char *ev = NULL; plist_get_string_val(error, &ev);
            plist_t desc = plist_dict_get_item(pl, "ErrorDescription");
            char *dv = NULL; if (desc) plist_get_string_val(desc, &dv);
            snprintf(err_desc, sizeof err_desc, "%s failed: %s", what, dv ? dv : (ev ? ev : "unknown error"));
            free(ev); free(dv);
        }
        plist_t status = plist_dict_get_item(pl, "Status");
        if (status) {
            char *sv = NULL; plist_get_string_val(status, &sv);
            if (sv) { snprintf(last_status, sizeof last_status, "%s", sv); if (!strcmp(sv, "Complete")) done = 1; }
            free(sv);
        }
        plist_free(pl);
    }
    close(s);
    strbuf b = {0};
    if (failed) {
        sb_printf(&b, "{\"id\":%ld,\"ok\":false,\"error\":{\"code\":\"device_error\",\"message\":", id);
        sb_json_string(&b, err_desc[0] ? err_desc : "installation_proxy did not complete");
        sb_puts(&b, "}}\n");
    } else {
        sb_printf(&b, "{\"id\":%ld,\"ok\":true,\"result\":{\"status\":", id);
        sb_json_string(&b, last_status[0] ? last_status : "Complete");
        sb_puts(&b, "}}\n");
    }
    if (b.p && !b.oom) send_all(fd, b.p, b.len);
    free(b.p);
    return failed ? -1 : 0;
}

static void method_install_app(int fd, long id, const char *line)
{
    const api_session *sess = g_session;
    char path[1024] = "";
    json_string_field(line, "path", path, sizeof path);
    if (!path[0]) { reply_error(fd, id, "bad_request", "path is required (a local .ipa file)"); return; }
    if (!sess->afc_port) { reply_error(fd, id, "unavailable", "the device did not offer afc"); return; }

    FILE *f = fopen(path, "rb");
    if (!f) { reply_error(fd, id, "bad_request", "could not open the local file"); return; }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize <= 0 || fsize > (512 << 20)) { fclose(f); reply_error(fd, id, "bad_request", "file is empty or over 512 MB"); return; }
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)fsize);
    if (!data || fread(data, 1, (size_t)fsize, f) != (size_t)fsize) {
        free(data); fclose(f);
        reply_error(fd, id, "internal_error", "could not read the local file");
        return;
    }
    fclose(f);

    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char staged[1200];
    snprintf(staged, sizeof staged, "PublicStaging/%s", base);

    afc_conn c; const char *err;
    if (afc_open(&c, sess, sess->afc_port, &err) != 0) { free(data); reply_error(fd, id, "unavailable", err); return; }
    afc_make_dir(&c, "PublicStaging");  /* ignore -- exists after the first install */
    int wrc = afc_write_file(&c, staged, data, (size_t)fsize);
    close(c.fd);
    free(data);
    if (wrc != 0) { reply_error(fd, id, "device_error", "could not stage the app over AFC"); return; }

    char staged_esc[1300];
    xml_escape(staged, staged_esc, sizeof staged_esc);
    char xml[1400];
    int n = snprintf(xml, sizeof xml,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n<dict>\n"
        "\t<key>Command</key>\n\t<string>Install</string>\n"
        "\t<key>PackagePath</key>\n\t<string>%s</string>\n</dict>\n</plist>\n", staged_esc);
    if (n <= 0 || (size_t)n >= sizeof xml) { reply_error(fd, id, "internal_error", "package path too long"); return; }
    instproxy_run(fd, id, xml, (size_t)n, "install");
}

static void method_uninstall_app(int fd, long id, const char *line)
{
    char bid[256] = {0};
    if (json_string_field(line, "bundle_id", bid, sizeof bid) != 0 || !bid[0]) {
        reply_error(fd, id, "bad_request", "bundle_id is required");
        return;
    }
    char bid_esc[300];
    xml_escape(bid, bid_esc, sizeof bid_esc);
    char xml[512];
    int n = snprintf(xml, sizeof xml,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n<dict>\n"
        "\t<key>Command</key>\n\t<string>Uninstall</string>\n"
        "\t<key>ApplicationIdentifier</key>\n\t<string>%s</string>\n</dict>\n</plist>\n", bid_esc);
    if (n <= 0 || (size_t)n >= sizeof xml) { reply_error(fd, id, "bad_request", "bundle_id too long"); return; }
    instproxy_run(fd, id, xml, (size_t)n, "uninstall");
}

static long afc_port_for(const char *service, const api_session *sess)
{
    if (!strcmp(service, "crash")) return sess->crashcopy_port;
    return sess->afc_port;
}

/* Fresh reports sit elsewhere until crashreportmover moves them; connecting to it does the move
 * and it answers "ping". Cheap, so done before every listing. */
static void crash_mover_poke(const api_session *sess)
{
    if (!sess->crashmover_port) return;
    int s = relay_open(sess, sess->crashmover_port, 15);
    if (s < 0) return;
    if (relay_checkin(s) == NULL) { char ping[4]; recvn(s, (uint8_t *)ping, 4); }
    close(s);
}

static void method_list_dir(int fd, long id, const char *line)
{
    const api_session *sess = g_session;
    char service[16] = "media", path[1024] = "/";
    json_string_field(line, "service", service, sizeof service);
    json_string_field(line, "path", path, sizeof path);
    if (!path[0]) snprintf(path, sizeof path, "/");
    long port = afc_port_for(service, sess);
    if (!port) { reply_error(fd, id, "unavailable", "the device did not offer that AFC service"); return; }
    if (!strcmp(service, "crash")) crash_mover_poke(sess);
    afc_conn c; const char *err;
    if (afc_open(&c, sess, port, &err) != 0) { reply_error(fd, id, "unavailable", err); return; }
    strbuf b = {0};
    sb_printf(&b, "{\"id\":%ld,\"ok\":true,\"result\":{\"path\":", id);
    sb_json_string(&b, path);
    sb_puts(&b, ",\"entries\":");
    int rc = afc_readdir(&c, path, &b, 1);
    close(c.fd);
    if (rc != 0) {
        free(b.p);
        char msg[96];
        snprintf(msg, sizeof msg, rc > 0 ? "AFC error %d (8 = no such path)" : "AFC read failed", rc);
        reply_error(fd, id, "device_error", msg);
        return;
    }
    sb_puts(&b, "}}\n");
    if (b.p && !b.oom) send_all(fd, b.p, b.len);
    free(b.p);
}

static void method_read_file(int fd, long id, const char *line)
{
    const api_session *sess = g_session;
    char service[16] = "media", path[1024] = "";
    json_string_field(line, "service", service, sizeof service);
    json_string_field(line, "path", path, sizeof path);
    if (!path[0]) { reply_error(fd, id, "bad_request", "path is required"); return; }
    long port = afc_port_for(service, sess);
    if (!port) { reply_error(fd, id, "unavailable", "the device did not offer that AFC service"); return; }
    afc_conn c; const char *err;
    if (afc_open(&c, sess, port, &err) != 0) { reply_error(fd, id, "unavailable", err); return; }
    size_t n = 0;
    uint8_t *data = afc_read_file(&c, path, &n);
    close(c.fd);
    if (!data) { reply_error(fd, id, "device_error", "could not read the file (missing, a directory, or over 64 MB)"); return; }
    size_t b64cap = ((n + 2) / 3) * 4 + 1;
    char *b64 = malloc(b64cap);
    size_t bn = b64 ? b64_encode(data, n, b64, b64cap) : 0;
    free(data);
    if (!bn && n) { free(b64); reply_error(fd, id, "internal_error", "could not encode the file"); return; }
    char head[1200];
    int hn = snprintf(head, sizeof head, "{\"id\":%ld,\"ok\":true,\"result\":{\"size\":%zu,\"data_b64\":\"", id, n);
    send_all(fd, head, (size_t)hn);
    if (bn) send_all(fd, b64, bn);
    send_all(fd, "\"}}\n", 4);
    free(b64);
}

/* Copy every crash report into a local directory; the user-facing action. */
static void method_export_crashes(int fd, long id, const char *line)
{
    const api_session *sess = g_session;
    char dir[1024] = "";
    json_string_field(line, "dir", dir, sizeof dir);
    if (!dir[0]) { reply_error(fd, id, "bad_request", "dir is required"); return; }
    if (!sess->crashcopy_port) { reply_error(fd, id, "unavailable", "the device did not offer crashreportcopymobile"); return; }
    crash_mover_poke(sess);
    afc_conn c; const char *err;
    if (afc_open(&c, sess, sess->crashcopy_port, &err) != 0) { reply_error(fd, id, "unavailable", err); return; }
    strbuf names = {0};
    if (afc_readdir(&c, "/", &names, 0) != 0 || !names.p) { close(c.fd); free(names.p); reply_error(fd, id, "device_error", "could not list crash reports"); return; }
    mkdir(dir, 0755);
    int copied = 0, failed = 0;
    /* names.p is [{"name":"..."},...]; walk it with the tiny JSON helper. */
    const char *p = names.p;
    while ((p = strstr(p, "\"name\":\"")) != NULL) {
        p += 8;
        const char *q = strchr(p, '"');
        if (!q) break;
        char name[512];
        size_t nl = (size_t)(q - p) < sizeof name - 1 ? (size_t)(q - p) : sizeof name - 1;
        memcpy(name, p, nl); name[nl] = 0;
        p = q + 1;
        if (!strstr(name, ".ips") && !strstr(name, ".crash") && !strstr(name, ".panic")) continue;
        char rpath[600], lpath[1600];
        snprintf(rpath, sizeof rpath, "/%s", name);
        snprintf(lpath, sizeof lpath, "%s/%s", dir, name);
        size_t n = 0;
        uint8_t *data = afc_read_file(&c, rpath, &n);
        if (!data) { failed++; continue; }
        FILE *f = fopen(lpath, "wb");
        if (f) { fwrite(data, 1, n, f); fclose(f); copied++; } else failed++;
        free(data);
    }
    close(c.fd);
    free(names.p);
    send_line(fd, "{\"id\":%ld,\"ok\":true,\"result\":{\"copied\":%d,\"failed\":%d,\"dir\":\"%s\"}}", id, copied, failed, dir);
}

/* ------------------------------------------------------------------ console (syslog_relay)
 *
 * After the checkin the relay simply streams the device's syslog as text, one NUL-terminated line
 * at a time, for as long as the connection lives. This is the one streaming method on the control
 * socket: the reply is followed by `{"event":"syslog","line":...}` objects until the client
 * closes its side (or sends anything at all), which ends the relay connection too.
 */
static void method_syslog(int fd, long id)
{
    const api_session *sess = g_session;
    if (!sess->syslog_port) {
        reply_error(fd, id, "unavailable", "the device did not offer syslog_relay");
        return;
    }
    int s = relay_open(sess, sess->syslog_port, 5);
    if (s < 0) { reply_error(fd, id, "unavailable", "cannot reach syslog_relay through the tunnel"); return; }
    const char *err = relay_checkin(s);
    if (err) { close(s); reply_error(fd, id, "internal_error", err); return; }
    send_line(fd, "{\"id\":%ld,\"ok\":true,\"result\":{\"streaming\":true}}", id);

    char acc[16384];
    size_t acc_len = 0;
    for (;;) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        FD_SET(fd, &rd);
        struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
        int n = select((s > fd ? s : fd) + 1, &rd, NULL, NULL, &tv);
        if (n < 0) break;
        if (FD_ISSET(fd, &rd)) break;            /* client spoke or hung up: done */
        if (!FD_ISSET(s, &rd)) continue;
        ssize_t r = recv(s, acc + acc_len, sizeof acc - acc_len - 1, 0);
        if (r <= 0) break;
        acc_len += (size_t)r;

        size_t start = 0;
        for (size_t i = 0; i < acc_len; i++) {
            if (acc[i] != '\n' && acc[i] != '\0') continue;
            acc[i] = 0;
            if (i > start) {
                strbuf b = {0};
                sb_puts(&b, "{\"event\":\"syslog\",\"line\":");
                sb_json_string(&b, acc + start);
                sb_puts(&b, "}\n");
                if (b.p) send_all(fd, b.p, b.len);
                free(b.p);
            }
            start = i + 1;
        }
        memmove(acc, acc + start, acc_len - start);
        acc_len -= start;
        if (acc_len == sizeof acc - 1) acc_len = 0;   /* a line longer than the buffer: drop it */
    }
    close(s);
}

/* ------------------------------------------------------------------ methods */

/* A way out that needs no signal and no sudo: the app (or `printf '{"method":"quit"}' | nc`)
 * can stop the daemon. Replies first so the caller sees it was heard. */
static void method_quit(int fd, long id)
{
    send_line(fd, "{\"id\":%ld,\"ok\":true,\"result\":{\"quitting\":true}}", id);
    fprintf(stderr, "  quit requested over the API; cdhost exiting\n");
    _exit(0);
}

/* Device info for the inspector. Any attached device, not only the bound one -- the sidebar lets
 * you look at a phone before switching to it. Built in a heap buffer: thirty rows of JSON. */
static void method_device_info(int fd, long id, const char *line)
{
    char udid[64] = {0};
    json_string_field(line, "udid", udid, sizeof udid);
    if (!udid[0] && g_session->udid) snprintf(udid, sizeof udid, "%s", g_session->udid);
    if (!udid[0]) { reply_error(fd, id, "bad_request", "udid is required"); return; }
    size_t cap = 64 * 1024;
    char *json = malloc(cap);
    if (!json) { reply_error(fd, id, "internal_error", "out of memory"); return; }
    if (cdhost_device_info(udid, json, cap) != 0) {
        /* json holds {"error": "..."}; lift the message out. */
        char msg[256] = "device info failed";
        json_string_field(json, "error", msg, sizeof msg);
        free(json);
        reply_error(fd, id, "device_error", msg);
        return;
    }
    strbuf b = {0};
    sb_printf(&b, "{\"id\":%ld,\"ok\":true,\"result\":", id);
    sb_puts(&b, json);
    sb_puts(&b, "}\n");
    if (b.p && !b.oom) send_all(fd, b.p, b.len);
    free(b.p);
    free(json);
}

/* Mount the developer disk image now (the app's manual trigger, e.g. after a reboot the daemon
 * did not see). Same flow the daemon runs on startup. */
static void method_activate(int fd, long id)
{
    const api_session *s = g_session;
    if (!s->mounter_port) { reply_error(fd, id, "unavailable", "the device did not offer mobile_image_mounter"); return; }
    int rc = cdhost_ddi_activate(s->tunnel_addr, s->mounter_port, s->ddi_dir);
    if (rc == RP_DDI_OK || rc == RP_DDI_ALREADY)
        send_line(fd, "{\"id\":%ld,\"ok\":true,\"result\":{\"mounted\":true,\"already\":%s}}", id, rc == RP_DDI_ALREADY ? "true" : "false");
    else if (rc == RP_DDI_LOCKED)  reply_error(fd, id, "locked", "unlock the device and try again");
    else if (rc == RP_DDI_NO_DDI)  reply_error(fd, id, "no_ddi", "the DDI files are not available on this host");
    else if (rc == RP_DDI_TSS_FAILED) reply_error(fd, id, "tss_failed", "Apple's signing server refused or was unreachable");
    else reply_error(fd, id, "internal_error", "could not mount the developer disk image");
}

static void method_ping(int fd, long id)
{
    send_line(fd, "{\"id\":%ld,\"ok\":true,\"result\":{\"engine\":\"cdhostd\",\"language\":\"c\"}}", id);
}

/* Bind a different device, so the sidebar selects what is mirrored the way Device Hub does.
 *
 * Replies BEFORE rebinding, because cdhost_rebind re-executes and never returns -- a client
 * waiting on a response would otherwise see the socket close and report an error for something
 * that worked. */
static void method_select_device(int fd, long id, const char *line)
{
    char udid[128] = {0};
    if (json_string_field(line, "udid", udid, sizeof udid) != 0 || !udid[0]) {
        reply_error(fd, id, "bad_request", "udid is required");
        return;
    }
    if (g_session->udid && !strcmp(g_session->udid, udid)) {
        send_line(fd, "{\"id\":%ld,\"ok\":true,\"result\":{\"rebinding\":false}}", id);
        return;
    }

    api_device devs[API_MAX_DEVICES];
    int n = usbmux_enumerate(devs, API_MAX_DEVICES);
    int known = 0;
    for (int i = 0; i < n; i++) if (!strcmp(devs[i].udid, udid)) { known = 1; break; }
    if (!known) {
        reply_error(fd, id, "unavailable", "that device is not attached");
        return;
    }

    send_line(fd, "{\"id\":%ld,\"ok\":true,\"result\":{\"rebinding\":true}}", id);
    /* Give the reply a moment to leave before the process is replaced. */
    usleep(150000);
    cdhost_rebind(udid);
}

static void method_list_devices(int fd, long id)
{
    const api_session *s = g_session;
    api_device devs[API_MAX_DEVICES];
    int n = usbmux_enumerate(devs, API_MAX_DEVICES);

    /* Every attached device, not just the one this daemon bound to. The sidebar used to show a
     * single hardcoded entry built from the session, so a Mac with two phones showed one and the
     * other looked disconnected -- and "connection" always read "usb" even over wifi. Only the
     * bound device carries a screen size; names, versions and product types come from
     * usbmux_enumerate's own lockdown queries, with the udid as the fallback title. */
    char buf[4096];
    int off = snprintf(buf, sizeof buf, "{\"id\":%ld,\"ok\":true,\"result\":{\"devices\":[", id);
    int wrote = 0;
    for (int i = 0; i < n && off < (int)sizeof buf - 512; i++) {
        int bound = s->udid && !strcmp(devs[i].udid, s->udid);
        const char *name = devs[i].name[0] ? devs[i].name : bound ? s->device_name : devs[i].udid;
        const char *ver = devs[i].version[0] ? devs[i].version : bound ? s->product_version : "";
        off += snprintf(buf + off, sizeof buf - off,
                        "%s{\"udid\":\"%s\",\"name\":\"%s\","
                        "\"os_version\":\"%s\",\"product_version\":\"%s\","
                        "\"product_type\":\"%s\","
                        "\"screen_width\":%d,\"screen_height\":%d,"
                        "\"connection\":\"%s\",\"bound\":%s}",
                        wrote ? "," : "", devs[i].udid, name, ver, ver, devs[i].product_type,
                        bound ? s->screen_w : 0, bound ? s->screen_h : 0,
                        strcmp(devs[i].connection, "Network") ? "usb" : "wifi",
                        bound ? "true" : "false");
        wrote++;
    }
    /* usbmuxd can be unreachable while the session is very much alive -- it is a separate daemon
     * and the tunnel does not depend on it once up. Reporting nothing then would blank a sidebar
     * that is actively mirroring, so fall back to the session we know we have. */
    if (!wrote && s->udid) {
        off += snprintf(buf + off, sizeof buf - off,
                        "{\"udid\":\"%s\",\"name\":\"%s\","
                        "\"os_version\":\"%s\",\"product_version\":\"%s\","
                        "\"screen_width\":%d,\"screen_height\":%d,"
                        "\"connection\":\"usb\",\"bound\":true}",
                        s->udid, s->device_name, s->product_version, s->product_version,
                        s->screen_w, s->screen_h);
    }
    snprintf(buf + off, sizeof buf - off, "]}}");
    send_line(fd, "%s", buf);
}

static void method_stream_info(int fd, long id)
{
    const api_session *s = g_session;
    /* Honest zeros. The media stream is not implemented in C yet, and reporting plausible
     * numbers for a stream that does not exist would make the app look connected to nothing. */
    uint64_t packets = 0, nals = 0, keys = 0, lost = 0, acks = 0;
    double mbps = 0;
    uint64_t bad = 0, late = 0, dup = 0;
    media_stats(g_media, &packets, &nals, &keys, &lost, &acks, &mbps, &bad, &late, &dup);
    double loss_pct = (packets + lost) ? 100.0 * (double)lost / (double)(packets + lost) : 0.0;
    send_line(fd,
              "{\"id\":%ld,\"ok\":true,\"result\":{"
              "\"port\":%d,\"codec\":\"hevc\",\"container\":\"annexb\",\"viewers\":%d,"
              "\"nals\":%llu,\"rtp_packets\":%llu,\"keyframes\":%llu,\"rtp_lost\":%llu,"
              "\"loss_pct\":%.2f,\"mbps\":%.2f,\"ltr_acked\":%llu,\"rtp_malformed\":%llu,\"rtp_late\":%llu,\"rtp_dup\":%llu,"
              "\"engine\":\"cdhostd\",\"streaming\":%s,\"viewer_drops\":%llu,"
              "\"display_service_port\":%ld,\"hid_service_port\":%ld}}",
              id, STREAM_PORT, viewer_count,
              (unsigned long long)nals, (unsigned long long)packets,
              (unsigned long long)keys, (unsigned long long)lost,
              loss_pct, mbps, (unsigned long long)acks, (unsigned long long)bad,
              (unsigned long long)late, (unsigned long long)dup,
              g_media ? "true" : "false", (unsigned long long)viewer_drops_total,
              s->display_port, s->hid_port);
}

/* ------------------------------------------------------------------ video fan-out
 *
 * Viewers get the cached parameter sets and the most recent keyframe first, then the live NALs.
 * Without that a viewer joining mid-stream has nothing to configure a decoder with and shows
 * nothing at all.
 */
#define MAX_VIEWERS 8

static pthread_mutex_t viewers_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    int      fd;
    uint64_t dropped;      /* NALs this viewer could not keep up with */
    int      behind;       /* consecutive drops, so a hopeless viewer gets closed */
} viewers[MAX_VIEWERS];

/* Cached so a late viewer can start decoding. One slot per parameter-set TYPE (VPS/SPS/PPS;
 * h264 uses the last two), overwritten in place -- the old grow-only cache overflowed after a
 * few hundred keyframes' worth of re-sent sets and reset MID-SET, after which a joining viewer
 * could receive an SPS+PPS with no VPS and decode nothing until the next in-band set, which a
 * static screen never sends. */
static uint8_t ps_cache[3][512];
static size_t  ps_len[3];
static uint8_t keyframe_cache[RP_RTP_MAX_NAL + 4];
static size_t  keyframe_len;

/* Which slot a parameter-set NAL belongs in. `annexb` starts with the 4-byte start code. Both
 * grammars are tried; the HEVC read of an h264 SPS/PPS lands outside 32..34 and vice versa, so
 * the classification is unambiguous for NALs media.c already flagged as parameter sets. */
static int ps_slot(const uint8_t *annexb, size_t len)
{
    if (len < 5) return -1;
    int hevc_t = (annexb[4] >> 1) & 0x3F;
    if (hevc_t >= 32 && hevc_t <= 34) return hevc_t - 32;
    int avc_t = annexb[4] & 0x1F;
    if (avc_t == 7) return 1;
    if (avc_t == 8) return 2;
    return -1;
}

/* Never block the caller.
 *
 * This runs on the RTP receive thread. A blocking send here couples a TCP consumer to a lossy
 * real-time producer: a slow viewer fills its socket buffer, send() blocks, the thread stops
 * calling recvfrom(), the UDP buffer overflows and packets are lost -- and because the device
 * sends one IDR per session, that loss is permanent corruption of the picture for everyone. The
 * viewer would never appear in the diagnosis.
 *
 * So the sockets are non-blocking and a viewer that cannot keep up loses data instead. That is
 * the right way round: dropping frames for one slow consumer is recoverable -- the periodic
 * keyframe repairs it within a few seconds -- while dropping RTP packets is not.
 */
static void viewers_write(const uint8_t *data, size_t len)
{
    pthread_mutex_lock(&viewers_lock);
    for (int i = 0; i < viewer_count; ) {
        size_t off = 0;
        int dead = 0, dropped = 0;
        while (off < len) {
            ssize_t w = send(viewers[i].fd, data + off, len - off, 0);
            if (w > 0) { off += (size_t)w; continue; }
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { dropped = 1; break; }
            dead = 1;
            break;
        }
        if (dead) {
            close(viewers[i].fd);
            viewers[i] = viewers[--viewer_count];
            continue;
        }
        if (dropped) {
            viewers[i].dropped++;
            viewer_drops_total++;
            /* A viewer that has not accepted anything for a long stretch is not coming back;
             * holding the slot only misleads whoever reads the viewer count. */
            if (++viewers[i].behind > 600) {
                printf("  dropping a viewer that stopped reading (%llu NALs behind)\n",
                       (unsigned long long)viewers[i].dropped);
                close(viewers[i].fd);
                viewers[i] = viewers[--viewer_count];
                continue;
            }
        } else {
            viewers[i].behind = 0;
        }
        i++;
    }
    pthread_mutex_unlock(&viewers_lock);
}

static void on_media_nal(void *ctx, const uint8_t *annexb, size_t len,
                         int is_parameter_set, int is_keyframe, int end_of_frame)
{
    (void)ctx;
    /* End-of-frame is a signal, not data: no bytes, nothing to cache or forward. The proxy
     * stream is Annex-B, which carries no frame boundaries anyway. */
    if (end_of_frame || !annexb || !len) return;
    if (is_parameter_set) {
        int slot = ps_slot(annexb, len);
        if (slot >= 0 && len <= sizeof ps_cache[0]) {
            memcpy(ps_cache[slot], annexb, len);
            ps_len[slot] = len;
        }
    } else if (is_keyframe && len <= sizeof keyframe_cache) {
        memcpy(keyframe_cache, annexb, len);
        keyframe_len = len;
    }
    viewers_write(annexb, len);
}

static void viewer_add(int fd)
{
    /* Non-blocking, so a viewer can never stall the RTP thread. */
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    /* A generous socket buffer absorbs bursts -- a keyframe is ~120 kB arriving at once -- so a
     * viewer only loses data if it is genuinely not reading. */
    int sndbuf = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);

    pthread_mutex_lock(&viewers_lock);
    if (viewer_count >= MAX_VIEWERS) { pthread_mutex_unlock(&viewers_lock); close(fd); return; }
    viewers[viewer_count].fd = fd;
    viewers[viewer_count].dropped = 0;
    viewers[viewer_count].behind = 0;
    viewer_count++;
    pthread_mutex_unlock(&viewers_lock);

    /* Prime this viewer so it can decode from its first frame: the full parameter set in
     * VPS, SPS, PPS order, then the last keyframe. */
    for (int i = 0; i < 3; i++)
        if (ps_len[i]) send(fd, ps_cache[i], ps_len[i], 0);
    if (keyframe_len) send(fd, keyframe_cache, keyframe_len, 0);
}

/* The stream is deferred until someone is watching, because the device sends its only unprompted
 * IDR at stream start -- starting early means the first viewer misses it. */
static void ensure_media(void)
{
    if (g_media) return;
    const api_session *s = g_session;
    if (!s->display_port) { printf("  no displayservice port; cannot stream\n"); return; }
    media_config cfg = {
        .device_addr = s->tunnel_addr,
        .our_addr = s->our_addr,
        .display_port = s->display_port,
        .ssrc = s->ssrc,
        .keyframe_every_s = s->keyframe_every_s,
    };
    printf("  starting the media stream (viewer connected)\n");
    g_media = media_start(&cfg, on_media_nal, NULL);
    if (!g_media) printf("  media stream failed to start\n");
}

/* Everything a player needs to talk to the device itself.
 *
 * The tunnel addresses are ordinary routes once the daemon has created the utun, so an
 * unprivileged process can open sockets on them directly -- no interface handle, no privilege.
 * Handing these out is what lets the player negotiate and receive RTP with no proxy in the data
 * path at all. Note the addresses change every session: a fresh ULA prefix per tunnel, so a
 * player must re-query rather than cache them across reconnects.
 */
static void method_tunnel_info(int fd, long id)
{
    const api_session *s = g_session;
    send_line(fd,
              "{\"id\":%ld,\"ok\":true,\"result\":{"
              "\"device_addr\":\"%s\",\"our_addr\":\"%s\",\"rsd_port\":%ld,"
              "\"services\":{\"displayservice\":%ld,\"hid\":%ld,\"screenshot\":%ld,"
              "\"diagnostics_relay\":%ld,\"appservice\":%ld,\"syslog_relay\":%ld,\"installation_proxy\":%ld,"
              "\"misagent\":%ld,\"mcinstall\":%ld,\"afc\":%ld,\"crash_copy\":%ld},"
              "\"udid\":\"%s\",\"screen_width\":%d,\"screen_height\":%d}}",
              id, s->tunnel_addr ? s->tunnel_addr : "",
              s->our_addr ? s->our_addr : "", s->rsd_port,
              s->display_port, s->hid_port, s->screenshot_port,
              s->diag_port, s->app_port, s->syslog_port, s->instproxy_port,
              s->misagent_port, s->mcinstall_port, s->afc_port, s->crashcopy_port,
              s->udid, s->screen_w, s->screen_h);
}

/* ------------------------------------------------------------------ server */

static int listen_on(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* local only: this is a developer tool */
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 8) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void dispatch(int fd, const char *line)
{
    long id = json_number_field(line, "id", 0);
    char method[64] = {0};
    if (json_string_field(line, "method", method, sizeof method) != 0) {
        reply_error(fd, id, "bad_request", "no method");
        return;
    }

    if (!strcmp(method, "ping"))              { method_ping(fd, id); return; }
    if (!strcmp(method, "quit"))              { method_quit(fd, id); return; }
    if (!strcmp(method, "activate"))          { method_activate(fd, id); return; }
    if (!strcmp(method, "device_info"))       { method_device_info(fd, id, line); return; }
    if (!strcmp(method, "list_devices"))      { method_list_devices(fd, id); return; }
    if (!strcmp(method, "stream_info"))       { method_stream_info(fd, id); return; }
    if (!strcmp(method, "press_button"))     { method_button(fd, id, line); return; }
    if (!strcmp(method, "device_action"))    { method_device_action(fd, id, line); return; }
    if (!strcmp(method, "list_apps"))        { method_list_apps(fd, id, line); return; }
    if (!strcmp(method, "list_processes"))   { method_list_processes(fd, id); return; }
    if (!strcmp(method, "launch_app"))       { method_launch_app(fd, id, line); return; }
    if (!strcmp(method, "terminate_app"))    { method_terminate_app(fd, id, line); return; }
    if (!strcmp(method, "install_app"))      { method_install_app(fd, id, line); return; }
    if (!strcmp(method, "uninstall_app"))    { method_uninstall_app(fd, id, line); return; }
    if (!strcmp(method, "syslog"))           { method_syslog(fd, id); return; }
    if (!strcmp(method, "list_profiles"))    { method_list_profiles(fd, id); return; }
    if (!strcmp(method, "install_profile"))  { method_install_profile(fd, id, line); return; }
    if (!strcmp(method, "remove_profile"))   { method_remove_profile(fd, id, line); return; }
    if (!strcmp(method, "list_dir"))         { method_list_dir(fd, id, line); return; }
    if (!strcmp(method, "read_file"))        { method_read_file(fd, id, line); return; }
    if (!strcmp(method, "export_crashes"))   { method_export_crashes(fd, id, line); return; }
    if (!strcmp(method, "select_device"))    { method_select_device(fd, id, line); return; }
    if (!strcmp(method, "tunnel_info"))       { method_tunnel_info(fd, id); return; }
    if (!strcmp(method, "take_screenshot"))   { method_screenshot(fd, id); return; }
    if (!strcmp(method, "tap"))               { method_touch(fd, id, line, 0); return; }
    if (!strcmp(method, "swipe"))             { method_touch(fd, id, line, 1); return; }

    /* Named explicitly rather than lumped into one message, so the log says which capability is
     * missing rather than that something is. */
    char msg[160];
    snprintf(msg, sizeof msg,
             "%s is not implemented by the C engine yet; run the Python engine for it", method);
    reply_error(fd, id, "not_implemented", msg);
}

/* One thread per client.
 *
 * A service call means connecting into the tunnel, running a RemoteXPC handshake and waiting for
 * a reply, and a swipe additionally paces samples over hundreds of milliseconds. Doing that on
 * the accept loop meant one request froze the daemon -- including its ability to answer a ping,
 * which is exactly when you most want it to respond. Clients are few and this is a developer
 * tool, so a thread each is the simple correct answer rather than making every call asynchronous.
 */
static void *client_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char in[8192];
    size_t in_len = 0;

    for (;;) {
        ssize_t r = recv(fd, in + in_len, sizeof in - in_len - 1, 0);
        if (r <= 0) break;
        in_len += (size_t)r;
        in[in_len] = 0;

        char *start = in;
        for (;;) {
            char *nl = strchr(start, '\n');
            if (!nl) break;
            *nl = 0;
            if (*start) dispatch(fd, start);
            start = nl + 1;
        }
        size_t left = in_len - (size_t)(start - in);
        memmove(in, start, left);
        in_len = left;
        if (in_len == sizeof in - 1) in_len = 0;      /* oversized line: drop it */
    }
    close(fd);
    return NULL;
}

static void spawn_client(int fd)
{
    pthread_t t;
    if (pthread_create(&t, NULL, client_thread, (void *)(intptr_t)fd) != 0) { close(fd); return; }
    pthread_detach(t);
}

int api_serve(api_session *session)
{
    g_session = session;
    signal(SIGPIPE, SIG_IGN);      /* a viewer closing mid-write must not kill the daemon */

    /* The Annex-B fan-out is optional now that a player can take RTP directly.
     *
     * Kept because a proxy is still the right shape for anything that is not the app -- ffplay,
     * a recorder, a second viewer -- and because the Python engine's clients expect it. Set
     * RPLAY_NO_VIDEO_PROXY=1 to leave it off and keep the daemon strictly out of the data path.
     */
    const char *nopx = getenv("RPLAY_NO_VIDEO_PROXY");
    int want_proxy = !(nopx && *nopx == '1');

    int api_fd = listen_on(API_PORT);
    int video_fd = want_proxy ? listen_on(STREAM_PORT) : -1;
    if (api_fd < 0 || (want_proxy && video_fd < 0)) {
        fprintf(stderr, "  cannot listen on %d/%d: %s\n", API_PORT, STREAM_PORT, strerror(errno));
        fprintf(stderr, "  another cdhost is probably still running -- this one can serve nobody,\n"
                        "  so it is stopping rather than pumping packets for no viewer.\n");
        return -1;
    }
    printf("  control: 127.0.0.1:%d (JSON lines)\n", API_PORT);
    if (want_proxy)
        printf("  video:   127.0.0.1:%d (Annex-B fan-out, optional; a player may instead take "
               "RTP directly)\n", STREAM_PORT);
    else
        printf("  video:   off (RPLAY_NO_VIDEO_PROXY=1) — players take RTP directly\n");

    for (;;) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(api_fd, &rd);
        if (video_fd >= 0) FD_SET(video_fd, &rd);
        int maxfd = api_fd > video_fd ? api_fd : video_fd;
        if (select(maxfd + 1, &rd, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (FD_ISSET(api_fd, &rd)) {
            int fd = accept(api_fd, NULL, NULL);
            if (fd >= 0) {
                int one = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                printf("  client connected\n");
                spawn_client(fd);
            }
        }
        if (video_fd >= 0 && FD_ISSET(video_fd, &rd)) {
            /* Accepted and held. The app treats a refused video port as a fatal disconnect and
             * retries in a loop, so refusing would look like a broken engine rather than one
             * whose streaming is not written yet. */
            int fd = accept(video_fd, NULL, NULL);
            if (fd >= 0) {
                int one = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                printf("  viewer connected (%d total)\n", viewer_count + 1);
                ensure_media();
                viewer_add(fd);
                media_request_keyframe(g_media);   /* a static screen would leave it black */
            }
        }
    }
    close(api_fd);
    if (video_fd >= 0) close(video_fd);
    return 0;
}
