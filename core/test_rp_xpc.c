/* Emits the RSD handshake message as hex, so it can be diffed against the Python implementation
 * that is already verified against a real device. Byte-for-byte equality is the only test that
 * matters here: the device silently ignores a dictionary whose padding or lengths are wrong. */
#include "rp_xpc.h"
#include <stdio.h>
#include <string.h>

int main(void)
{
    static const uint8_t uuid[16] = {
        0x12,0x34,0x56,0x78,0x12,0x34,0x56,0x78,0x12,0x34,0x56,0x78,0x12,0x34,0x56,0x78
    };
    uint8_t body[4096];
    rp_xpc_writer w;
    rp_xpc_writer_init(&w, body, sizeof body);

    rp_xpc_dict_begin(&w);
      rp_xpc_set_string(&w, "MessageType", "Handshake");
      rp_xpc_set_uint64(&w, "MessagingProtocolVersion", 7);
      rp_xpc_set_uuid(&w, "UUID", uuid);
      rp_xpc_key(&w, "Properties");
      rp_xpc_dict_begin(&w);
        rp_xpc_set_uint64(&w, "RemoteXPCVersionFlags", 0x0100000000000006ULL);
        rp_xpc_set_bool(&w, "SensitivePropertiesVisible", true);
      rp_xpc_dict_end(&w);
      rp_xpc_key(&w, "Services");
      rp_xpc_dict_begin(&w);
      rp_xpc_dict_end(&w);
    rp_xpc_dict_end(&w);

    if (w.overflow) { fprintf(stderr, "overflow\n"); return 1; }

    uint8_t msg[4096];
    size_t n = rp_xpc_wrap(body, w.len, RP_XPC_F_ALWAYS_SET | RP_XPC_F_DATA_PRESENT, 1,
                           msg, sizeof msg);
    if (!n) { fprintf(stderr, "wrap failed\n"); return 1; }
    for (size_t i = 0; i < n; i++) printf("%02x", msg[i]);
    printf("\n");

    /* Read it back: the reader has to walk key padding and nested containers correctly, or it
     * will find the wrong entries — which on a real device looks like a service that answers
     * nothing. */
    int bad = 0;
    uint32_t flags = 0; uint64_t mid = 0; rp_xpc_obj root;
    if (rp_xpc_unwrap(msg, n, &flags, &mid, &root) != 0) { fprintf(stderr, "unwrap failed\n"); bad++; }
    if (mid != 1) { fprintf(stderr, "message id %llu\n", (unsigned long long)mid); bad++; }

    rp_xpc_obj v;
    const char *str = NULL;
    if (rp_xpc_dict_get(&root, "MessageType", &v) || rp_xpc_get_string(&v, &str)
        || strcmp(str, "Handshake")) { fprintf(stderr, "MessageType\n"); bad++; }

    uint64_t u = 0;
    if (rp_xpc_dict_get(&root, "MessagingProtocolVersion", &v) || rp_xpc_get_uint64(&v, &u)
        || u != 7) { fprintf(stderr, "MessagingProtocolVersion\n"); bad++; }

    /* a value AFTER a nested dictionary — proves container sizes are walked correctly */
    rp_xpc_obj props, services;
    if (rp_xpc_dict_get(&root, "Properties", &props)) { fprintf(stderr, "Properties\n"); bad++; }
    if (rp_xpc_dict_get(&props, "RemoteXPCVersionFlags", &v) || rp_xpc_get_uint64(&v, &u)
        || u != 0x0100000000000006ULL) { fprintf(stderr, "nested lookup\n"); bad++; }
    if (rp_xpc_dict_get(&root, "Services", &services)) { fprintf(stderr, "Services\n"); bad++; }
    if (!rp_xpc_is_empty_dict(&services)) { fprintf(stderr, "empty dict detection\n"); bad++; }
    if (rp_xpc_dict_get(&root, "NoSuchKey", &v) == 0) { fprintf(stderr, "missing key\n"); bad++; }

    /* the empty-dict ack the device sends before every real reply must be recognisable */
    uint8_t ack[64]; rp_xpc_writer aw;
    rp_xpc_writer_init(&aw, ack, sizeof ack);
    rp_xpc_dict_begin(&aw); rp_xpc_dict_end(&aw);
    uint8_t ackmsg[128];
    size_t an = rp_xpc_wrap(ack, aw.len, RP_XPC_F_ALWAYS_SET, 2, ackmsg, sizeof ackmsg);
    rp_xpc_obj ackobj;
    if (rp_xpc_unwrap(ackmsg, an, NULL, NULL, &ackobj) || !rp_xpc_is_empty_dict(&ackobj)) {
        fprintf(stderr, "ack detection\n"); bad++;
    }

    fprintf(stderr, bad ? "  reader: %d FAILURE(S)\n" : "  reader: parses back correctly%.0d\n", bad);
    return bad ? 1 : 0;
}
