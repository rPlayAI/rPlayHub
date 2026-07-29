#include "rp_remotexpc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rp_http2.h"

/* Set RPLAY_XPC_DEBUG=1 to trace every frame and message. A receive loop that silently skips
 * things is impossible to diagnose from the outside -- which is how "the service never answered"
 * ended up meaning four different things over four hardware runs. */
static int xpc_debug(void)
{
    static int cached = -1;
    if (cached < 0) { const char *e = getenv("RPLAY_XPC_DEBUG"); cached = (e && *e == '1'); }
    return cached;
}

#define H2_DATA          0x0
#define H2_RST_STREAM    0x3
#define H2_SETTINGS      0x4
#define H2_GOAWAY        0x7
#define H2_FLAG_ACK      0x1

void rp_rxpc_init(rp_rxpc_session *s, rp_rxpc_io io,
                  uint8_t *reassembly, size_t reassembly_cap,
                  uint8_t *raw, size_t raw_cap)
{
    memset(s, 0, sizeof *s);
    s->io = io;
    s->buf = reassembly;
    s->buf_cap = reassembly_cap;
    s->raw = raw;
    s->raw_cap = raw_cap;
    s->next_message_id = 1;
}

static int write_all(rp_rxpc_session *s, const uint8_t *p, size_t n)
{
    while (n) {
        long w = s->io.write(s->io.ctx, p, n);
        if (w <= 0) return -1;
        p += (size_t)w;
        n -= (size_t)w;
    }
    return 0;
}

/* Pull one frame, reading from the transport until a whole one is buffered.
 *
 * The returned payload points into the raw buffer and stays valid until the NEXT call. That is
 * why consuming the previous frame is deferred to the top of this function rather than done at
 * the bottom: compacting the buffer immediately would move the bytes out from under the pointer
 * we just handed back. */
static int read_frame(rp_rxpc_session *s, rp_h2_frame *frame)
{
    if (s->pending_consume) {
        memmove(s->raw, s->raw + s->pending_consume, s->raw_len - s->pending_consume);
        s->raw_len -= s->pending_consume;
        s->pending_consume = 0;
    }
    for (;;) {
        size_t consumed = 0;
        if (rp_h2_parse_frame(s->raw, s->raw_len, frame, &consumed) == 1) {
            s->pending_consume = consumed;
            return 0;
        }
        if (s->raw_len == s->raw_cap) return -1;             /* a frame bigger than the buffer */
        long r = s->io.read(s->io.ctx, s->raw + s->raw_len, s->raw_cap - s->raw_len);
        if (r <= 0) return -1;
        s->raw_len += (size_t)r;
    }
}

int rp_rxpc_handshake(rp_rxpc_session *s)
{
    uint8_t out[512];
    uint8_t msg[128];
    size_t n, mlen;

    /* Written out step by step rather than via rp_h2_write_connection_start, because the order
     * here is not the obvious one: the root stream carries a message BETWEEN the two HEADERS
     * frames. This mirrors the Python that is proven against real devices; a tidier ordering is
     * not worth the risk of the device disagreeing. */
    if (!(n = rp_h2_write_preface(out, sizeof out))) return -1;
    if (write_all(s, out, n)) return -1;
    if (!(n = rp_h2_write_settings(100, RP_RXPC_INITIAL_WINDOW, out, sizeof out))) return -1;
    if (write_all(s, out, n)) return -1;
    if (!(n = rp_h2_write_window_update(0, RP_RXPC_INITIAL_WINDOW - 65535, out, sizeof out))) return -1;
    if (write_all(s, out, n)) return -1;

    if (!(n = rp_h2_write_headers(RP_RXPC_ROOT_STREAM, out, sizeof out))) return -1;
    if (write_all(s, out, n)) return -1;

    /* An empty dictionary, message id 0. Note the flags: DATA_PRESENT is NOT set even though a
     * payload follows, because the reference implementation keys that flag off the dictionary
     * being non-empty. Faithfulness beats tidiness here. */
    {
        rp_xpc_writer w;
        uint8_t body[32];
        rp_xpc_writer_init(&w, body, sizeof body);
        rp_xpc_dict_begin(&w);
        rp_xpc_dict_end(&w);
        if (!(mlen = rp_xpc_wrap(body, w.len, RP_XPC_F_ALWAYS_SET, 0, msg, sizeof msg))) return -1;
        if (!(n = rp_h2_write_data(RP_RXPC_ROOT_STREAM, msg, mlen, out, sizeof out))) return -1;
        if (write_all(s, out, n)) return -1;
    }

    if (!(n = rp_h2_write_headers(RP_RXPC_REPLY_STREAM, out, sizeof out))) return -1;
    if (write_all(s, out, n)) return -1;

    if (!(mlen = rp_xpc_wrap_empty(0x0201, 0, msg, sizeof msg))) return -1;
    if (!(n = rp_h2_write_data(RP_RXPC_ROOT_STREAM, msg, mlen, out, sizeof out))) return -1;
    if (write_all(s, out, n)) return -1;

    if (!(mlen = rp_xpc_wrap_empty(RP_XPC_F_ALWAYS_SET | RP_XPC_F_INIT_HANDSHAKE, 0,
                                   msg, sizeof msg))) return -1;
    if (!(n = rp_h2_write_data(RP_RXPC_REPLY_STREAM, msg, mlen, out, sizeof out))) return -1;
    if (write_all(s, out, n)) return -1;

    for (;;) {
        rp_h2_frame f;
        if (read_frame(s, &f) != 0) return -1;
        if (f.type == H2_GOAWAY || f.type == H2_RST_STREAM) return -1;
        if (f.type == H2_SETTINGS && !(f.flags & H2_FLAG_ACK)) {
            if (!(n = rp_h2_write_settings_ack(out, sizeof out))) return -1;
            return write_all(s, out, n);
        }
    }
}

int rp_rxpc_send(rp_rxpc_session *s, const uint8_t *body, size_t body_len, int wanting_reply)
{
    /* One buffer for the whole frame. Requests are small -- the largest is a media-stream offer
     * of a few hundred bytes -- so this stays well inside the 16 KB default frame limit, and
     * building header and payload together avoids a partial write splitting them. */
    static uint8_t frame[16384];
    uint8_t msg[8192];

    uint32_t flags = RP_XPC_F_ALWAYS_SET;
    if (body_len) flags |= RP_XPC_F_DATA_PRESENT;
    if (wanting_reply) flags |= RP_XPC_F_WANTING_REPLY;

    size_t mlen = rp_xpc_wrap(body, body_len, flags, s->next_message_id++, msg, sizeof msg);
    if (!mlen) return -1;

    size_t n = rp_h2_write_data(RP_RXPC_ROOT_STREAM, msg, mlen, frame, sizeof frame);
    if (!n) return -1;
    return write_all(s, frame, n);
}

int rp_rxpc_recv(rp_rxpc_session *s, rp_xpc_obj *obj)
{
    uint8_t out[64];
    for (;;) {
        rp_h2_frame f;
        if (read_frame(s, &f) != 0) return -1;

        if (f.type == H2_GOAWAY || f.type == H2_RST_STREAM) return -1;
        if (f.type == H2_SETTINGS && !(f.flags & H2_FLAG_ACK)) {
            size_t n = rp_h2_write_settings_ack(out, sizeof out);
            if (!n || write_all(s, out, n)) return -1;
            continue;
        }
        if (xpc_debug())
            fprintf(stderr, "    [xpc] frame type=0x%x flags=0x%x stream=%u len=%zu\n",
                    f.type, f.flags, f.stream, f.length);
        if (f.type != H2_DATA || !f.length || !f.payload) continue;

        /* One accumulator per stream. The device interleaves DATA on the root and reply streams,
         * and appending both to a single buffer produces a message that is two halves of
         * different replies -- which fails to parse, forever, while the bytes keep arriving. */
        int slot = (f.stream == RP_RXPC_REPLY_STREAM) ? 1 : 0;
        size_t half = s->buf_cap / 2;
        uint8_t *buf = s->buf + (size_t)slot * half;

        if (s->buf_len[slot] + f.length > half) {
            /* The reply is larger than the caller's buffer. Dropping it silently makes a service
             * look like it answered nothing, which is indistinguishable from a protocol fault --
             * so report it instead of discarding it quietly. */
            s->buf_len[slot] = 0;
            s->overflowed = 1;
            return -1;
        }
        memcpy(buf + s->buf_len[slot], f.payload, f.length);
        s->buf_len[slot] += f.length;

        uint32_t flags;
        uint64_t mid;
        rp_xpc_obj parsed;
        if (rp_xpc_unwrap(buf, s->buf_len[slot], &flags, &mid, &parsed) != 0) {
            if (xpc_debug())
                fprintf(stderr, "    [xpc] incomplete: %zu bytes buffered on stream %u\n",
                        s->buf_len[slot], f.stream);
            continue;      /* incomplete: keep the bytes and wait for the rest */
        }
        if (xpc_debug())
            fprintf(stderr, "    [xpc] message flags=0x%x id=%llu payload=%zu bytes%s\n",
                    flags, (unsigned long long)mid, parsed.size,
                    (parsed.size == 0 || rp_xpc_is_empty_dict(&parsed)) ? "  (SKIPPED as ack)" : "");
        s->buf_len[slot] = 0;

        /* Skip acknowledgements -- no payload, or an empty dictionary. The device sends one of
         * these before the real answer, and mistaking it for the answer is the classic way to
         * "receive" a reply that has not arrived. */
        if (parsed.size == 0 || rp_xpc_is_empty_dict(&parsed)) continue;

        if (obj) *obj = parsed;
        return 0;
    }
}
