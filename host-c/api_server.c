#include "api_server.h"

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
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>

#include "../core/rp_coredevice.h"
#include "media.h"
#include "../core/rp_xpc.h"

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


static long svc_read(void *ctx, void *buf, size_t len)  { return (long)recv(*(int *)ctx, buf, len, 0); }
static long svc_write(void *ctx, const void *buf, size_t len) { return (long)send(*(int *)ctx, buf, len, 0); }

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

    struct sockaddr_in6 sa;
    memset(&sa, 0, sizeof sa);
    sa.sin6_family = AF_INET6;
    sa.sin6_port = htons((uint16_t)port);
    if (inet_pton(AF_INET6, addr, &sa.sin6_addr) != 1) goto fail;
    c->fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (c->fd < 0) goto fail;

    /* Connect with a deadline. The default would block for over a minute on an unreachable
     * service, and the caller is answering a request while that happens. */
    int flags = fcntl(c->fd, F_GETFL, 0);
    fcntl(c->fd, F_SETFL, flags | O_NONBLOCK);
    int rc = connect(c->fd, (struct sockaddr *)&sa, sizeof sa);
    if (rc != 0) {
        if (errno != EINPROGRESS) goto fail;
        fd_set wr;
        FD_ZERO(&wr);
        FD_SET(c->fd, &wr);
        struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
        if (select(c->fd + 1, NULL, &wr, NULL, &tv) <= 0) goto fail;
        int err = 0;
        socklen_t elen = sizeof err;
        if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0) goto fail;
    }
    fcntl(c->fd, F_SETFL, flags);

    /* A full-screen PNG takes the device time to render and push over wifi. */
    struct timeval rtv = { .tv_sec = 30, .tv_usec = 0 };
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof rtv);

    rp_rxpc_io io = { svc_read, svc_write, &c->fd };
    rp_rxpc_init(&c->s, io, c->reassembly, SVC_REASSEMBLY, c->raw, 1 << 16);
    if (rp_rxpc_handshake(&c->s) != 0) goto fail;
    return 0;

fail:
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
    free(c->reassembly); c->reassembly = NULL;
    free(c->raw); c->raw = NULL;
    return -1;
}

static void svc_close(svc_conn *c)
{
    if (c->fd >= 0) close(c->fd);
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

/* ------------------------------------------------------------------ methods */

static void method_ping(int fd, long id)
{
    send_line(fd, "{\"id\":%ld,\"ok\":true,\"result\":{\"engine\":\"cdhostd\",\"language\":\"c\"}}", id);
}

static void method_list_devices(int fd, long id)
{
    const api_session *s = g_session;
    send_line(fd,
              "{\"id\":%ld,\"ok\":true,\"result\":{\"devices\":[{"
              "\"udid\":\"%s\",\"name\":\"%s\",\"product_version\":\"%s\","
              "\"screen_width\":%d,\"screen_height\":%d,\"connection\":\"usb\"}]}}",
              id, s->udid, s->device_name, s->product_version, s->screen_w, s->screen_h);
}

static void method_stream_info(int fd, long id)
{
    const api_session *s = g_session;
    /* Honest zeros. The media stream is not implemented in C yet, and reporting plausible
     * numbers for a stream that does not exist would make the app look connected to nothing. */
    uint64_t packets = 0, nals = 0, keys = 0, lost = 0, acks = 0;
    double mbps = 0;
    media_stats(g_media, &packets, &nals, &keys, &lost, &acks, &mbps);
    double loss_pct = (packets + lost) ? 100.0 * (double)lost / (double)(packets + lost) : 0.0;
    send_line(fd,
              "{\"id\":%ld,\"ok\":true,\"result\":{"
              "\"port\":%d,\"codec\":\"hevc\",\"container\":\"annexb\",\"viewers\":%d,"
              "\"nals\":%llu,\"rtp_packets\":%llu,\"keyframes\":%llu,\"rtp_lost\":%llu,"
              "\"loss_pct\":%.2f,\"mbps\":%.2f,\"ltr_acked\":%llu,"
              "\"engine\":\"cdhostd\",\"streaming\":%s,\"viewer_drops\":%llu,"
              "\"display_service_port\":%ld,\"hid_service_port\":%ld}}",
              id, STREAM_PORT, viewer_count,
              (unsigned long long)nals, (unsigned long long)packets,
              (unsigned long long)keys, (unsigned long long)lost,
              loss_pct, mbps, (unsigned long long)acks,
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

/* Cached so a late viewer can start decoding. */
static uint8_t param_cache[4096];
static size_t  param_len;
static uint8_t keyframe_cache[RP_RTP_MAX_NAL + 4];
static size_t  keyframe_len;

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
                         int is_parameter_set, int is_keyframe)
{
    (void)ctx;
    if (is_parameter_set) {
        /* Parameter sets accumulate: VPS, SPS and PPS are three separate NALs and a decoder
         * needs all three. Reset when one repeats, which is how a new set is signalled. */
        if (param_len + len > sizeof param_cache) param_len = 0;
        memcpy(param_cache + param_len, annexb, len);
        param_len += len;
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

    /* Prime this viewer so it can decode from its first frame. */
    if (param_len) send(fd, param_cache, param_len, 0);
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
    if (!strcmp(method, "list_devices"))      { method_list_devices(fd, id); return; }
    if (!strcmp(method, "stream_info"))       { method_stream_info(fd, id); return; }
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

    int api_fd = listen_on(API_PORT);
    int video_fd = listen_on(STREAM_PORT);
    if (api_fd < 0 || video_fd < 0) {
        fprintf(stderr, "  cannot listen on %d/%d: %s\n", API_PORT, STREAM_PORT, strerror(errno));
        return -1;
    }
    printf("  control: 127.0.0.1:%d (JSON lines)\n", API_PORT);
    printf("  video:   127.0.0.1:%d (Annex-B; the stream starts when a viewer connects)\n",
           STREAM_PORT);

    for (;;) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(api_fd, &rd);
        FD_SET(video_fd, &rd);
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
        if (FD_ISSET(video_fd, &rd)) {
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
            }
        }
    }
    close(api_fd);
    close(video_fd);
    return 0;
}
