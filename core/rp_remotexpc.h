/*
 * rp_remotexpc.h — a RemoteXPC session: HTTP/2 framing plus the XPC codec, as one conversation.
 *
 * rp_http2 knows how to shape frames and rp_xpc knows how to encode objects, but neither knows
 * the sequence a CoreDevice service expects: a specific opening exchange on two streams, empty
 * acknowledgement messages that must be skipped, and replies that can arrive split across DATA
 * frames. That sequence is what this file owns.
 *
 * Deliberately transport-agnostic. The caller supplies read and write functions, so the same code
 * serves a plain socket through the tunnel today and a TLS or relay transport later without
 * changing anything here. It is also what makes the whole layer testable with no device present:
 * the tests drive it over a memory buffer.
 *
 * Ported from host/rplayhub/wire/remotexpc.py, which is the version proven against real phones.
 * Where the two disagree, the Python is right and this is the bug.
 */
#ifndef RP_REMOTEXPC_H
#define RP_REMOTEXPC_H

#include <stddef.h>
#include <stdint.h>

#include "rp_xpc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The two HTTP/2 streams a RemoteXPC session uses. Fixed by the protocol, not chosen by us. */
#define RP_RXPC_ROOT_STREAM   1u
#define RP_RXPC_REPLY_STREAM  3u

#define RP_RXPC_INITIAL_WINDOW 1048576u

/* Transport. Return the number of bytes moved, 0 on clean close, negative on error. */
typedef struct {
    long (*read)(void *ctx, void *buf, size_t len);
    long (*write)(void *ctx, const void *buf, size_t len);
    void *ctx;
} rp_rxpc_io;

typedef struct {
    rp_rxpc_io io;
    uint64_t   next_message_id;

    /* Reassembly, PER STREAM. A reply larger than one DATA frame arrives in pieces -- the RSD
     * service map is tens of kilobytes, so that is the normal case -- and the device interleaves
     * frames on the root and reply streams. Accumulating both into one buffer splices unrelated
     * messages together and nothing parses. The buffer is split in half, one side per stream. */
    uint8_t   *buf;
    size_t     buf_cap;
    size_t     buf_len[2];

    /* Bytes read from the transport but not yet consumed as whole frames. */
    uint8_t   *raw;
    size_t     raw_cap;
    size_t     raw_len;
    /* Bytes of the frame handed out last call, consumed at the start of the next one so the
     * payload pointer stays valid in between. */
    size_t     pending_consume;
} rp_rxpc_session;

/* Bind a session to a transport. The two buffers are caller-owned and must outlive the session;
 * no allocation happens in here, so this is usable on a thread with a fixed stack or in a daemon
 * that refuses to malloc on the data path. */
void rp_rxpc_init(rp_rxpc_session *s, rp_rxpc_io io,
                  uint8_t *reassembly, size_t reassembly_cap,
                  uint8_t *raw, size_t raw_cap);

/* Perform the opening exchange. Returns 0 on success. */
int rp_rxpc_handshake(rp_rxpc_session *s);

/* Send an XPC object (already encoded by rp_xpc_writer) as a request on the root stream. */
int rp_rxpc_send(rp_rxpc_session *s, const uint8_t *body, size_t body_len, int wanting_reply);

/* Read until a message with an actual payload arrives.
 *
 * Empty-dictionary messages are acknowledgements the device sends before the real answer; every
 * receive loop must skip them or it will mistake one for the reply. Returns 0 on success and
 * leaves `obj` pointing into the session's reassembly buffer, valid until the next call. */
int rp_rxpc_recv(rp_rxpc_session *s, rp_xpc_obj *obj);

#ifdef __cplusplus
}
#endif

#endif /* RP_REMOTEXPC_H */
