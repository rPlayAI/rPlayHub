/*
 * rp_xpc — Apple's XPC object codec and the RemoteXPC wrapper, in portable C.
 *
 * This is the gateway to everything above the tunnel: RSD's handshake and every coredevice.*
 * service invocation are XPC dictionaries inside a RemoteXPC wrapper, carried over HTTP/2.
 *
 * Wire format (all little-endian), verified against a real device by the Python implementation
 * this is ported from (host/rplayhub/wire/xpc.py):
 *
 *   wrapper : magic 0x29B00B92 | flags u32 | length u64 | message_id u64 | [payload]
 *   payload : magic 0x42133742 | version 5 | object
 *   object  : type u32 | data(type)
 *
 * Strings and data are padded to a 4-byte boundary; dictionary keys are NUL-terminated and
 * padded, with no length prefix. Getting that padding wrong is the classic way to produce a
 * dictionary the device silently ignores.
 *
 * The writer is append-only into a caller-supplied buffer, so there is no allocation and no
 * ownership question. Nested dictionaries are written by beginning one, adding entries, then
 * ending it — the length is back-patched.
 */
#ifndef RP_XPC_H
#define RP_XPC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RP_XPC_WRAPPER_MAGIC  0x29B00B92u
#define RP_XPC_PAYLOAD_MAGIC  0x42133742u
#define RP_XPC_PAYLOAD_VER    5u

/* wrapper flags */
#define RP_XPC_F_ALWAYS_SET     0x00000001u
#define RP_XPC_F_DATA_PRESENT   0x00000100u
#define RP_XPC_F_WANTING_REPLY  0x00010000u
#define RP_XPC_F_INIT_HANDSHAKE 0x00400000u

/* object types */
typedef enum {
    RP_XPC_NULL   = 0x1000,
    RP_XPC_BOOL   = 0x2000,
    RP_XPC_INT64  = 0x3000,
    RP_XPC_UINT64 = 0x4000,
    RP_XPC_DOUBLE = 0x5000,
    RP_XPC_DATE   = 0x7000,
    RP_XPC_DATA   = 0x8000,
    RP_XPC_STRING = 0x9000,
    RP_XPC_UUID   = 0xA000,
    RP_XPC_ARRAY  = 0xE000,
    RP_XPC_DICT   = 0xF000
} rp_xpc_type;

/* ------------------------------------------------------------------ writer */

#define RP_XPC_MAX_NESTING 8

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    bool     overflow;                       /* set once the buffer was too small */
    size_t   stack[RP_XPC_MAX_NESTING];      /* offsets of open container headers */
    size_t   counts[RP_XPC_MAX_NESTING];     /* entries written into each */
    int      depth;
} rp_xpc_writer;

void rp_xpc_writer_init(rp_xpc_writer *w, uint8_t *buf, size_t cap);

/* Containers. Every begin must be matched by an end. */
void rp_xpc_dict_begin(rp_xpc_writer *w);
void rp_xpc_dict_end(rp_xpc_writer *w);
void rp_xpc_array_begin(rp_xpc_writer *w);
void rp_xpc_array_end(rp_xpc_writer *w);

/* Dictionary entries: the key, then a value. */
void rp_xpc_key(rp_xpc_writer *w, const char *key);
void rp_xpc_string(rp_xpc_writer *w, const char *value);
void rp_xpc_uint64(rp_xpc_writer *w, uint64_t value);
void rp_xpc_int64(rp_xpc_writer *w, int64_t value);
void rp_xpc_bool(rp_xpc_writer *w, bool value);
void rp_xpc_data(rp_xpc_writer *w, const void *bytes, size_t n);
void rp_xpc_uuid(rp_xpc_writer *w, const uint8_t uuid[16]);
void rp_xpc_null(rp_xpc_writer *w);

/* Splice an already-encoded object in as one value.
 *
 * Needed wherever a generic layer has to carry a payload whose shape it does not know: the
 * CoreDevice envelope wraps each feature's arguments without understanding them, and re-encoding
 * would mean that envelope having to know every feature. The bytes must be exactly one complete
 * XPC object -- the writer counts it as a single entry and does not validate it. */
void rp_xpc_raw(rp_xpc_writer *w, const uint8_t *object, size_t n);

/* Convenience: key + value in one call. */
void rp_xpc_set_string(rp_xpc_writer *w, const char *key, const char *value);
void rp_xpc_set_uint64(rp_xpc_writer *w, const char *key, uint64_t value);
void rp_xpc_set_int64(rp_xpc_writer *w, const char *key, int64_t value);
void rp_xpc_set_bool(rp_xpc_writer *w, const char *key, bool value);
void rp_xpc_set_uuid(rp_xpc_writer *w, const char *key, const uint8_t uuid[16]);

/* Wrap whatever was written into a RemoteXPC message. Returns the total size, or 0 on overflow.
 * `body` is the object bytes produced by the writer; `out` receives the framed message. */
size_t rp_xpc_wrap(const uint8_t *body, size_t body_len, uint32_t flags, uint64_t message_id,
                   uint8_t *out, size_t out_cap);

/* A wrapper with no payload at all — used for the HTTP/2 init handshake. */
size_t rp_xpc_wrap_empty(uint32_t flags, uint64_t message_id, uint8_t *out, size_t out_cap);

/* ------------------------------------------------------------------ reader */

typedef struct {
    const uint8_t *data;      /* start of the object */
    size_t         size;      /* bytes available */
} rp_xpc_obj;

/* Parse a wrapper. Returns 0 on success. `obj` is left pointing at the payload object, or with
 * size 0 when the message carried none (the empty-dict acks the device sends before a reply). */
int rp_xpc_unwrap(const uint8_t *msg, size_t len, uint32_t *flags, uint64_t *message_id,
                  rp_xpc_obj *obj);

/* Look up a key in a dictionary object. Returns 0 and fills `value` when found. */
int rp_xpc_dict_get(const rp_xpc_obj *dict, const char *key, rp_xpc_obj *value);

/* Walk a dictionary's entries in wire order.
 *
 * Start with *cursor = 0 and call until it returns non-zero. `key` points into the message and
 * stays valid as long as it does. Needed because the interesting dictionaries -- the RSD service
 * map above all -- are answers whose keys we do not know in advance; looking those up one at a
 * time is impossible when the whole point is to discover what the device offers. */
int rp_xpc_dict_next(const rp_xpc_obj *dict, size_t *cursor,
                     const char **key, rp_xpc_obj *value);

/* Number of entries in a dictionary, or -1 if it is not one. */
int rp_xpc_dict_count(const rp_xpc_obj *dict);

rp_xpc_type rp_xpc_obj_type(const rp_xpc_obj *obj);
int rp_xpc_get_uint64(const rp_xpc_obj *obj, uint64_t *out);
int rp_xpc_get_string(const rp_xpc_obj *obj, const char **out);  /* NUL-terminated in place */
int rp_xpc_get_data(const rp_xpc_obj *obj, const uint8_t **out, size_t *n);

/* True when the object is an empty dictionary — the ack the device sends before the real reply,
 * which every receive loop must skip or it will mistake it for the answer. */
bool rp_xpc_is_empty_dict(const rp_xpc_obj *obj);

#ifdef __cplusplus
}
#endif

#endif /* RP_XPC_H */
