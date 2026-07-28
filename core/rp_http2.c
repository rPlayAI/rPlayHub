#include "rp_http2.h"

#include <string.h>

/* Frame header: length(24) | type(8) | flags(8) | R(1)+stream id(31), all big-endian. */

static size_t put_header(uint8_t *out, size_t cap, size_t len, uint8_t type, uint8_t flags,
                         uint32_t stream)
{
    if (cap < RP_H2_FRAME_HEADER_LEN) return 0;
    out[0] = (uint8_t)(len >> 16);
    out[1] = (uint8_t)(len >> 8);
    out[2] = (uint8_t)len;
    out[3] = type;
    out[4] = flags;
    stream &= 0x7FFFFFFFu;                 /* the top bit is reserved and must be sent as 0 */
    out[5] = (uint8_t)(stream >> 24);
    out[6] = (uint8_t)(stream >> 16);
    out[7] = (uint8_t)(stream >> 8);
    out[8] = (uint8_t)stream;
    return RP_H2_FRAME_HEADER_LEN;
}

size_t rp_h2_write_preface(uint8_t *out, size_t cap)
{
    if (cap < RP_H2_PREFACE_LEN) return 0;
    memcpy(out, RP_H2_PREFACE, RP_H2_PREFACE_LEN);
    return RP_H2_PREFACE_LEN;
}

size_t rp_h2_write_frame(uint8_t type, uint8_t flags, uint32_t stream,
                         const uint8_t *payload, size_t len, uint8_t *out, size_t cap)
{
    if (len > 0xFFFFFF) return 0;
    if (cap < RP_H2_FRAME_HEADER_LEN + len) return 0;
    put_header(out, cap, len, type, flags, stream);
    if (len && payload) memcpy(out + RP_H2_FRAME_HEADER_LEN, payload, len);
    return RP_H2_FRAME_HEADER_LEN + len;
}

size_t rp_h2_write_settings(uint32_t max_streams, uint32_t window, uint8_t *out, size_t cap)
{
    /* Each setting is a 16-bit identifier followed by a 32-bit value. */
    uint8_t body[12];
    uint16_t ids[2] = { RP_H2_SETTINGS_MAX_CONCURRENT_STREAMS, RP_H2_SETTINGS_INITIAL_WINDOW_SIZE };
    uint32_t vals[2] = { max_streams, window };
    for (int i = 0; i < 2; i++) {
        body[i * 6 + 0] = (uint8_t)(ids[i] >> 8);
        body[i * 6 + 1] = (uint8_t)ids[i];
        body[i * 6 + 2] = (uint8_t)(vals[i] >> 24);
        body[i * 6 + 3] = (uint8_t)(vals[i] >> 16);
        body[i * 6 + 4] = (uint8_t)(vals[i] >> 8);
        body[i * 6 + 5] = (uint8_t)vals[i];
    }
    return rp_h2_write_frame(RP_H2_SETTINGS, 0, 0, body, sizeof body, out, cap);
}

size_t rp_h2_write_settings_ack(uint8_t *out, size_t cap)
{
    return rp_h2_write_frame(RP_H2_SETTINGS, RP_H2_FLAG_ACK, 0, NULL, 0, out, cap);
}

size_t rp_h2_write_window_update(uint32_t stream, uint32_t increment, uint8_t *out, size_t cap)
{
    uint8_t body[4] = {
        (uint8_t)(increment >> 24), (uint8_t)(increment >> 16),
        (uint8_t)(increment >> 8), (uint8_t)increment
    };
    return rp_h2_write_frame(RP_H2_WINDOW_UPDATE, 0, stream, body, sizeof body, out, cap);
}

size_t rp_h2_write_headers(uint32_t stream, uint8_t *out, size_t cap)
{
    /* Empty HEADERS. RemoteXPC uses these purely to open a stream, so there is no header block
     * and therefore no HPACK involved anywhere in this implementation. */
    return rp_h2_write_frame(RP_H2_HEADERS, RP_H2_FLAG_END_HEADERS, stream, NULL, 0, out, cap);
}

size_t rp_h2_write_data(uint32_t stream, const uint8_t *payload, size_t len,
                        uint8_t *out, size_t cap)
{
    return rp_h2_write_frame(RP_H2_DATA, 0, stream, payload, len, out, cap);
}

size_t rp_h2_write_connection_start(uint32_t window, uint8_t *out, size_t cap)
{
    size_t off = 0, n;
    /* The order matters: the preface must come first, and the streams must be opened before any
     * DATA is sent on them. */
    if (!(n = rp_h2_write_preface(out + off, cap - off))) return 0; off += n;
    if (!(n = rp_h2_write_settings(100, window, out + off, cap - off))) return 0; off += n;
    if (window > 65535) {
        /* The connection window starts at 65535; raise it by the difference. */
        if (!(n = rp_h2_write_window_update(0, window - 65535, out + off, cap - off))) return 0;
        off += n;
    }
    if (!(n = rp_h2_write_headers(RP_H2_STREAM_ROOT, out + off, cap - off))) return 0; off += n;
    if (!(n = rp_h2_write_headers(RP_H2_STREAM_REPLY, out + off, cap - off))) return 0; off += n;
    return off;
}

int rp_h2_parse_frame(const uint8_t *buf, size_t len, rp_h2_frame *out, size_t *consumed)
{
    if (!buf || len < RP_H2_FRAME_HEADER_LEN) return 0;
    size_t plen = ((size_t)buf[0] << 16) | ((size_t)buf[1] << 8) | (size_t)buf[2];
    if (len < RP_H2_FRAME_HEADER_LEN + plen) return 0;      /* frame not fully arrived */
    if (out) {
        out->type = buf[3];
        out->flags = buf[4];
        out->stream = ((uint32_t)buf[5] << 24 | (uint32_t)buf[6] << 16 |
                       (uint32_t)buf[7] << 8 | (uint32_t)buf[8]) & 0x7FFFFFFFu;
        out->payload = plen ? buf + RP_H2_FRAME_HEADER_LEN : NULL;
        out->length = plen;
    }
    if (consumed) *consumed = RP_H2_FRAME_HEADER_LEN + plen;
    return 1;
}

const char *rp_h2_frame_type_name(uint8_t type)
{
    switch (type) {
    case RP_H2_DATA:          return "DATA";
    case RP_H2_HEADERS:       return "HEADERS";
    case RP_H2_RST_STREAM:    return "RST_STREAM";
    case RP_H2_SETTINGS:      return "SETTINGS";
    case RP_H2_GOAWAY:        return "GOAWAY";
    case RP_H2_WINDOW_UPDATE: return "WINDOW_UPDATE";
    default:                  return "?";
    }
}
