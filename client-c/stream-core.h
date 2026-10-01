/* stream-core.h — the portable video-stream engine shared by rplay-view (the minimal SDL viewer)
 * and rplay-gui (the ImGui app): Annex-B split, access-unit assembly, keyframe gating, RVRA
 * trailer parse, single-thread decode. Display and input stay with each front-end; frames are
 * delivered through stream_state.on_frame. Split out of rplay-view.c, whose behavior it keeps
 * verbatim (verified against the reference captures). */
#ifndef STREAM_CORE_H
#define STREAM_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <libavcodec/avcodec.h>

uint64_t now_ms(void);

/* ---- Annex-B splitter: byte stream in, complete NAL units out. */
typedef struct {
    uint8_t *buf;
    size_t   len, cap;
} annexb_parser;

typedef void (*nal_fn)(void *ctx, const uint8_t *nal, size_t len);

void annexb_feed(annexb_parser *p, const uint8_t *chunk, size_t n, nal_fn on_nal, void *ctx);
void annexb_finish(annexb_parser *p, nal_fn on_nal, void *ctx);

/* ---- codec model. The stream is one codec per session; g_h264 selects the NAL grammar. */
extern int g_h264;

/* ---- stream state: access-unit assembly + decode. Feed NALs with handle_nal (typically as
 * annexb_feed's callback); decoded frames arrive on on_frame with the RVRA active rect. */
typedef struct {
    uint8_t *au;                 /* the pending access unit, Annex-B */
    size_t   au_len, au_cap;
    size_t   last_vcl_off;       /* offset of the last VCL NAL's payload, for trailer stripping */
    size_t   last_vcl_len;
    int      awaiting_keyframe;
    uint64_t last_nal_ms;        /* for the idle flush */

    int      active_w, active_h; /* what the trailer last said; 0 until seen */
    uint64_t nals, frames_submitted, frames_before_keyframe, trailers;

    AVCodecContext *dec;
    AVPacket       *pkt;
    AVFrame        *frame;
    void (*on_frame)(AVFrame *f, int active_w, int active_h);
    uint64_t frames_decoded, decode_errors;
} stream_state;

void handle_nal(void *ctx, const uint8_t *nal, size_t len);
void flush_au(stream_state *s);

/* ---- transport helpers. */
int tcp_connect(const char *host, int port);
int stream_says_h264(const char *host, int api_port);

#ifdef __cplusplus
}
#endif

#endif
