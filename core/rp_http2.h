/*
 * rp_http2 — the sliver of HTTP/2 that RemoteXPC needs, in portable C.
 *
 * Every coredevice.* service speaks XPC over HTTP/2. But almost none of HTTP/2 is involved: the
 * connection sends a preface, one SETTINGS, one WINDOW_UPDATE, two empty HEADERS frames to open
 * streams 1 and 3, and then DATA frames carrying XPC objects. There is no HPACK, because the
 * headers are empty; no flow control beyond opening a large window; no priority, no push.
 *
 * So this is deliberately not an HTTP/2 library. Bending a real one — or the AccessorySDK's
 * HTTP/1.1 client, which cannot do HTTP/2 at all — would be more work than the ~200 lines here.
 *
 * There is no socket code in this file on purpose. Everything is "write these bytes into a
 * buffer" or "here are bytes I received": that keeps it free of winsock-versus-BSD differences,
 * makes it testable with no device and no network, and leaves the platform layer to the daemon.
 */
#ifndef RP_HTTP2_H
#define RP_HTTP2_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The connection preface every HTTP/2 client sends first. */
#define RP_H2_PREFACE "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
#define RP_H2_PREFACE_LEN 24

#define RP_H2_FRAME_HEADER_LEN 9

/* Frame types we use. */
#define RP_H2_DATA          0x0
#define RP_H2_HEADERS       0x1
#define RP_H2_RST_STREAM    0x3
#define RP_H2_SETTINGS      0x4
#define RP_H2_GOAWAY        0x7
#define RP_H2_WINDOW_UPDATE 0x8

/* Flags. */
#define RP_H2_FLAG_ACK          0x1
#define RP_H2_FLAG_END_STREAM   0x1
#define RP_H2_FLAG_END_HEADERS  0x4

/* Settings identifiers we set. */
#define RP_H2_SETTINGS_MAX_CONCURRENT_STREAMS 0x3
#define RP_H2_SETTINGS_INITIAL_WINDOW_SIZE    0x4

/* The two streams RemoteXPC uses: 1 carries requests, 3 carries replies. */
#define RP_H2_STREAM_ROOT  1
#define RP_H2_STREAM_REPLY 3

/* A parsed frame. `payload` points into the caller's buffer — copy it if it must outlive. */
typedef struct {
    uint8_t        type;
    uint8_t        flags;
    uint32_t       stream;
    const uint8_t *payload;
    size_t         length;
} rp_h2_frame;

/* --- writing. Each returns bytes written, or 0 if the buffer was too small. --- */

size_t rp_h2_write_preface(uint8_t *out, size_t cap);

size_t rp_h2_write_frame(uint8_t type, uint8_t flags, uint32_t stream,
                         const uint8_t *payload, size_t len, uint8_t *out, size_t cap);

/* SETTINGS with max-concurrent-streams and initial-window-size. */
size_t rp_h2_write_settings(uint32_t max_streams, uint32_t window, uint8_t *out, size_t cap);
size_t rp_h2_write_settings_ack(uint8_t *out, size_t cap);
size_t rp_h2_write_window_update(uint32_t stream, uint32_t increment, uint8_t *out, size_t cap);
/* An empty HEADERS frame, which is how RemoteXPC opens a stream. */
size_t rp_h2_write_headers(uint32_t stream, uint8_t *out, size_t cap);
size_t rp_h2_write_data(uint32_t stream, const uint8_t *payload, size_t len,
                        uint8_t *out, size_t cap);

/* Everything a client sends before its first XPC message: preface, SETTINGS, WINDOW_UPDATE,
 * HEADERS for stream 1, then HEADERS for stream 3. Returns the total, or 0 if too small. */
size_t rp_h2_write_connection_start(uint32_t window, uint8_t *out, size_t cap);

/* --- reading --- */

/* Parse one frame from the front of `buf`. Returns:
 *   1  a frame was parsed; *consumed is how many bytes it used
 *   0  more bytes are needed
 *  -1  malformed
 */
int rp_h2_parse_frame(const uint8_t *buf, size_t len, rp_h2_frame *out, size_t *consumed);

const char *rp_h2_frame_type_name(uint8_t type);

#ifdef __cplusplus
}
#endif

#endif /* RP_HTTP2_H */
