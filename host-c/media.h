/*
 * media.h — the live screen stream: negotiate it, receive it, fan it out.
 *
 * Ties together the verified pieces in ../core: rp_media_offer builds what we ask for, rp_rtp
 * turns datagrams into NAL units, rp_rtcp keeps the stream alive and repairable. This file is the
 * plumbing around them — sockets, threads, and the viewer fan-out.
 *
 * Two device behaviours shape the whole design:
 *
 *  - The stream is started once and cannot be restarted within a session. Every attempt has
 *    failed, and a half-negotiated stream still occupies the device's single slot, so a retry
 *    turns "one viewer is blind" into "nobody gets video".
 *  - Without Receiver Reports about once a second the device stops sending after ~20 seconds. The
 *    RTCP thread is not a refinement; it is what keeps the picture on.
 */
#ifndef RP_MEDIA_H
#define RP_MEDIA_H

#include <stdint.h>

#include "../core/rp_rtcp.h"
#include "../core/rp_rtp.h"

typedef struct media_session media_session;

typedef struct {
    const char *device_addr;     /* device side of the tunnel */
    const char *our_addr;        /* our side */
    long        display_port;    /* displayservice, from RSD */
    uint32_t    ssrc;            /* offer field 5.1 AND our RTCP SSRC — they must be equal */
    double      keyframe_every_s;/* 0 disables periodic keyframe requests */
} media_config;

/* Called on the receive thread for every NAL, already framed with a start code. */
typedef void (*media_nal_fn)(void *ctx, const uint8_t *annexb, size_t len, int is_parameter_set,
                             int is_keyframe);

media_session *media_start(const media_config *cfg, media_nal_fn on_nal, void *ctx);
void media_stop(media_session *m);

/* Live counters for stream_info. */
void media_stats(const media_session *m, uint64_t *packets, uint64_t *nals, uint64_t *keyframes,
                 uint64_t *lost, uint64_t *ltr_acked, double *mbps);

#endif /* RP_MEDIA_H */
