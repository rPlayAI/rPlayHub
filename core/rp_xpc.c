#include "rp_xpc.h"

#include <string.h>

/* ------------------------------------------------------------------ writer */

static void put(rp_xpc_writer *w, const void *bytes, size_t n)
{
    if (w->len + n > w->cap) { w->overflow = true; return; }
    memcpy(w->buf + w->len, bytes, n);
    w->len += n;
}

static void put_u32(rp_xpc_writer *w, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    put(w, b, 4);
}

static void put_u64(rp_xpc_writer *w, uint64_t v)
{
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
    put(w, b, 8);
}

static void pad4(rp_xpc_writer *w)
{
    static const uint8_t zeros[4] = { 0, 0, 0, 0 };
    size_t r = w->len % 4;
    if (r) put(w, zeros, 4 - r);
}

static void bump(rp_xpc_writer *w)
{
    if (w->depth > 0) w->counts[w->depth - 1]++;
}

void rp_xpc_writer_init(rp_xpc_writer *w, uint8_t *buf, size_t cap)
{
    memset(w, 0, sizeof *w);
    w->buf = buf;
    w->cap = cap;
}

static void container_begin(rp_xpc_writer *w, rp_xpc_type type)
{
    bump(w);
    if (w->depth >= RP_XPC_MAX_NESTING) { w->overflow = true; return; }
    put_u32(w, (uint32_t)type);
    w->stack[w->depth] = w->len;      /* where the inner length goes */
    w->counts[w->depth] = 0;
    w->depth++;
    put_u32(w, 0);                    /* inner length, back-patched */
    put_u32(w, 0);                    /* entry count, back-patched */
}

static void container_end(rp_xpc_writer *w)
{
    if (w->depth <= 0) { w->overflow = true; return; }
    w->depth--;
    size_t at = w->stack[w->depth];
    if (at + 8 > w->cap) { w->overflow = true; return; }
    /* Inner length covers the count field and every entry, but not the length field itself. */
    uint32_t inner = (uint32_t)(w->len - at - 4);
    uint32_t count = (uint32_t)w->counts[w->depth];
    for (int i = 0; i < 4; i++) {
        w->buf[at + i]     = (uint8_t)(inner >> (8 * i));
        w->buf[at + 4 + i] = (uint8_t)(count >> (8 * i));
    }
}

void rp_xpc_dict_begin(rp_xpc_writer *w)  { container_begin(w, RP_XPC_DICT); }
void rp_xpc_dict_end(rp_xpc_writer *w)    { container_end(w); }
void rp_xpc_array_begin(rp_xpc_writer *w) { container_begin(w, RP_XPC_ARRAY); }
void rp_xpc_array_end(rp_xpc_writer *w)   { container_end(w); }

void rp_xpc_key(rp_xpc_writer *w, const char *key)
{
    /* Keys are NUL-terminated and 4-byte aligned, with NO length prefix. */
    put(w, key, strlen(key) + 1);
    pad4(w);
}

void rp_xpc_string(rp_xpc_writer *w, const char *value)
{
    size_t n = strlen(value) + 1;             /* the NUL is counted in the length */
    bump(w);
    put_u32(w, RP_XPC_STRING);
    put_u32(w, (uint32_t)n);
    put(w, value, n);
    pad4(w);
}

void rp_xpc_raw(rp_xpc_writer *w, const uint8_t *object, size_t n)
{
    bump(w);
    put(w, object, n);
    /* No pad4: a well-formed object is already aligned, and padding one that is not would only
     * hide the corruption further downstream. */
}

void rp_xpc_uint64(rp_xpc_writer *w, uint64_t v)
{
    bump(w); put_u32(w, RP_XPC_UINT64); put_u64(w, v);
}

void rp_xpc_int64(rp_xpc_writer *w, int64_t v)
{
    bump(w); put_u32(w, RP_XPC_INT64); put_u64(w, (uint64_t)v);
}

void rp_xpc_bool(rp_xpc_writer *w, bool v)
{
    bump(w); put_u32(w, RP_XPC_BOOL); put_u32(w, v ? 1u : 0u);
}

/* Doubles ride as their raw IEEE-754 bits, like every other 8-byte scalar here. Needed for
 * coredevice.configuration's liquidGlass opacity, which is a fraction 0..1 -- writing it as an
 * integer would collapse every value to 0 or 1. */
void rp_xpc_double(rp_xpc_writer *w, double v)
{
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    bump(w); put_u32(w, RP_XPC_DOUBLE); put_u64(w, bits);
}

void rp_xpc_data(rp_xpc_writer *w, const void *bytes, size_t n)
{
    bump(w);
    put_u32(w, RP_XPC_DATA);
    put_u32(w, (uint32_t)n);
    put(w, bytes, n);
    pad4(w);
}

void rp_xpc_uuid(rp_xpc_writer *w, const uint8_t uuid[16])
{
    bump(w); put_u32(w, RP_XPC_UUID); put(w, uuid, 16);
}

void rp_xpc_null(rp_xpc_writer *w)
{
    bump(w); put_u32(w, RP_XPC_NULL);
}

void rp_xpc_set_string(rp_xpc_writer *w, const char *k, const char *v)
{ rp_xpc_key(w, k); rp_xpc_string(w, v); }
void rp_xpc_set_uint64(rp_xpc_writer *w, const char *k, uint64_t v)
{ rp_xpc_key(w, k); rp_xpc_uint64(w, v); }
void rp_xpc_set_int64(rp_xpc_writer *w, const char *k, int64_t v)
{ rp_xpc_key(w, k); rp_xpc_int64(w, v); }
void rp_xpc_set_bool(rp_xpc_writer *w, const char *k, bool v)
{ rp_xpc_key(w, k); rp_xpc_bool(w, v); }
void rp_xpc_set_uuid(rp_xpc_writer *w, const char *k, const uint8_t u[16])
{ rp_xpc_key(w, k); rp_xpc_uuid(w, u); }
void rp_xpc_set_double(rp_xpc_writer *w, const char *k, double v)
{ rp_xpc_key(w, k); rp_xpc_double(w, v); }

/* ---------------------------------------------------------------- wrapping */

static void w32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8*i)); }
static void w64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8*i)); }
static uint32_t r32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t r64(const uint8_t *p)
{ uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }

size_t rp_xpc_wrap(const uint8_t *body, size_t body_len, uint32_t flags, uint64_t message_id,
                   uint8_t *out, size_t out_cap)
{
    size_t total = 16 + 8 + 8 + body_len;      /* header + msgid + payload magic/ver + object */
    if (!out || out_cap < total) return 0;
    w32(out + 0, RP_XPC_WRAPPER_MAGIC);
    w32(out + 4, flags);
    /* The length field counts everything after the message id. */
    w64(out + 8, (uint64_t)(8 + body_len));
    w64(out + 16, message_id);
    w32(out + 24, RP_XPC_PAYLOAD_MAGIC);
    w32(out + 28, RP_XPC_PAYLOAD_VER);
    memcpy(out + 32, body, body_len);
    return total;
}

size_t rp_xpc_wrap_empty(uint32_t flags, uint64_t message_id, uint8_t *out, size_t out_cap)
{
    if (!out || out_cap < 24) return 0;
    w32(out + 0, RP_XPC_WRAPPER_MAGIC);
    w32(out + 4, flags);
    w64(out + 8, 0);
    w64(out + 16, message_id);
    return 24;
}

/* ------------------------------------------------------------------ reader */

int rp_xpc_unwrap(const uint8_t *msg, size_t len, uint32_t *flags, uint64_t *message_id,
                  rp_xpc_obj *obj)
{
    if (!msg || len < 24) return -1;
    if (r32(msg) != RP_XPC_WRAPPER_MAGIC) return -1;
    uint64_t plen = r64(msg + 8);
    if (flags) *flags = r32(msg + 4);
    if (message_id) *message_id = r64(msg + 16);
    if (obj) { obj->data = NULL; obj->size = 0; }
    if (plen == 0) return 0;                       /* control frame, no payload */

    /* The whole declared payload must be present.
     *
     * Without this check a message that has only partly arrived parses as a complete but
     * TRUNCATED object: the header is intact, the first entries decode, and the rest is simply
     * missing. The caller then looks for a key that has not arrived yet, does not find it, and
     * throws the buffered bytes away -- so a reply spanning several DATA frames can never be
     * assembled, and the failure looks like the device going silent rather than like a parse
     * error. That is exactly how RSD service discovery failed: the map is tens of kilobytes and
     * always spans multiple frames.
     *
     * The wrapper is magic(4) + flags(4) + length(8) + message_id(8) + payload, and the length
     * field counts the payload only. */
    if (len < 24 + plen) return -1;
    if (r32(msg + 24) != RP_XPC_PAYLOAD_MAGIC) return -1;
    if (obj) {
        obj->data = msg + 32;
        /* Sized from the declaration, not from what happens to be in the buffer: anything after
         * this message belongs to the next one. */
        obj->size = (size_t)plen - 8;
    }
    return 0;
}

rp_xpc_type rp_xpc_obj_type(const rp_xpc_obj *obj)
{
    if (!obj || !obj->data || obj->size < 4) return RP_XPC_NULL;
    return (rp_xpc_type)r32(obj->data);
}

/* Size of one object, so entries can be walked. Returns 0 if malformed. */
static size_t obj_size(const uint8_t *p, size_t avail)
{
    if (avail < 4) return 0;
    uint32_t type = r32(p);
    switch (type) {
    case RP_XPC_NULL:   return 4;
    case RP_XPC_BOOL:   return avail >= 8 ? 8 : 0;
    case RP_XPC_INT64:
    case RP_XPC_UINT64:
    case RP_XPC_DOUBLE:
    case RP_XPC_DATE:   return avail >= 12 ? 12 : 0;
    case RP_XPC_UUID:   return avail >= 20 ? 20 : 0;
    case RP_XPC_STRING:
    case RP_XPC_DATA: {
        if (avail < 8) return 0;
        uint32_t n = r32(p + 4);
        size_t padded = (n + 3u) & ~3u;
        return (8 + padded <= avail) ? 8 + padded : 0;
    }
    case RP_XPC_ARRAY:
    case RP_XPC_DICT: {
        if (avail < 8) return 0;
        uint32_t inner = r32(p + 4);
        return (8 + (size_t)inner - 4 <= avail) ? 4 + 4 + inner : 0;
    }
    default: return 0;
    }
}

int rp_xpc_dict_get(const rp_xpc_obj *dict, const char *key, rp_xpc_obj *value)
{
    if (!dict || !dict->data || dict->size < 12) return -1;
    if (r32(dict->data) != RP_XPC_DICT) return -1;
    uint32_t count = r32(dict->data + 8);
    const uint8_t *p = dict->data + 12;
    const uint8_t *end = dict->data + dict->size;

    for (uint32_t i = 0; i < count && p < end; i++) {
        const char *k = (const char *)p;
        size_t klen = strnlen(k, (size_t)(end - p));
        if (klen == (size_t)(end - p)) return -1;          /* unterminated */
        p += (klen + 1 + 3u) & ~3u;
        if (p >= end) return -1;
        size_t vsize = obj_size(p, (size_t)(end - p));
        if (!vsize) return -1;
        if (strcmp(k, key) == 0) {
            if (value) { value->data = p; value->size = vsize; }
            return 0;
        }
        p += vsize;
    }
    return -1;
}

int rp_xpc_dict_count(const rp_xpc_obj *dict)
{
    if (!dict || !dict->data || dict->size < 12) return -1;
    if (r32(dict->data) != RP_XPC_DICT) return -1;
    return (int)r32(dict->data + 8);
}

int rp_xpc_dict_next(const rp_xpc_obj *dict, size_t *cursor,
                     const char **key, rp_xpc_obj *value)
{
    if (!dict || !dict->data || !cursor || dict->size < 12) return -1;
    if (r32(dict->data) != RP_XPC_DICT) return -1;
    uint32_t count = r32(dict->data + 8);

    /* The cursor counts entries, and each step re-walks from the start. That is quadratic, but
     * these dictionaries hold tens of entries and are parsed once per session, so the simplicity
     * is worth more than the cycles -- and it keeps the cursor a plain integer the caller cannot
     * corrupt into an out-of-bounds pointer. */
    size_t want = *cursor;
    if (want >= count) return -1;

    const uint8_t *p = dict->data + 12;
    const uint8_t *end = dict->data + dict->size;
    for (uint32_t i = 0; i < count && p < end; i++) {
        const char *k = (const char *)p;
        size_t klen = strnlen(k, (size_t)(end - p));
        if (klen == (size_t)(end - p)) return -1;          /* unterminated */
        p += (klen + 1 + 3u) & ~3u;
        if (p >= end) return -1;
        size_t vsize = obj_size(p, (size_t)(end - p));
        if (!vsize) return -1;
        if (i == want) {
            if (key) *key = k;
            if (value) { value->data = p; value->size = vsize; }
            *cursor = want + 1;
            return 0;
        }
        p += vsize;
    }
    return -1;
}

int rp_xpc_array_count(const rp_xpc_obj *arr)
{
    if (!arr || !arr->data || arr->size < 12) return -1;
    if (r32(arr->data) != RP_XPC_ARRAY) return -1;
    return (int)r32(arr->data + 8);
}

int rp_xpc_array_next(const rp_xpc_obj *arr, size_t *cursor, rp_xpc_obj *value)
{
    if (!arr || !arr->data || !cursor || arr->size < 12) return -1;
    if (r32(arr->data) != RP_XPC_ARRAY) return -1;
    uint32_t count = r32(arr->data + 8);
    size_t want = *cursor;
    if (want >= count) return -1;

    /* Same re-walk-from-the-start shape as rp_xpc_dict_next, for the same reasons. */
    const uint8_t *p = arr->data + 12;
    const uint8_t *end = arr->data + arr->size;
    for (uint32_t i = 0; i < count && p < end; i++) {
        size_t vsize = obj_size(p, (size_t)(end - p));
        if (!vsize) return -1;
        if (i == want) {
            if (value) { value->data = p; value->size = vsize; }
            *cursor = want + 1;
            return 0;
        }
        p += vsize;
    }
    return -1;
}

int rp_xpc_get_bool(const rp_xpc_obj *obj, bool *out)
{
    if (!obj || obj->size < 8 || r32(obj->data) != RP_XPC_BOOL) return -1;
    if (out) *out = obj->data[4] != 0;
    return 0;
}

int rp_xpc_get_uint64(const rp_xpc_obj *obj, uint64_t *out)
{
    if (!obj || obj->size < 12) return -1;
    uint32_t t = r32(obj->data);
    if (t != RP_XPC_UINT64 && t != RP_XPC_INT64 && t != RP_XPC_DATE) return -1;
    if (out) *out = r64(obj->data + 4);
    return 0;
}

int rp_xpc_get_string(const rp_xpc_obj *obj, const char **out)
{
    if (!obj || obj->size < 8 || r32(obj->data) != RP_XPC_STRING) return -1;
    if (out) *out = (const char *)(obj->data + 8);
    return 0;
}

int rp_xpc_get_data(const rp_xpc_obj *obj, const uint8_t **out, size_t *n)
{
    if (!obj || obj->size < 8 || r32(obj->data) != RP_XPC_DATA) return -1;
    if (n) *n = r32(obj->data + 4);
    if (out) *out = obj->data + 8;
    return 0;
}

bool rp_xpc_is_empty_dict(const rp_xpc_obj *obj)
{
    if (!obj || !obj->data || obj->size < 12) return false;
    return r32(obj->data) == RP_XPC_DICT && r32(obj->data + 8) == 0;
}
