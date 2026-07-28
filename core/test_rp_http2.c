/* Emits the RemoteXPC connection-start byte sequence as hex, so it can be diffed against the
 * Python implementation that is verified against a real device. Also exercises the parser,
 * including a frame arriving in pieces — the normal case on a socket. */
#include "rp_http2.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(c, ...) do { if(!(c)){ printf("  FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } } while(0)

int main(int argc, char **argv)
{
    uint8_t out[512];
    size_t n = rp_h2_write_connection_start(16 * 1024 * 1024, out, sizeof out);
    if (!n) { fprintf(stderr, "connection start failed\n"); return 1; }

    if (argc > 1 && strcmp(argv[1], "--hex") == 0) {
        for (size_t i = 0; i < n; i++) printf("%02x", out[i]);
        printf("\n");
        return 0;
    }

    size_t off = RP_H2_PREFACE_LEN, consumed = 0;
    rp_h2_frame f;
    int seen[16] = {0};
    while (off < n && rp_h2_parse_frame(out + off, n - off, &f, &consumed) == 1) {
        seen[f.type & 0xF]++;
        off += consumed;
    }
    CHECK(off == n, "parsed %zu of %zu bytes", off, n);
    CHECK(seen[RP_H2_SETTINGS] == 1, "one SETTINGS, got %d", seen[RP_H2_SETTINGS]);
    CHECK(seen[RP_H2_WINDOW_UPDATE] == 1, "one WINDOW_UPDATE, got %d", seen[RP_H2_WINDOW_UPDATE]);
    CHECK(seen[RP_H2_HEADERS] == 2, "two HEADERS (streams 1 and 3), got %d", seen[RP_H2_HEADERS]);
    printf("  connection start: preface + SETTINGS + WINDOW_UPDATE + 2 HEADERS — OK\n");

    uint8_t data[64];
    uint8_t payload[20] = {0};
    size_t dn = rp_h2_write_data(RP_H2_STREAM_ROOT, payload, sizeof payload, data, sizeof data);
    for (size_t partial = 1; partial < dn; partial++) {
        CHECK(rp_h2_parse_frame(data, partial, &f, &consumed) == 0,
              "a %zu-byte prefix of a %zu-byte frame must not parse", partial, dn);
    }
    CHECK(rp_h2_parse_frame(data, dn, &f, &consumed) == 1, "complete frame parses");
    CHECK(f.type == RP_H2_DATA && f.stream == 1 && f.length == sizeof payload, "DATA on stream 1");
    printf("  partial frames: incomplete reads are held, not misread — OK\n");

    uint8_t hb[32];
    size_t hn = rp_h2_write_headers(0x80000003u, hb, sizeof hb);
    rp_h2_parse_frame(hb, hn, &f, &consumed);
    CHECK(f.stream == 3, "reserved bit masked, got stream %u", f.stream);
    printf("  stream ids: reserved bit handled — OK\n");

    if (failures) { printf("%d check(s) FAILED\n", failures); return 1; }
    printf("all checks passed\n");
    return 0;
}
