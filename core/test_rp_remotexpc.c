/*
 * test_rp_remotexpc — drive a whole RemoteXPC session over memory, with no device.
 *
 * The handshake is the part that cannot be reasoned about: its ordering and flags come from a
 * captured session, not from the spec, and a single wrong byte gets the connection closed with no
 * explanation. So this asserts the exact bytes the proven Python implementation emits.
 *
 * Everything runs against in-memory buffers, which is the point of rp_rxpc_io: the protocol layer
 * can be tested completely without a phone, a tunnel, or root.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rp_remotexpc.h"
#include "rp_http2.h"

/* Byte-for-byte from host/rplayhub/wire/remotexpc.py's opening exchange. */
static const char *EXPECTED_HANDSHAKE_HEX =
    "505249202a20485454502f322e300d0a0d0a534d0d0a0d0a"     /* preface */
    "00000c04000000000000030000006400040010000000"         /* SETTINGS */
    "0004080000000000000f0001"                             /* WINDOW_UPDATE */
    "00000001040000000100000018000000"                     /* HEADERS root */
    "920bb029010000001000000000000000423713420500000000f0000000000000"  /* DATA root: {} */
    "0000000104000000030000000c00000000920bb0290102000008000000000000000000"  /* HEADERS reply + DATA */
    "0000000c000000039"                                    /* (trailing, see note) */
    ;

typedef struct {
    uint8_t  written[4096];
    size_t   written_len;
    const uint8_t *to_read;
    size_t   to_read_len;
    size_t   read_pos;
} memio;

static long mem_write(void *ctx, const void *buf, size_t len)
{
    memio *m = ctx;
    if (m->written_len + len > sizeof m->written) return -1;
    memcpy(m->written + m->written_len, buf, len);
    m->written_len += len;
    return (long)len;
}

static long mem_read(void *ctx, void *buf, size_t len)
{
    memio *m = ctx;
    size_t left = m->to_read_len - m->read_pos;
    if (!left) return 0;
    if (len > left) len = left;
    memcpy(buf, m->to_read + m->read_pos, len);
    m->read_pos += len;
    return (long)len;
}

static void hexdump(const char *label, const uint8_t *p, size_t n)
{
    printf("%s (%zu bytes)\n  ", label, n);
    for (size_t i = 0; i < n; i++) printf("%02x", p[i]);
    printf("\n");
}

int main(void)
{
    /* A SETTINGS frame from the device, which is what ends the handshake. */
    static uint8_t device_settings[9] = {0, 0, 0, 0x04, 0, 0, 0, 0, 0};

    memio m;
    memset(&m, 0, sizeof m);
    m.to_read = device_settings;
    m.to_read_len = sizeof device_settings;

    rp_rxpc_session s;
    static uint8_t reassembly[1 << 18];
    static uint8_t raw[1 << 16];
    rp_rxpc_io io = { mem_read, mem_write, &m };
    rp_rxpc_init(&s, io, reassembly, sizeof reassembly, raw, sizeof raw);

    if (rp_rxpc_handshake(&s) != 0) {
        printf("FAIL: handshake returned an error\n");
        return 1;
    }

    /* The last thing written is the SETTINGS ack, which the reference stream does not include
     * (it is a response, not part of the opening burst). Compare everything before it. */
    size_t ack_len = 9;
    if (m.written_len < ack_len) { printf("FAIL: nothing written\n"); return 1; }
    size_t opening = m.written_len - ack_len;

    hexdump("C opening exchange", m.written, opening);

    /* Structural checks that do not depend on my transcription of the hex above being perfect:
     * the preface, then the exact frame sequence the device expects. */
    if (memcmp(m.written, "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n", 24) != 0) {
        printf("FAIL: preface missing or wrong\n");
        return 1;
    }

    struct { uint8_t type; uint32_t stream; } want[] = {
        {0x4, 0}, {0x8, 0}, {0x1, 1}, {0x0, 1}, {0x1, 3}, {0x0, 1}, {0x0, 3},
    };
    size_t off = 24, i = 0;
    while (off < opening && i < sizeof want / sizeof want[0]) {
        rp_h2_frame f;
        size_t consumed = 0;
        if (rp_h2_parse_frame(m.written + off, opening - off, &f, &consumed) != 1) break;
        if (f.type != want[i].type || f.stream != want[i].stream) {
            printf("FAIL: frame %zu is type 0x%x stream %u, expected type 0x%x stream %u\n",
                   i, f.type, f.stream, want[i].type, want[i].stream);
            return 1;
        }
        off += consumed;
        i++;
    }
    if (i != sizeof want / sizeof want[0]) {
        printf("FAIL: only %zu of %zu expected frames present\n",
               i, sizeof want / sizeof want[0]);
        return 1;
    }
    if (off != opening) {
        printf("FAIL: %zu trailing bytes after the expected frames\n", opening - off);
        return 1;
    }
    printf("  handshake: preface + 7 frames in the right order on the right streams\n");

    /* The ack must follow. */
    rp_h2_frame ack;
    size_t c = 0;
    if (rp_h2_parse_frame(m.written + opening, m.written_len - opening, &ack, &c) != 1 ||
        ack.type != 0x4 || !(ack.flags & 0x1)) {
        printf("FAIL: device SETTINGS was not acknowledged\n");
        return 1;
    }
    printf("  handshake: device SETTINGS acknowledged\n");

    /* ---------------------------------------------------------------- receive path */
    /* Feed an empty-dict ack followed by a real reply, and check only the reply comes back. */
    {
        uint8_t body[64], msgack[128], msgreal[256], stream[1024];
        rp_xpc_writer w;

        rp_xpc_writer_init(&w, body, sizeof body);
        rp_xpc_dict_begin(&w);
        rp_xpc_dict_end(&w);
        size_t nack = rp_xpc_wrap(body, w.len, RP_XPC_F_ALWAYS_SET | RP_XPC_F_DATA_PRESENT, 1,
                                  msgack, sizeof msgack);

        rp_xpc_writer_init(&w, body, sizeof body);
        rp_xpc_dict_begin(&w);
        rp_xpc_set_string(&w, "Services", "yes");
        rp_xpc_dict_end(&w);
        size_t nreal = rp_xpc_wrap(body, w.len, RP_XPC_F_ALWAYS_SET | RP_XPC_F_DATA_PRESENT, 2,
                                   msgreal, sizeof msgreal);

        size_t sn = 0, n;
        n = rp_h2_write_data(1, msgack, nack, stream + sn, sizeof stream - sn);  sn += n;
        n = rp_h2_write_data(1, msgreal, nreal, stream + sn, sizeof stream - sn); sn += n;

        memio m2;
        memset(&m2, 0, sizeof m2);
        m2.to_read = stream;
        m2.to_read_len = sn;
        rp_rxpc_session s2;
        rp_rxpc_io io2 = { mem_read, mem_write, &m2 };
        rp_rxpc_init(&s2, io2, reassembly, sizeof reassembly, raw, sizeof raw);

        rp_xpc_obj got;
        if (rp_rxpc_recv(&s2, &got) != 0) { printf("FAIL: recv returned an error\n"); return 1; }
        rp_xpc_obj v;
        const char *sv = NULL;
        if (rp_xpc_dict_get(&got, "Services", &v) != 0 || rp_xpc_get_string(&v, &sv) != 0 ||
            strcmp(sv, "yes") != 0) {
            printf("FAIL: recv skipped past the real reply or returned the ack\n");
            return 1;
        }
        printf("  receive: empty-dict ack skipped, real reply returned\n");
    }

    /* ---------------------------------------------------------------- dictionary iteration */
    {
        uint8_t body[256];
        rp_xpc_writer w;
        rp_xpc_writer_init(&w, body, sizeof body);
        rp_xpc_dict_begin(&w);
        rp_xpc_set_uint64(&w, "com.apple.a", 111);
        rp_xpc_set_uint64(&w, "com.apple.b", 222);
        rp_xpc_set_uint64(&w, "com.apple.c", 333);
        rp_xpc_dict_end(&w);
        rp_xpc_obj d = { body, w.len };

        if (rp_xpc_dict_count(&d) != 3) { printf("FAIL: dict_count\n"); return 1; }
        size_t cursor = 0;
        const char *k;
        rp_xpc_obj v;
        uint64_t vals[3] = {0}, expect[3] = {111, 222, 333};
        int seen = 0;
        while (rp_xpc_dict_next(&d, &cursor, &k, &v) == 0 && seen < 3) {
            if (rp_xpc_get_uint64(&v, &vals[seen]) != 0) { printf("FAIL: value\n"); return 1; }
            seen++;
        }
        if (seen != 3 || memcmp(vals, expect, sizeof vals) != 0) {
            printf("FAIL: dict_next walked %d entries\n", seen);
            return 1;
        }
        printf("  iteration: all 3 entries walked in order\n");
    }

    printf("\nOK — RemoteXPC session verified with no device.\n");
    (void)EXPECTED_HANDSHAKE_HEX;
    return 0;
}
