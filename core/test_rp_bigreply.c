/*
 * test_rp_bigreply — a reply that spans several DATA frames.
 *
 * The RSD service map is tens of kilobytes and always arrives in pieces, so reassembly is the
 * normal path rather than an edge case. The earlier handshake test only ever exercised a
 * single-frame message, which is why it passed while service discovery failed against a real
 * device: a partly-arrived message parsed as a complete but truncated one, the caller found no
 * Services key, and the buffered bytes were discarded -- presenting as the device going silent.
 */
#include "rp_remotexpc.h"
#include "rp_http2.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static uint8_t stream[1<<20]; static size_t slen, spos;
static long r_(void *c, void *b, size_t n){(void)c;
    size_t left = slen - spos; if(!left) return 0; if(n>left) n=left;
    if (n > 1400) n = 1400;                 /* dribble it in, like a socket would */
    memcpy(b, stream+spos, n); spos+=n; return (long)n; }
static long w_(void *c, const void *b, size_t n){(void)c;(void)b; return (long)n;}
int main(void){
    /* A dictionary with many entries, like the RSD service map. */
    static uint8_t body[1<<19];
    rp_xpc_writer w; rp_xpc_writer_init(&w, body, sizeof body);
    rp_xpc_dict_begin(&w);
    rp_xpc_key(&w,"Services"); rp_xpc_dict_begin(&w);
    char k[64];
    for (int i=0;i<900;i++){ snprintf(k,sizeof k,"com.apple.coredevice.service%03d",i);
        rp_xpc_key(&w,k); rp_xpc_dict_begin(&w); rp_xpc_set_string(&w,"Port","51234"); rp_xpc_dict_end(&w); }
    rp_xpc_dict_end(&w); rp_xpc_dict_end(&w);
    if (w.overflow){printf("writer overflow\n");return 1;}
    printf("  object %zu bytes\n", w.len);

    static uint8_t msg[1<<19];
    size_t mlen = rp_xpc_wrap(body, w.len, RP_XPC_F_ALWAYS_SET|RP_XPC_F_DATA_PRESENT, 1, msg, sizeof msg);
    if(!mlen){printf("wrap failed\n");return 1;}
    printf("  wrapped %zu bytes\n", mlen);

    /* Split across 16 KB DATA frames, which is what the device does. */
    slen=0; size_t off=0; int frames=0;
    while (off < mlen) {
        size_t chunk = mlen-off; if (chunk > 16384) chunk = 16384;
        size_t n = rp_h2_write_data(3, msg+off, chunk, stream+slen, sizeof stream - slen);
        if(!n){printf("frame write failed\n");return 1;}
        slen+=n; off+=chunk; frames++;
    }
    printf("  split into %d DATA frames on stream 3\n", frames);

    rp_rxpc_session s; static uint8_t re[1<<20], raw[1<<16];
    rp_rxpc_io io={r_,w_,NULL};
    rp_rxpc_init(&s,io,re,sizeof re,raw,sizeof raw);
    rp_xpc_obj got;
    if (rp_rxpc_recv(&s,&got)!=0){ printf("  FAIL: recv could not reassemble it\n"); return 1; }
    rp_xpc_obj svc;
    if (rp_xpc_dict_get(&got,"Services",&svc)!=0){ printf("  FAIL: no Services key\n"); return 1; }
    printf("  OK: %d services parsed\n", rp_xpc_dict_count(&svc));
    return 0;
}
