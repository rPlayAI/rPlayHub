#include "media.h"
#include "rp_rtp_assembler.h"
#include "usernet.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <pthread.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "../core/rp_coredevice.h"
#include "../core/rp_media_offer.h"
#include "../core/rp_remotexpc.h"
#include "../core/rp_xpc.h"

#define CLIENT_SUPPORTED_FEATURES 140
#define ACCESS_NETWORK_TYPE        1
#define TRANSPORT_PROTOCOL_TYPE    2
/* Apple's own value, byte-for-byte. NOT a free parameter: the device echoes it back as
 * RTCPTimeoutInterval, and an oversized value (600 was ours for a while) makes it fall back to a
 * NON-RESETTABLE 20 s fuse -- the stream then dies at exactly 20 s no matter what RTCP arrives.
 * With a sane value the timer resets on our 1/s Receiver Reports and the session lives.
 * Measured on iPhone14,2 / iOS 27.0: timeout=600 -> dead at 20.0 s every run; timeout=40 ->
 * outlives both marks. RPLAY_STREAM_TIMEOUT_S overrides for experiments. */
#define STREAM_TIMEOUT_S         20

struct media_session {
    int      udp;                 /* RTP and RTCP share this socket */
    int      svc;                 /* the RemoteXPC connection that negotiated the stream */
    rp_rxpc_session rxpc;
    uint8_t *rxpc_reassembly, *rxpc_raw;

    rp_rtp_session  rtp;
    rp_rtcp_session rtcp;
    uint8_t        *rtp_storage;

    struct sockaddr_in6 peer;     /* where their packets come from is where ours must go */
    usernet_addr        upeer;    /* the same, when the socket is lwIP (usernet_owns(m->udp)) */
    int      have_peer;

    pthread_t recv_thread, rtcp_thread;
    volatile int stop;

    media_nal_fn on_nal;
    void        *ctx;
    double       keyframe_every_s;
    void       (*on_discontinuity)(void *ctx);
    void       (*on_active_rect)(void *ctx, uint32_t width, uint32_t height);
    uint16_t     last_ext_profile;
    /* When the last loss-triggered keyframe request went out, so a burst of damaged frames asks
     * once rather than once per frame. Asking per frame would make the storm worse: each request
     * costs another ~130 kB IDR, delivered as ~110 back-to-back packets. */
    uint64_t     last_disc_pli_ms;
    uint64_t     discontinuities;

    /* Only intact frames are acknowledged: telling the encoder to re-anchor on a frame we could
     * not reconstruct corrupts everything after it, which is the exact failure LTR exists to
     * prevent. */
    uint64_t lost_at_frame_start;
    /* The vendored Miracast assembler, used instead of rp_rtp when RPLAY_ASSEMBLER=miracast. */
    int use_ra;
    rp_rtp_assembler ra;
    rp_ra_packet *ra_queue;
    uint8_t *ra_nal;
    unsigned emitted_in_packet;
    /* Bytes per access unit. Device Hub's own capture has a median of 4.4 KB but a p90 of 11.6 KB
     * and peaks of 60-119 KB: its encoder spends when motion demands it. If ours never reaches
     * those peaks the encoder is being held down for us, which no amount of work on the receive
     * path can fix -- and every counter would still read clean, exactly as they do. */
    uint64_t frame_bytes, frame_max, frame_sum, frame_count;
    int fwd_fd;                       /* -1 unless RPLAY_RTP_FORWARD is set */
    struct sockaddr_in fwd_addr;

    int      negotiated;      /* the device is holding a stream slot for us */
    uint64_t packets, bytes, nals, keyframes;
    struct timespec started;
};

static uint64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)(t.tv_nsec / 1000000);
}

/* Send to wherever the device's packets come from, over whichever stack owns the socket. */
static long udp_send_peer(media_session *m, const void *buf, size_t len)
{
    if (!m->have_peer) return -1;
#ifdef HAVE_USERNET
    if (usernet_owns(m->udp)) return usernet_sendto(m->udp, buf, len, &m->upeer);
#endif
    return (long)sendto(m->udp, buf, len, 0, (struct sockaddr *)&m->peer, sizeof m->peer);
}

/* ------------------------------------------------------------------ NAL classification */

static int hevc_type(const uint8_t *nal, size_t n) { return n ? (nal[0] >> 1) & 0x3F : -1; }

/* The assembler delivers whole NALs and signals frame ends itself, so both paths converge on
 * the same media_nal_fn the consumer already sees. */
static void ra_nal_cb(void *ctx, const uint8_t *nal, size_t len);
static void frame_size_note(media_session *m);
static void note_discontinuity(media_session *m);
static void ra_frame_cb(void *ctx);

static void nal_cb(void *ctx, const uint8_t *nal, size_t len)
{
    media_session *m = ctx;
    int t = hevc_type(nal, len);
    int is_param = (t >= 32 && t <= 34);
    int is_key = (t >= 16 && t <= 23);
    m->nals++;
    if (is_key) m->keyframes++;

    static uint8_t framed[RP_RTP_MAX_NAL + 4];
    framed[0] = 0; framed[1] = 0; framed[2] = 0; framed[3] = 1;
    if (len > RP_RTP_MAX_NAL) return;
    memcpy(framed + 4, nal, len);
    /* The marker applies to the last NAL out of this packet. We do not know which NAL that is
     * until the packet is fully depacketized, so it is stamped afterwards, below. */
    if (m->on_nal) m->on_nal(m->ctx, framed, len + 4, is_param, is_key, 0);
    m->emitted_in_packet++;
    m->frame_bytes += len;
}

/* ------------------------------------------------------------------ receive */

static void *recv_loop(void *arg)
{
    media_session *m = arg;
    uint8_t pkt[65536];
    /* RPLAY_NET_DEBUG=1: drain-rate accounting. The recvmbox overflows silently when this loop
     * is the bottleneck, so print consumed pkts/s and where the time went every 2 s. */
    int dbg = getenv("RPLAY_NET_DEBUG") && getenv("RPLAY_NET_DEBUG")[0] == '1';
    uint64_t dbg_last = 0, dbg_pkts = 0, dbg_recv_us = 0, dbg_work_us = 0;
    while (!m->stop) {
        ssize_t n;
        uint64_t t_in = 0;
        if (dbg) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
                   t_in = (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000; }
#ifdef HAVE_USERNET
        if (usernet_owns(m->udp)) {
            usernet_addr from;
            n = (ssize_t)usernet_recvfrom(m->udp, pkt, sizeof pkt, &from);
            if (n > 0 && !m->have_peer) { m->upeer = from; m->have_peer = 1; }
        } else
#endif
        {
            struct sockaddr_in6 from;
            socklen_t flen = sizeof from;
            n = recvfrom(m->udp, pkt, sizeof pkt, 0, (struct sockaddr *)&from, &flen);
            if (n > 0 && !m->have_peer) { m->peer = from; m->have_peer = 1; }
        }
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                /* Idle. This is what enforces the 100 ms deadline when no further packet arrives
                 * to drive it; without it a gap at the end of a burst would hold the picture. */
                if (m->use_ra) rp_ra_tick(&m->ra, now_ms() * 1000ull);
                continue;
            }
            break;
        }
        m->packets++;
        m->bytes += (uint64_t)n;
        if (dbg) {
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            uint64_t t_got = (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000;
            dbg_recv_us += t_got - t_in;      /* time blocked waiting in recvfrom */
            dbg_pkts++;
            if (!dbg_last) dbg_last = t_got;
            if (t_got - dbg_last >= 2000000ull) {
                fprintf(stderr, "  [recv_loop] %.0f pkt/s, recv-wait %.0f%%, work %.0f%%\n",
                        dbg_pkts * 1e6 / (double)(t_got - dbg_last),
                        100.0 * (double)dbg_recv_us / (double)(t_got - dbg_last),
                        100.0 * (double)dbg_work_us / (double)(t_got - dbg_last));
                dbg_last = t_got; dbg_pkts = 0; dbg_recv_us = 0; dbg_work_us = 0;
            }
            t_in = t_got;                     /* from here on we time the work */
        }

        /* Optional: hand a verbatim copy of every RTP packet to a second, independent receiver.
         * RPLAY_RTP_FORWARD=<port> mirrors the stream to 127.0.0.1:<port>, where ffmpeg or
         * GStreamer can depacketize it with a stack that is not ours. Same packets, two
         * implementations -- if a production RTP receiver renders the same corruption, our
         * depacketizer is not the cause, and if it does not, it is.
         *
         * MEASURED CAVEAT: this is not free, and my first version of this comment claimed it was.
         * Enabling the mirror took a stream that had run at 0.00% loss for tens of thousands of
         * packets to 7.9-10.6% loss with decode failures to match. One extra syscall per packet on
         * the receive thread is enough to make it miss the next datagram, so the mirror IS part of
         * what it measures. The socket is non-blocking and given a large send buffer to reduce
         * that, but do not treat a mirrored run as a clean baseline -- compare mirrored against
         * mirrored, and take loss figures from a run with the mirror off. */
        if (m->fwd_fd >= 0)
            sendto(m->fwd_fd, pkt, (size_t)n, 0,
                   (struct sockaddr *)&m->fwd_addr, sizeof m->fwd_addr);

#define DBG_WORK_DONE() do { if (dbg) { struct timespec ts_; clock_gettime(CLOCK_MONOTONIC, &ts_); \
        dbg_work_us += (uint64_t)ts_.tv_sec * 1000000ull + (uint64_t)ts_.tv_nsec / 1000 - t_in; } } while (0)
        if (rp_rtp_is_rtcp(pkt, (size_t)n)) {
            rp_rtcp_note_rtcp(&m->rtcp, pkt, (size_t)n, now_ms());
            DBG_WORK_DONE();
            continue;
        }
        rp_rtcp_note_rtp(&m->rtcp, pkt, (size_t)n, now_ms());

        /* Watch the extension profile: it is how this device says the coded picture shrank. */
        uint16_t prof = rp_rtp_ext_profile(pkt, (size_t)n);
        if (prof && prof != m->last_ext_profile) {
            m->last_ext_profile = prof;
            uint32_t aw = 0, ah = 0;
            rp_rtp_active_rect(prof, &aw, &ah);
            fprintf(stderr, "  active rect: profile 0x%04x -> %ux%u\n", prof, aw, ah);
            if (aw && ah && m->on_active_rect) m->on_active_rect(m->ctx, aw, ah);
        }

        if (m->use_ra) {
            /* The assembler owns sequencing, fragment reassembly and the frame boundary, and
             * raises the LTR acknowledgement from its own callback -- it must, because with a
             * reorder queue the packet that completes a frame is not necessarily the one that
             * just arrived, which is precisely the case the old path got wrong. */
            rp_ra_feed(&m->ra, pkt, (size_t)n, now_ms() * 1000ull);
            DBG_WORK_DONE();
            continue;
        }

        uint64_t lost_before = m->rtp.lost;
        m->emitted_in_packet = 0;
        rp_rtp_feed(&m->rtp, pkt, (size_t)n);

        /* The marker bit ends a frame, so this is the moment to acknowledge it -- but only if
         * nothing went missing while it was arriving. */
        int marker = 0;
        uint32_t ts = 0;
        if (rp_rtp_header(pkt, (size_t)n, NULL, &ts, &marker, NULL) == 0 && marker) {
            /* Announce the end of the access unit. A zero-length NAL carries no data and exists
             * only to say "that was the last one" -- which lets the consumer submit the picture
             * now instead of waiting for the next picture to start. */
            frame_size_note(m);
            if (m->on_nal) m->on_nal(m->ctx, NULL, 0, 0, 0, 1);
            if (m->rtp.lost == m->lost_at_frame_start && m->rtp.lost == lost_before) {
                uint8_t ack[32];
                size_t an = rp_rtcp_build_ltr_ack(&m->rtcp, ts, ack, sizeof ack);
                if (an) udp_send_peer(m, ack, an);
            } else {
                /* The same test that decides not to acknowledge also decides that what we just
                 * handed the consumer cannot be decoded safely. Only the first half was ever
                 * acted on. */
                note_discontinuity(m);
            }
            m->lost_at_frame_start = m->rtp.lost;
        }
        DBG_WORK_DONE();
    }
#undef DBG_WORK_DONE
    return NULL;
}

/* ------------------------------------------------------------------ RTCP timer */

static void *rtcp_loop(void *arg)
{
    media_session *m = arg;
    uint64_t last_rr = 0, last_pli = 0, last_rctl = 0;
    /* What to ask the encoder for, in bits per second. The device answers TX/RXMaxBitrate = 6 Mbps
     * and Device Hub asks for exactly that. RPLAY_RCTL=0 disables sending RCTL at all, so the
     * before/after can be compared without rebuilding. */
    const char *rctl_env = getenv("RPLAY_RCTL");
    int rctl_on = !(rctl_env && rctl_env[0] == '0' && rctl_env[1] == '\0');
    uint32_t rctl_target = 6000000;
    if (rctl_env && rctl_env[0] && !(rctl_env[0] == '0' && rctl_env[1] == '\0')) {
        long v = strtol(rctl_env, NULL, 10);
        if (v > 0) rctl_target = (uint32_t)v;
    }
    while (!m->stop) {
        struct timespec s = {0, 200 * 1000 * 1000};
        nanosleep(&s, NULL);
        if (!m->have_peer) continue;
        uint64_t t = now_ms();
        uint8_t buf[64];

        /* Once a second, because the device's own answer asks for exactly that and stops sending
         * video after twenty seconds without it. The SR is what actually feeds its liveness
         * timer (see rp_rtcp_build_sr); the RR carries the loss/jitter story. Device Hub sends
         * both, each compounded with SDES. */
        if (t - last_rr >= 1000) {
            size_t n = rp_rtcp_build_sr(&m->rtcp, buf, sizeof buf);
            if (n) udp_send_peer(m, buf, n);
            n = rp_rtcp_build_rr(&m->rtcp, t, buf, sizeof buf);
            if (n) udp_send_peer(m, buf, n);
            last_rr = t;
        }
        /* Rate-control feedback, on a free-running 50 ms timer.
         *
         * Measured from Device Hub's own capture: mean interval 50.16 ms, std 5.9 ms, and only 112
         * of 290 landed within 5 ms of a frame — a timer, not a frame-driven send. Always alone in
         * its own datagram, never compounded with SR/RR/SDES, so it is sent here rather than
         * appended to the receiver report. */
        if (rctl_on && t - last_rctl >= 50) {
            size_t n = rp_rtcp_build_rctl(&m->rtcp, t, rctl_target, buf, sizeof buf);
            long sent = -1;
            if (n) sent = udp_send_peer(m, buf, n);
            static int reported = 0;
            if (!reported) {
                reported = 1;
                fprintf(stderr, "  rctl: on=%d target=%u built=%zu sent=%ld%s%s\n",
                        rctl_on, rctl_target, n, sent,
                        sent < 0 ? " errno=" : "", sent < 0 ? strerror(errno) : "");
            }
            last_rctl = t;
        }

        /* Periodic keyframes. The device sends exactly one IDR unprompted, so without asking,
         * any corruption stays on screen for the rest of the session. It answers a PLI once the
         * request carries the SSRC it registered. */
        if (m->keyframe_every_s > 0 &&
            t - last_pli >= (uint64_t)(m->keyframe_every_s * 1000)) {
            size_t n = rp_rtcp_build_pli(&m->rtcp, buf, sizeof buf);
            if (n) udp_send_peer(m, buf, n);
            last_pli = t;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ negotiation */

static int connect_service(media_session *m, const char *addr, long port);

/* Debug: print every key/scalar of an XPC object (RPLAY_NET_DEBUG). The negotiation answer is
 * where the device states its ports and intervals, and it has never been looked at. */
static void dump_xpc(const rp_xpc_obj *o, int depth)
{
    char pad[32];
    snprintf(pad, sizeof pad, "%*s", depth * 2 + 2, "");
    rp_xpc_type t = rp_xpc_obj_type(o);
    if (t == RP_XPC_DICT) {
        size_t cur = 0; const char *k; rp_xpc_obj v;
        while (rp_xpc_dict_next(o, &cur, &k, &v) == 0) {
            rp_xpc_type vt = rp_xpc_obj_type(&v);
            if (vt == RP_XPC_DICT || vt == RP_XPC_ARRAY) {
                fprintf(stderr, "%s%s:\n", pad, k);
                dump_xpc(&v, depth + 1);
            } else {
                uint64_t u; const char *s; bool b;
                if (rp_xpc_get_uint64(&v, &u) == 0) fprintf(stderr, "%s%s = %llu\n", pad, k, (unsigned long long)u);
                else if (rp_xpc_get_string(&v, &s) == 0) fprintf(stderr, "%s%s = \"%s\"\n", pad, k, s);
                else if (rp_xpc_get_bool(&v, &b) == 0) fprintf(stderr, "%s%s = %s\n", pad, k, b ? "true" : "false");
                else if (vt == RP_XPC_DOUBLE && v.size >= 12) {
                    double d; memcpy(&d, v.data + 4, 8);
                    fprintf(stderr, "%s%s = %g\n", pad, k, d);
                }
                else fprintf(stderr, "%s%s (type 0x%x, %zu B)\n", pad, k, (unsigned)vt, v.size);
            }
        }
    } else if (t == RP_XPC_ARRAY) {
        size_t cur = 0; rp_xpc_obj v; int i = 0;
        while (rp_xpc_array_next(o, &cur, &v) == 0) {
            fprintf(stderr, "%s[%d]:\n", pad, i++);
            dump_xpc(&v, depth + 1);
        }
    }
}

/* Debug: after negotiation nothing reads the display-service channel, so anything the device
 * sends there -- HTTP/2 PINGs, GOAWAY, RemoteXPC keepalives -- vanishes into the receive buffer.
 * Watch it and say what arrives. */
static void *svc_watch_loop(void *arg)
{
    media_session *m = arg;
    uint8_t buf[512];
    for (;;) {
        long n = tun_read(m->svc, buf, sizeof buf);
        if (n == 0) { fprintf(stderr, "  [svc] channel EOF\n"); return NULL; }
        if (n < 0) { continue; }   /* recv timeout: keep watching */
        fprintf(stderr, "  [svc %ld B] ", n);
        for (long i = 0; i < n && i < 48; i++) fprintf(stderr, "%02x", buf[i]);
        fprintf(stderr, "\n");
    }
}

static void frame_size_note(media_session *m)
{
    if (!m->frame_bytes) return;
    if (m->frame_bytes > m->frame_max) m->frame_max = m->frame_bytes;
    m->frame_sum += m->frame_bytes;
    m->frame_count++;
    m->frame_bytes = 0;
    if (m->frame_count % 300 == 0)
        fprintf(stderr, "  frames %llu: mean %llu B, peak %llu B "
                        "(Device Hub: mean 6947, p90 11650, peak 119368)\n",
                (unsigned long long)m->frame_count,
                (unsigned long long)(m->frame_sum / m->frame_count),
                (unsigned long long)m->frame_max);
}

/* This access unit did not arrive intact.
 *
 * Two things follow, and neither used to happen. The consumer is told, so it can stop decoding
 * until a keyframe re-anchors the reference chain — decoding onwards from a missing reference
 * produces a picture that is wrong and stays wrong. And a keyframe is requested immediately
 * rather than waiting for the periodic timer, which at the default cadence could be three
 * seconds away: three seconds of pictures predicted from a reference nobody has. */
static void note_discontinuity(media_session *m)
{
    m->discontinuities++;
    if (m->on_discontinuity) m->on_discontinuity(m->ctx);

    uint64_t t = now_ms();
    if (t - m->last_disc_pli_ms < 250) return;   /* one ask per burst, not one per frame */
    m->last_disc_pli_ms = t;
    uint8_t buf[64];
    size_t n = rp_rtcp_build_pli(&m->rtcp, buf, sizeof buf);
    if (n) udp_send_peer(m, buf, n);
}

/* One NAL from the vendored assembler: frame it and hand it on, exactly as nal_cb does. */
static void ra_nal_cb(void *ctx, const uint8_t *nal, size_t len)
{
    nal_cb(ctx, nal, len);
}

/* The access unit is complete. Announce it, then acknowledge it as a long-term reference -- but
 * only if nothing was declared lost while it was being assembled. Acknowledging a frame we did
 * not receive intact would have the device predict from a reference we do not hold, and every
 * frame after it would drift until the next keyframe. */
static void ra_frame_cb(void *ctx)
{
    media_session *m = (media_session *)ctx;
    frame_size_note(m);
    if (m->on_nal) m->on_nal(m->ctx, NULL, 0, 0, 0, 1);

    if (m->ra.lost == m->lost_at_frame_start) {
        uint8_t ack[32];
        size_t an = rp_rtcp_build_ltr_ack(&m->rtcp, m->ra.last_rtp_time, ack, sizeof ack);
        if (an) udp_send_peer(m, ack, an);
    } else {
        note_discontinuity(m);
    }
    m->lost_at_frame_start = m->ra.lost;
}

/* The kernel-socket receive path: bind an ephemeral port, then fight for buffer and queueing. */
static int kernel_udp_socket(int *recv_port)
{
    int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in6 bind_addr;
    memset(&bind_addr, 0, sizeof bind_addr);
    bind_addr.sin6_family = AF_INET6;
    if (bind(fd, (struct sockaddr *)&bind_addr, sizeof bind_addr) != 0) { close(fd); return -1; }
    socklen_t blen = sizeof bind_addr;
    getsockname(fd, (struct sockaddr *)&bind_addr, &blen);
    *recv_port = ntohs(bind_addr.sin6_port);

    /* A burst from a full-screen transition is a few hundred packets at once; the default
     * receive buffer drops them and the loss shows up as permanent corruption.
     *
     * Read the size back rather than trusting the request. The kernel clamps this to
     * kern.ipc.maxsockbuf without failing the call, so a silent clamp to the 786 KB default
     * looks exactly like success -- and a burst that overflows leaves no trace on this side
     * beyond a sequence gap that is indistinguishable from loss on the wire. Step down until
     * one is actually granted. */
    int rcvbuf = 0;
    for (int want = 4 * 1024 * 1024; want >= 512 * 1024; want /= 2) {
        if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &want, sizeof want) != 0) continue;
        int got = 0; socklen_t glen = sizeof got;
        if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &got, &glen) == 0 && got >= want / 2) {
            rcvbuf = got;
            break;
        }
    }
    if (rcvbuf < 1024 * 1024)
        fprintf(stderr, "  warning: UDP receive buffer is only %d KB; "
                        "fast motion will overflow it (raise kern.ipc.maxsockbuf)\n",
                rcvbuf / 1024);
    /* Classify the flow as video, the way Apple's own AccessorySDK does for media sockets.
     * This is what decides which queue the packets sit in when the link is contended -- on
     * Wi-Fi it selects the WMM access category -- so it costs nothing and is exactly the case
     * where a burst currently hurts: a fast swipe, not a still screen. */
#ifdef SO_TRAFFIC_CLASS
    setsockopt(fd, SOL_SOCKET, SO_TRAFFIC_CLASS, &(int){ SO_TC_VI }, sizeof(int));
#endif
#ifdef SO_NET_SERVICE_TYPE
    setsockopt(fd, SOL_SOCKET, SO_NET_SERVICE_TYPE, &(int){ NET_SERVICE_TYPE_VI }, sizeof(int));
#endif

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return fd;
}

media_session *media_start(const media_config *cfg, media_nal_fn on_nal, void *ctx)
{
    media_session *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->udp = m->svc = -1;
    m->on_nal = on_nal;
    m->fwd_fd = -1;
    const char *fwd = getenv("RPLAY_RTP_FORWARD");
    if (fwd && *fwd) {
        m->fwd_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (m->fwd_fd >= 0) {
            memset(&m->fwd_addr, 0, sizeof m->fwd_addr);
            m->fwd_addr.sin_family = AF_INET;
            m->fwd_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            m->fwd_addr.sin_port = htons((uint16_t)atoi(fwd));
            /* Never let the copy stall the receive thread: non-blocking, and a send buffer big
             * enough that a bursty consumer does not push back within one frame. */
            int fl = fcntl(m->fwd_fd, F_GETFL, 0);
            if (fl >= 0) fcntl(m->fwd_fd, F_SETFL, fl | O_NONBLOCK);
            setsockopt(m->fwd_fd, SOL_SOCKET, SO_SNDBUF, &(int){ 4 * 1024 * 1024 }, sizeof(int));
            fprintf(stderr, "  mirroring RTP to 127.0.0.1:%s for an independent receiver\n", fwd);
        }
    }
    m->ctx = ctx;
    m->keyframe_every_s = cfg->keyframe_every_s;
    m->on_discontinuity = cfg->on_discontinuity;
    m->on_active_rect = cfg->on_active_rect;

    m->rtp_storage = malloc((size_t)RP_RTP_REORDER_WINDOW * 1500 + RP_RTP_MAX_NAL);
    m->rxpc_reassembly = malloc(1 << 20);
    m->rxpc_raw = malloc(1 << 16);
    if (!m->rtp_storage || !m->rxpc_reassembly || !m->rxpc_raw) goto fail;

    const char *asm_sel = getenv("RPLAY_ASSEMBLER");
    m->use_ra = (asm_sel && !strcmp(asm_sel, "miracast"));
    if (m->use_ra) {
        m->ra_queue = calloc(RP_RA_MAX_PACKETS, sizeof *m->ra_queue);
        m->ra_nal = malloc(RP_RA_MAX_NAL);
        if (!m->ra_queue || !m->ra_nal) { m->use_ra = 0; }
        else if (rp_ra_init(&m->ra, RP_RA_CODEC_AUTO, m->ra_queue, RP_RA_MAX_PACKETS,
                            m->ra_nal, RP_RA_MAX_NAL, ra_nal_cb, ra_frame_cb, m) != 0) {
            m->use_ra = 0;
        } else {
            fprintf(stderr, "  using the vendored Miracast assembler "
                            "(100 ms reorder deadline, discontinuity on loss)\n");
        }
    }

    rp_rtp_init(&m->rtp, RP_RTP_CODEC_HEVC, m->rtp_storage,
                (size_t)RP_RTP_REORDER_WINDOW * 1500 + RP_RTP_MAX_NAL, nal_cb, m);
    rp_rtcp_init(&m->rtcp, cfg->ssrc);

    /* Bind first: the port number goes into the offer. In userspace mode the RTP flow rides the
     * lwIP stack, so video keeps the same no-root property as the control path; the burst
     * headroom the kernel path buys with SO_RCVBUF is sized in lwip-engine/lwipopts.h instead
     * (PBUF pool, tcpip mbox, UDP recvmbox), and the traffic-class options do not apply because
     * the packets travel inside the tunnel's TCP connection, which has its own socket setup. */
    int recv_port = 0;
#ifdef HAVE_USERNET
    if (usernet_is_on()) {
        m->udp = usernet_udp_socket(&recv_port);
        if (m->udp < 0) goto fail;
        usernet_set_recv_timeout_ms(m->udp, 1000);
    } else
#endif
    {
        m->udp = kernel_udp_socket(&recv_port);
        if (m->udp < 0) goto fail;
    }

    if (connect_service(m, cfg->device_addr, cfg->display_port) != 0) goto fail;

    /* The offer, and the request that carries it. */
    static uint8_t offer[2048];
    rp_offer_params op = {0};
    op.ssrc = cfg->ssrc;
    /* Which codec banks to offer. AUTO offers both and lets the device choose -- and the note
     * in rp_media_offer.h claiming it then chooses HEVC is simply not true: a live session was
     * measured arriving on payload type 100, the H.264 bank. That matters a great deal at the
     * bitrate this device picks. H.264 needs roughly twice the bits of HEVC for equal quality,
     * so ~2.3 Mbit/s at 1184x2544 is thin in HEVC and hopeless in H.264 -- and it falls apart
     * under motion, which is exactly the symptom. RPLAY_CODEC=hevc offers only the HEVC bank. */
    op.codec = RP_OFFER_CODEC_AUTO;
    const char *want_codec = getenv("RPLAY_CODEC");
    if (want_codec && !strcmp(want_codec, "hevc")) op.codec = RP_OFFER_CODEC_HEVC;
    else if (want_codec && !strcmp(want_codec, "h264")) op.codec = RP_OFFER_CODEC_H264;
    op.model = "Mac15,9";
    op.os_version = "2205.3.1";
    op.build = "25F80";
    op.call_id = "1E312779-5E86-4742-9215-7522E1EB1610";
    op.ltrp_enabled = 1;
    size_t offer_len = rp_build_offer(&op, offer, sizeof offer);
    if (!offer_len) goto fail;

    static uint8_t input[4096];
    rp_xpc_writer w;
    rp_xpc_writer_init(&w, input, sizeof input);
    rp_xpc_dict_begin(&w);
    rp_xpc_set_uint64(&w, "clientSupportedFeatures", CLIENT_SUPPORTED_FEATURES);
    rp_xpc_set_string(&w, "direction", "output");
    rp_xpc_key(&w, "negotiatorOffer");
    rp_xpc_data(&w, offer, offer_len);
    rp_xpc_key(&w, "options");
    rp_xpc_dict_begin(&w);
    rp_xpc_key(&w, "AVCMediaStreamNegotiatorAccessNetworkType");
    rp_xpc_dict_begin(&w); rp_xpc_set_int64(&w, "int", ACCESS_NETWORK_TYPE); rp_xpc_dict_end(&w);
    rp_xpc_key(&w, "AVCMediaStreamNegotiatorTransportProtocolType");
    rp_xpc_dict_begin(&w); rp_xpc_set_int64(&w, "int", TRANSPORT_PROTOCOL_TYPE); rp_xpc_dict_end(&w);
    rp_xpc_key(&w, "CoreDeviceVideoDisplayMode");
    rp_xpc_dict_begin(&w); rp_xpc_set_string(&w, "string", "DisplayByID"); rp_xpc_dict_end(&w);
    rp_xpc_key(&w, "VideoStreamForDisplayID");
    rp_xpc_dict_begin(&w); rp_xpc_set_int64(&w, "int", 1); rp_xpc_dict_end(&w);
    rp_xpc_key(&w, "avcMediaStreamOptionClientSessionID");
    rp_xpc_dict_begin(&w);
    {
        static const uint8_t csid[16] = {0x16,0x0f,0xd8,0xd6,0x03,0x69,0x48,0xc1,
                                         0xaa,0xd8,0x3a,0x96,0x3a,0xde,0x19,0x3f};
        rp_xpc_set_uuid(&w, "uuid", csid);
    }
    rp_xpc_dict_end(&w);
    rp_xpc_dict_end(&w);

    /* streamConfig — the encoding settings the device otherwise picks alone.
     *
     * A sibling of `options` and `negotiatorOffer`, and the one lever with real headroom: at the
     * negotiated 1184x2576 the ~2-4 Mbps the device grants is around 0.03 bits/pixel, several
     * times below what moving UI content needs, which is why a fast swipe outruns the encoder.
     * Device Hub measures the same, so this is a budget to escape rather than a bug to fix.
     * Asking for half the coded width and height quarters the pixel count and puts the same
     * bitrate near 0.12 bits/pixel.
     *
     * The values are PLAIN scalars, not the {"int": n} wrappers `options` uses. Sending wrapped
     * values here hung startmediastream once — the device reads an integer and finds a
     * dictionary. host/screen.py carries the same warning.
     *
     * Omitted entirely unless asked for, so the default path stays byte-identical to Apple's. */
    {
        static const struct { const char *env, *key; } knobs[] = {
            { "RPLAY_WIDTH",             "CustomWidth"       },
            { "RPLAY_HEIGHT",            "CustomHeight"      },
            { "RPLAY_FPS",               "Framerate"         },
            { "RPLAY_VIDEO_RESOLUTION",  "VideoResolution"   },
            { "RPLAY_MAX_BITRATE",       "TXMaxBitrate"      },
            { "RPLAY_MIN_BITRATE",       "TXMinBitrate"      },
            { "RPLAY_KEYFRAME_INTERVAL", "KeyFrameInterval"  },
        };
        int any = 0;
        for (size_t i = 0; i < sizeof knobs / sizeof knobs[0]; i++)
            if (getenv(knobs[i].env)) { any = 1; break; }
        if (any) {
            rp_xpc_key(&w, "streamConfig");
            rp_xpc_dict_begin(&w);
            for (size_t i = 0; i < sizeof knobs / sizeof knobs[0]; i++) {
                const char *raw = getenv(knobs[i].env);
                if (!raw || !*raw) continue;
                char *end = NULL;
                long v = strtol(raw, &end, 10);
                if (end == raw || *end) continue;      /* not a number: ignore, do not guess */
                rp_xpc_set_int64(&w, knobs[i].key, (int64_t)v);
                fprintf(stderr, "  streamConfig %s=%ld\n", knobs[i].key, v);
            }
            rp_xpc_dict_end(&w);
        }
    }

    rp_xpc_set_string(&w, "receiverIP", cfg->our_addr);
    rp_xpc_set_uint64(&w, "receiverPort", (uint64_t)recv_port);
    rp_xpc_set_string(&w, "senderIP", cfg->device_addr);
    /* RPLAY_STREAM_TIMEOUT_S overrides the offered timeout (default 600). The device echoes this
     * back as RTCPTimeoutInterval; varying it tells apart "our RTCP never resets that timer"
     * (death follows the value) from "a separate fixed lease" (death stays at 20 s). */
    {
        uint64_t tmo = STREAM_TIMEOUT_S;
        const char *ts = getenv("RPLAY_STREAM_TIMEOUT_S");
        if (ts && ts[0]) {
            long v = strtol(ts, NULL, 10);
            if (v > 0) tmo = (uint64_t)v;
        }
        rp_xpc_set_uint64(&w, "timeout", tmo);
    }
    rp_xpc_set_string(&w, "type", "video");
    rp_xpc_dict_end(&w);
    if (w.overflow) goto fail;

    char ua[37] = "8f1e2c40-0000-4000-8000-000000000001";
    char ub[37] = "8f1e2c40-0000-4000-8000-000000000002";
    rp_xpc_obj out;
    if (rp_cd_invoke(&m->rxpc, RP_CD_FEATURE_STARTSTREAM, RP_CD_ACTION_STARTSTREAM,
                     input, w.len, ua, ub, &out, NULL) != 0) {
        fprintf(stderr, "  startmediastream returned no output\n");
        goto fail;
    }
    if (getenv("RPLAY_NET_DEBUG") && getenv("RPLAY_NET_DEBUG")[0] == '1') {
        dump_xpc(&out, 0);   /* the device's answer names ports and intervals we currently ignore */
        pthread_t dbg_t;
        pthread_create(&dbg_t, NULL, svc_watch_loop, m);   /* watch what the device says post-negotiation */
        pthread_detach(dbg_t);
    }
    printf("  media stream negotiated, receiving on port %d\n", recv_port);
    m->negotiated = 1;

    clock_gettime(CLOCK_MONOTONIC, &m->started);
    pthread_create(&m->recv_thread, NULL, recv_loop, m);
    pthread_create(&m->rtcp_thread, NULL, rtcp_loop, m);
    return m;

fail:
    media_stop(m);
    return NULL;
}

static int connect_service(media_session *m, const char *addr, long port)
{
#ifdef HAVE_USERNET
    if (usernet_is_on()) {
        /* The negotiation channel over the userspace stack. lwIP's connect enforces its own
         * SYN-retry deadline, so the SYN_SENT hang the kernel path guards against below cannot
         * pin the caller; the read deadline matches the kernel path's. */
        m->svc = usernet_connect(addr, (int)port);
        if (m->svc < 0) return -1;
        usernet_set_recv_timeout_ms(m->svc, 10000);
        goto handshake;
    }
#endif

    struct sockaddr_in6 sa;
    memset(&sa, 0, sizeof sa);
    sa.sin6_family = AF_INET6;
    sa.sin6_port = htons((uint16_t)port);
    if (inet_pton(AF_INET6, addr, &sa.sin6_addr) != 1) return -1;
    m->svc = socket(AF_INET6, SOCK_STREAM, 0);
    if (m->svc < 0) return -1;
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(m->svc, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    /* No Nagle: every write here is one complete request that the device must answer before we
     * proceed, so coalescing can only add a delay with nothing to coalesce with. And no SIGPIPE
     * -- a device unplugged mid-session would otherwise kill the whole process instead of
     * failing one write. Both from the AccessorySDK's socket setup. */
    setsockopt(m->svc, IPPROTO_TCP, TCP_NODELAY, &(int){ 1 }, sizeof(int));
#ifdef SO_NOSIGPIPE
    setsockopt(m->svc, SOL_SOCKET, SO_NOSIGPIPE, &(int){ 1 }, sizeof(int));
#endif

    /* Connect with a deadline. The app runs this on its main thread, and a stale tunnel address
     * (daemon restarted between tunnel_info and here) otherwise leaves it in SYN_SENT for the
     * kernel's ~75 s -- a frozen GUI that looks like a hang in whatever was clicked last. */
    int flags = fcntl(m->svc, F_GETFL, 0);
    fcntl(m->svc, F_SETFL, flags | O_NONBLOCK);
    if (connect(m->svc, (struct sockaddr *)&sa, sizeof sa) != 0) {
        if (errno != EINPROGRESS) return -1;
        fd_set wr;
        FD_ZERO(&wr);
        FD_SET(m->svc, &wr);
        struct timeval ctv = { .tv_sec = 5, .tv_usec = 0 };
        if (select(m->svc + 1, NULL, &wr, NULL, &ctv) <= 0) return -1;
        int err = 0;
        socklen_t elen = sizeof err;
        if (getsockopt(m->svc, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0) return -1;
    }
    fcntl(m->svc, F_SETFL, flags);

#ifdef HAVE_USERNET
handshake:;
#endif
    extern long media_sock_read(void *ctx, void *buf, size_t len);
    extern long media_sock_write(void *ctx, const void *buf, size_t len);
    rp_rxpc_io io = { media_sock_read, media_sock_write, &m->svc };
    rp_rxpc_init(&m->rxpc, io, m->rxpc_reassembly, 1 << 20, m->rxpc_raw, 1 << 16);
    return rp_rxpc_handshake(&m->rxpc);
}

/* When userspace networking is compiled in (the engine), tunnel I/O routes through tun_*, which
 * dispatches to lwIP for lwIP fds. The app compiles media.c without usernet/lwIP and only ever
 * runs the kernel path, so there it is a plain recv/send. */
#ifdef HAVE_USERNET
long media_sock_read(void *ctx, void *buf, size_t len)  { return tun_read(*(int *)ctx, buf, len); }
long media_sock_write(void *ctx, const void *buf, size_t len) { return tun_write(*(int *)ctx, buf, len); }
#else
long media_sock_read(void *ctx, void *buf, size_t len)  { return (long)recv(*(int *)ctx, buf, len, 0); }
long media_sock_write(void *ctx, const void *buf, size_t len) { return (long)send(*(int *)ctx, buf, len, 0); }
#endif

void media_stop(media_session *m)
{
    if (m && m->fwd_fd >= 0) { close(m->fwd_fd); m->fwd_fd = -1; }
    if (m) { free(m->ra_queue); m->ra_queue = NULL; free(m->ra_nal); m->ra_nal = NULL; }
    if (!m) return;

    /* Tell the device to release the stream before dropping the sockets.
     *
     * It allows ONE media stream per session and holds the slot for the negotiated lifetime --
     * ten minutes -- if nobody stops it. Now that the app owns the stream rather than a
     * long-lived daemon, restarts are frequent, and without this every restart would find the
     * device still busy from the previous run. */
    if (m->svc >= 0 && m->negotiated) {
        static const uint8_t csid[16] = {0x16,0x0f,0xd8,0xd6,0x03,0x69,0x48,0xc1,
                                         0xaa,0xd8,0x3a,0x96,0x3a,0xde,0x19,0x3f};
        uint8_t input[256];
        rp_xpc_writer w;
        rp_xpc_writer_init(&w, input, sizeof input);
        rp_xpc_dict_begin(&w);
        rp_xpc_key(&w, "avcMediaStreamOptionClientSessionID");
        rp_xpc_dict_begin(&w);
        rp_xpc_set_uuid(&w, "uuid", csid);
        rp_xpc_dict_end(&w);
        rp_xpc_dict_end(&w);
        if (!w.overflow) {
            rp_xpc_obj out;
            char ua[37] = "8f1e2c40-0000-4000-8000-00000000000a";
            char ub[37] = "8f1e2c40-0000-4000-8000-00000000000b";
            if (rp_cd_invoke(&m->rxpc, RP_CD_FEATURE_STOPSTREAM, RP_CD_ACTION_STOPSTREAM,
                             input, w.len, ua, ub, &out, NULL) == 0)
                printf("  media stream released\n");
        }
    }

    m->stop = 1;
    if (m->recv_thread) pthread_join(m->recv_thread, NULL);
    if (m->rtcp_thread) pthread_join(m->rtcp_thread, NULL);
#ifdef HAVE_USERNET
    if (m->udp >= 0) tun_close(m->udp);
    if (m->svc >= 0) tun_close(m->svc);
#else
    if (m->udp >= 0) close(m->udp);
    if (m->svc >= 0) close(m->svc);
#endif
    free(m->rtp_storage);
    free(m->rxpc_reassembly);
    free(m->rxpc_raw);
    free(m);
}

void media_stats(const media_session *m, uint64_t *packets, uint64_t *nals, uint64_t *keyframes,
                 uint64_t *lost, uint64_t *ltr_acked, double *mbps, uint64_t *bad,
                 uint64_t *late, uint64_t *dup)
{
    if (!m) {
        if (packets) *packets = 0; if (nals) *nals = 0; if (keyframes) *keyframes = 0;
        if (lost) *lost = 0; if (ltr_acked) *ltr_acked = 0; if (mbps) *mbps = 0;
        if (bad) *bad = 0; if (late) *late = 0; if (dup) *dup = 0;
        return;
    }
    if (packets) *packets = m->packets;
    if (nals) *nals = m->nals;
    if (keyframes) *keyframes = m->keyframes;
    if (m->use_ra) {
        if (lost) *lost = m->ra.lost;
        if (bad) *bad = m->ra.malformed;
        if (late) *late = m->ra.late;
        if (dup) *dup = m->ra.duplicates;
    } else {
        if (lost) *lost = m->rtp.lost;
        if (bad) *bad = m->rtp.malformed + m->rtp.truncated;
        if (late) *late = m->rtp.late;
        if (dup) *dup = m->rtp.duplicates;
    }
    if (ltr_acked) *ltr_acked = m->rtcp.ltr_acked;
    if (mbps) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        double secs = (double)(t.tv_sec - m->started.tv_sec) +
                      (double)(t.tv_nsec - m->started.tv_nsec) / 1e9;
        *mbps = secs > 0 ? (double)m->bytes * 8.0 / secs / 1e6 : 0.0;
    }
}
