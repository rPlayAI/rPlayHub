#include "media.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
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
#define STREAM_TIMEOUT_S         600

struct media_session {
    int      udp;                 /* RTP and RTCP share this socket */
    int      svc;                 /* the RemoteXPC connection that negotiated the stream */
    rp_rxpc_session rxpc;
    uint8_t *rxpc_reassembly, *rxpc_raw;

    rp_rtp_session  rtp;
    rp_rtcp_session rtcp;
    uint8_t        *rtp_storage;

    struct sockaddr_in6 peer;     /* where their packets come from is where ours must go */
    int      have_peer;

    pthread_t recv_thread, rtcp_thread;
    volatile int stop;

    media_nal_fn on_nal;
    void        *ctx;
    double       keyframe_every_s;

    /* Only intact frames are acknowledged: telling the encoder to re-anchor on a frame we could
     * not reconstruct corrupts everything after it, which is the exact failure LTR exists to
     * prevent. */
    uint64_t lost_at_frame_start;
    unsigned emitted_in_packet;

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

/* ------------------------------------------------------------------ NAL classification */

static int hevc_type(const uint8_t *nal, size_t n) { return n ? (nal[0] >> 1) & 0x3F : -1; }

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
}

/* ------------------------------------------------------------------ receive */

static void *recv_loop(void *arg)
{
    media_session *m = arg;
    uint8_t pkt[65536];
    while (!m->stop) {
        struct sockaddr_in6 from;
        socklen_t flen = sizeof from;
        ssize_t n = recvfrom(m->udp, pkt, sizeof pkt, 0, (struct sockaddr *)&from, &flen);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            break;
        }
        if (!m->have_peer) { m->peer = from; m->have_peer = 1; }
        m->packets++;
        m->bytes += (uint64_t)n;

        if (rp_rtp_is_rtcp(pkt, (size_t)n)) {
            rp_rtcp_note_rtcp(&m->rtcp, pkt, (size_t)n, now_ms());
            continue;
        }
        rp_rtcp_note_rtp(&m->rtcp, pkt, (size_t)n);

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
            if (m->on_nal) m->on_nal(m->ctx, NULL, 0, 0, 0, 1);
            if (m->rtp.lost == m->lost_at_frame_start && m->rtp.lost == lost_before) {
                uint8_t ack[32];
                size_t an = rp_rtcp_build_ltr_ack(&m->rtcp, ts, ack, sizeof ack);
                if (an && m->have_peer)
                    sendto(m->udp, ack, an, 0, (struct sockaddr *)&m->peer, sizeof m->peer);
            }
            m->lost_at_frame_start = m->rtp.lost;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ RTCP timer */

static void *rtcp_loop(void *arg)
{
    media_session *m = arg;
    uint64_t last_rr = 0, last_pli = 0;
    while (!m->stop) {
        struct timespec s = {0, 200 * 1000 * 1000};
        nanosleep(&s, NULL);
        if (!m->have_peer) continue;
        uint64_t t = now_ms();
        uint8_t buf[64];

        /* Once a second, because the device's own answer asks for exactly that and stops sending
         * video after twenty seconds without it. */
        if (t - last_rr >= 1000) {
            size_t n = rp_rtcp_build_rr(&m->rtcp, t, buf, sizeof buf);
            if (n) sendto(m->udp, buf, n, 0, (struct sockaddr *)&m->peer, sizeof m->peer);
            last_rr = t;
        }
        /* Periodic keyframes. The device sends exactly one IDR unprompted, so without asking,
         * any corruption stays on screen for the rest of the session. It answers a PLI once the
         * request carries the SSRC it registered. */
        if (m->keyframe_every_s > 0 &&
            t - last_pli >= (uint64_t)(m->keyframe_every_s * 1000)) {
            size_t n = rp_rtcp_build_pli(&m->rtcp, buf, sizeof buf);
            if (n) sendto(m->udp, buf, n, 0, (struct sockaddr *)&m->peer, sizeof m->peer);
            last_pli = t;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ negotiation */

static int connect_service(media_session *m, const char *addr, long port);

media_session *media_start(const media_config *cfg, media_nal_fn on_nal, void *ctx)
{
    media_session *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->udp = m->svc = -1;
    m->on_nal = on_nal;
    m->ctx = ctx;
    m->keyframe_every_s = cfg->keyframe_every_s;

    m->rtp_storage = malloc((size_t)RP_RTP_REORDER_WINDOW * 1500 + RP_RTP_MAX_NAL);
    m->rxpc_reassembly = malloc(1 << 20);
    m->rxpc_raw = malloc(1 << 16);
    if (!m->rtp_storage || !m->rxpc_reassembly || !m->rxpc_raw) goto fail;

    rp_rtp_init(&m->rtp, RP_RTP_CODEC_HEVC, m->rtp_storage,
                (size_t)RP_RTP_REORDER_WINDOW * 1500 + RP_RTP_MAX_NAL, nal_cb, m);
    rp_rtcp_init(&m->rtcp, cfg->ssrc);

    /* Bind first: the port number goes into the offer. */
    m->udp = socket(AF_INET6, SOCK_DGRAM, 0);
    if (m->udp < 0) goto fail;
    struct sockaddr_in6 bind_addr;
    memset(&bind_addr, 0, sizeof bind_addr);
    bind_addr.sin6_family = AF_INET6;
    if (bind(m->udp, (struct sockaddr *)&bind_addr, sizeof bind_addr) != 0) goto fail;
    socklen_t blen = sizeof bind_addr;
    getsockname(m->udp, (struct sockaddr *)&bind_addr, &blen);
    int recv_port = ntohs(bind_addr.sin6_port);

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
        if (setsockopt(m->udp, SOL_SOCKET, SO_RCVBUF, &want, sizeof want) != 0) continue;
        int got = 0; socklen_t glen = sizeof got;
        if (getsockopt(m->udp, SOL_SOCKET, SO_RCVBUF, &got, &glen) == 0 && got >= want / 2) {
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
    setsockopt(m->udp, SOL_SOCKET, SO_TRAFFIC_CLASS, &(int){ SO_TC_VI }, sizeof(int));
#endif
#ifdef SO_NET_SERVICE_TYPE
    setsockopt(m->udp, SOL_SOCKET, SO_NET_SERVICE_TYPE, &(int){ NET_SERVICE_TYPE_VI }, sizeof(int));
#endif

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(m->udp, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    if (connect_service(m, cfg->device_addr, cfg->display_port) != 0) goto fail;

    /* The offer, and the request that carries it. */
    static uint8_t offer[2048];
    rp_offer_params op = {0};
    op.ssrc = cfg->ssrc;
    op.codec = RP_OFFER_CODEC_AUTO;
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
    rp_xpc_set_string(&w, "receiverIP", cfg->our_addr);
    rp_xpc_set_uint64(&w, "receiverPort", (uint64_t)recv_port);
    rp_xpc_set_string(&w, "senderIP", cfg->device_addr);
    rp_xpc_set_uint64(&w, "timeout", STREAM_TIMEOUT_S);
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

    if (connect(m->svc, (struct sockaddr *)&sa, sizeof sa) != 0) return -1;

    static long (*rd)(void *, void *, size_t);
    static long (*wr)(void *, const void *, size_t);
    (void)rd; (void)wr;
    extern long media_sock_read(void *ctx, void *buf, size_t len);
    extern long media_sock_write(void *ctx, const void *buf, size_t len);
    rp_rxpc_io io = { media_sock_read, media_sock_write, &m->svc };
    rp_rxpc_init(&m->rxpc, io, m->rxpc_reassembly, 1 << 20, m->rxpc_raw, 1 << 16);
    return rp_rxpc_handshake(&m->rxpc);
}

long media_sock_read(void *ctx, void *buf, size_t len)  { return (long)recv(*(int *)ctx, buf, len, 0); }
long media_sock_write(void *ctx, const void *buf, size_t len) { return (long)send(*(int *)ctx, buf, len, 0); }

void media_stop(media_session *m)
{
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
    if (m->udp >= 0) close(m->udp);
    if (m->svc >= 0) close(m->svc);
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
    if (lost) *lost = m->rtp.lost;
    if (bad) *bad = m->rtp.malformed + m->rtp.truncated;
    if (late) *late = m->rtp.late;
    if (dup) *dup = m->rtp.duplicates;
    if (ltr_acked) *ltr_acked = m->rtcp.ltr_acked;
    if (mbps) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        double secs = (double)(t.tv_sec - m->started.tv_sec) +
                      (double)(t.tv_nsec - m->started.tv_nsec) / 1e9;
        *mbps = secs > 0 ? (double)m->bytes * 8.0 / secs / 1e6 : 0.0;
    }
}
