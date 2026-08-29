// coredevice-c — from-scratch CoreDevice host in C, on the CarPlay SDK.
//
// Layers 0-2, the C port of scripts/coredevice-host/*.py, verified live against the same iPhone.
//   L0 usbmux · L1 lockdown · L1.5 TLS session (OpenSSL) · L2 CoreDevice tunnel (CDTunnel)
// Uses the SDK's CF plist API (CFLite = CoreFoundation-compatible) + OpenSSL (as the SDK's
// MFiClientPlatformOpenSSL does). The network RemotePairing door (SRP/ChaCha20/Curve25519 +
// BonjourBrowser) is the next module — see remotepairing.c.
//
// Build:  make            (macOS)      Run:  ./cdhost

#include <arpa/inet.h>
#include "tls.h"
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <limits.h>
#include <sys/un.h>
#include <unistd.h>

#include <stdarg.h>
#ifdef __APPLE__
#include <sys/kern_control.h>
#include <sys/sys_domain.h>
#endif
#include <sys/ioctl.h>
#include <net/if.h>
#include <pthread.h>
#include <signal.h>

#include "../core/rp_remotexpc.h"
#include "api_server.h"
#include "../core/rp_xpc.h"
#include "ddi.h"
#include "usernet.h"

#define USBMUX_TYPE_PLIST 8
#define CDTUNNEL_MAGIC "CDTunnel"

// ============================ connection (raw fd or TLS) ============================
/* `idev` is set for the CoreDevice tunnel connection, which now rides on libimobiledevice
 * (Stage 3); `fd`/`tls` are the old hand-rolled path, still used by the usbmux/lockdown code that
 * has not been migrated. Exactly one of {idev} or {fd,tls} is active per conn_t. */
typedef struct { int fd; rp_tls_conn *tls; void *idev; } conn_t;

static int cwrite(conn_t *c, const void *buf, size_t n) {
    if (c->idev) return imd_conn_send(c->idev, buf, n);
    size_t off = 0;
    while (off < n) {
        ssize_t r = c->tls ? rp_tls_write(c->tls, (const char *)buf + off, n - off)
                           : send(c->fd, (const char *)buf + off, n - off, 0);
        if (r <= 0) return -1;
        off += (size_t)r;
    }
    return 0;
}
static int cread_n(conn_t *c, void *buf, size_t n) {
    if (c->idev) return imd_conn_recv(c->idev, buf, n);
    size_t off = 0;
    while (off < n) {
        ssize_t r = c->tls ? rp_tls_read(c->tls, (char *)buf + off, n - off)
                           : recv(c->fd, (char *)buf + off, n - off, 0);
        if (r <= 0) return -1;
        off += (size_t)r;
    }
    return 0;
}

// ============================ Layer 2: CoreDevice tunnel (CDTunnel) ============================
// naive extractors for the small JSON handshake response
static int json_str(const char *j, const char *key, char *out, size_t outlen) {
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(j, pat);
    if (!p) return -1;
    p += strlen(pat);
    const char *e = strchr(p, '"');
    if (!e) return -1;
    size_t n = (size_t)(e - p); if (n >= outlen) n = outlen - 1;
    memcpy(out, p, n); out[n] = 0; return 0;
}
static long json_int(const char *j, const char *key) {
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(j, pat);
    return p ? strtol(p + strlen(pat), NULL, 10) : -1;
}
static int tunnel_handshake_full(conn_t *c, char *addr, size_t addrlen,
                                 char *ours, size_t ourslen, long *rsd_port, long *mtu) {
    const char *req = "{\"type\":\"clientHandshakeRequest\",\"mtu\":16000}";
    uint16_t blen = htons((uint16_t)strlen(req));
    if (cwrite(c, CDTUNNEL_MAGIC, 8) < 0 || cwrite(c, &blen, 2) < 0 || cwrite(c, req, strlen(req)) < 0)
        return -1;
    uint8_t hdr[10];
    if (cread_n(c, hdr, sizeof hdr) < 0) return -1;
    uint16_t plen = ntohs(*(uint16_t *)(hdr + 8));
    char *body = malloc(plen + 1);
    if (cread_n(c, body, plen) < 0) { free(body); return -1; }
    body[plen] = 0;
    int rc = json_str(body, "serverAddress", addr, addrlen);
    /* "address" appears inside clientParameters, which comes first — that is our end. */
    if (ours) json_str(body, "address", ours, ourslen);
    *rsd_port = json_int(body, "serverRSDPort");
    if (mtu) *mtu = json_int(body, "mtu");
    free(body);
    return rc;
}

/* ---------------------------------------------------------------- Layer 3a: utun + pump
 *
 * The tunnel socket carries raw IPv6 packets. Put those on a utun with a route and the device's
 * RSD port becomes reachable with an ordinary socket — which is the whole point, and the only
 * step in the stack that needs root.
 *
 * This is the C port of host/rplayhub/net/{tun,pump}.py. The failure signalling matters: if
 * either direction dies the tunnel is dead, and saying so beats looking alive.
 */
#ifdef __APPLE__
#define UTUN_CONTROL_NAME "com.apple.net.utun_control"
#define UTUN_OPT_IFNAME 2
#define IPV6_HDR_LEN 40

static int utun_open(char *ifname, size_t ifname_len) {
    int fd = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
    if (fd < 0) return -1;
    struct ctl_info ci;
    memset(&ci, 0, sizeof ci);
    strlcpy(ci.ctl_name, UTUN_CONTROL_NAME, sizeof ci.ctl_name);
    if (ioctl(fd, CTLIOCGINFO, &ci) < 0) { close(fd); return -1; }

    struct sockaddr_ctl sc;
    memset(&sc, 0, sizeof sc);
    sc.sc_len = sizeof sc;
    sc.sc_family = AF_SYSTEM;
    sc.ss_sysaddr = AF_SYS_CONTROL;
    sc.sc_id = ci.ctl_id;
    sc.sc_unit = 0;                     /* 0 => the kernel picks a free utun number */
    if (connect(fd, (struct sockaddr *)&sc, sizeof sc) < 0) { close(fd); return -1; }

    socklen_t len = (socklen_t)ifname_len;
    if (getsockopt(fd, SYSPROTO_CONTROL, UTUN_OPT_IFNAME, ifname, &len) < 0) {
        close(fd); return -1;
    }
    return fd;
}

static int run_cmd(const char *fmt, ...) {
    char cmd[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    return system(cmd);
}

static int utun_configure(const char *ifname, const char *ours, const char *device, long mtu) {
    if (run_cmd("ifconfig %s inet6 %s prefixlen 64 up >/dev/null 2>&1", ifname, ours) != 0)
        return -1;
    if (mtu > 0) run_cmd("ifconfig %s mtu %ld >/dev/null 2>&1", ifname, mtu);
    /* Host route for the device end; without it nothing knows to use this interface. */
    run_cmd("route add -inet6 %s -interface %s >/dev/null 2>&1", device, ifname);
    return 0;
}

typedef struct {
    int     utun;
    conn_t *tun;
    volatile int failed;
    const char *reason;
    unsigned long tx, rx;
} pump_t;

/* When either pump thread fails, the CoreDevice tunnel is dead and cannot be recovered without
 * re-establishing it -- which needs a full restart. Rather than leave the daemon serving a
 * live-looking-but-dead session (bug #7: it keeps answering tunnel_info while nothing flows),
 * exit the process. The kernel destroys the utun on exit; the app's control connection drops and
 * it reconnects; under launchd the daemon auto-restarts. A clean drop beats a silent black. */
/* Include errno and the traffic counters. Every past round of "why did it quit this time" was a
 * guessing game because the message named a stage and nothing else: five call sites, one string
 * each, no way to tell a dropped connection from a transient error, and no idea whether the
 * tunnel had ever carried anything. */
static void pump_die(const char *reason) {
    int e = errno;
    fprintf(stderr, "\n  tunnel died: %s (errno %d: %s) -- exiting so the session does not go "
                    "stale (bug #7)\n", reason ? reason : "unknown", e, e ? strerror(e) : "none");
    _exit(1);
}

static void *pump_host_to_device(void *arg) {
    pump_t *p = arg;
    uint8_t buf[70000];
    for (;;) {
        ssize_t n = read(p->utun, buf, sizeof buf);
        /* A signal interrupting the read is not the tunnel dying. macOS's signal() asks for
         * SA_RESTART so this should not happen, but "should not" is what made the previous
         * unexplained exits so expensive: retrying costs nothing and removes the whole class. */
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 4) { pump_die("utun read ended"); }
        if (cwrite(p->tun, buf + 4, (size_t)(n - 4)) < 0) {   /* strip the 4-byte AF prefix */
            pump_die("tunnel write failed");
        }
        p->tx++;
    }
}

static void *pump_device_to_host(void *arg) {
    pump_t *p = arg;
    uint8_t frame[70000];
    uint32_t af = htonl(AF_INET6);
    for (;;) {
        /* Reframe from a byte stream: Payload Length covers everything after the fixed header. */
        if (cread_n(p->tun, frame + 4, IPV6_HDR_LEN) < 0) {
            pump_die("tunnel read ended");
        }
        uint16_t plen = ntohs(*(uint16_t *)(frame + 4 + 4));
        if (plen && cread_n(p->tun, frame + 4 + IPV6_HDR_LEN, plen) < 0) {
            pump_die("tunnel read ended mid-packet");
        }
        memcpy(frame, &af, 4);
        ssize_t w;
        do { w = write(p->utun, frame, 4 + IPV6_HDR_LEN + plen); }
        while (w < 0 && (errno == EINTR || errno == EAGAIN));
        if (w < 0) {
            pump_die("utun write failed");
        }
        p->rx++;
    }
}
#endif /* __APPLE__ — the kernel-utun path; other platforms use RPLAY_USERSPACE_NET=1 (lwIP) */

/* Prove the routing works: an ordinary TCP connect to the device's RSD port. */
static int g_userspace;   /* fwd: set in main from RPLAY_USERSPACE_NET */
static int rsd_reachable(const char *device, long rsd_port) {
    if (g_userspace) {
        int s = usernet_connect(device, (int)rsd_port);
        if (s < 0) return -1;
        usernet_close(s);
        return 0;
    }
    struct sockaddr_in6 sa;
    memset(&sa, 0, sizeof sa);
    sa.sin6_family = AF_INET6;
    sa.sin6_port = htons((uint16_t)rsd_port);
    if (inet_pton(AF_INET6, device, &sa.sin6_addr) != 1) return -1;
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int rc = connect(fd, (struct sockaddr *)&sa, sizeof sa);
    close(fd);
    return rc;
}


/* ---------------------------------------------------------------- Layer 3b: RSD over RemoteXPC
 *
 * The tunnel gives us an ordinary socket to the device; this is what makes it useful. One
 * RemoteXPC handshake to the RSD port returns the service map -- roughly 85 entries naming every
 * coredevice.* feature and the port it listens on. Nothing else can be reached without it.
 *
 * The session layer lives in ../core so the daemon and the ports share it; only the socket
 * plumbing is here.
 */
/* g_userspace (declared above): reach the device over lwIP (no root utun) instead of kernel
 * sockets routed through the utun. Read/write route by fd: lwIP fds are offset above kernel fds. */
static long sock_read(void *ctx, void *buf, size_t len)
{
    int fd = *(int *)ctx;
    return usernet_owns(fd) ? usernet_read(fd, buf, len) : (long)recv(fd, buf, len, 0);
}

static long sock_write(void *ctx, const void *buf, size_t len)
{
    int fd = *(int *)ctx;
    return usernet_owns(fd) ? usernet_write(fd, buf, len) : (long)send(fd, buf, len, 0);
}

static int rsd_connect(const char *addr, long port)
{
    if (g_userspace) return usernet_connect(addr, (int)port);
    struct sockaddr_in6 sa;
    memset(&sa, 0, sizeof sa);
    sa.sin6_family = AF_INET6;
    sa.sin6_port = htons((uint16_t)port);
    if (inet_pton(AF_INET6, addr, &sa.sin6_addr) != 1) return -1;
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = { .tv_sec = 8, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return -1; }
    return fd;
}

/* Ask RSD what the device offers, and print it. Returns the number of services, or -1. */
static int rsd_enumerate(const char *addr, long port, api_session *out)
{
    int fd = rsd_connect(addr, port);
    if (fd < 0) { fprintf(stderr, "  cannot connect to [%s]:%ld\n", addr, port); return -1; }

    static uint8_t reassembly[1 << 20];   /* the service map runs to tens of kilobytes */
    static uint8_t raw[1 << 16];
    rp_rxpc_session s;
    rp_rxpc_io io = { sock_read, sock_write, &fd };
    rp_rxpc_init(&s, io, reassembly, sizeof reassembly, raw, sizeof raw);

    if (rp_rxpc_handshake(&s) != 0) {
        fprintf(stderr, "  RemoteXPC handshake failed\n");
        close(fd);
        return -1;
    }

    uint8_t body[512];
    rp_xpc_writer w;
    rp_xpc_writer_init(&w, body, sizeof body);
    rp_xpc_dict_begin(&w);
    rp_xpc_set_string(&w, "MessageType", "Handshake");
    rp_xpc_set_uint64(&w, "MessagingProtocolVersion", 7);
    {
        /* A fixed UUID is fine: the device echoes it and does not key anything on the value. */
        static const uint8_t uuid[16] = {0x12,0x34,0x56,0x78,0x12,0x34,0x56,0x78,
                                         0x12,0x34,0x56,0x78,0x12,0x34,0x56,0x78};
        rp_xpc_set_uuid(&w, "UUID", uuid);
    }
    rp_xpc_key(&w, "Properties");
    rp_xpc_dict_begin(&w);
    rp_xpc_set_uint64(&w, "RemoteXPCVersionFlags", 0x0100000000000006ULL);
    rp_xpc_set_bool(&w, "SensitivePropertiesVisible", true);
    rp_xpc_dict_end(&w);
    rp_xpc_key(&w, "Services");
    rp_xpc_dict_begin(&w);
    rp_xpc_dict_end(&w);
    rp_xpc_dict_end(&w);
    if (w.overflow) { fprintf(stderr, "  handshake message overflowed\n"); close(fd); return -1; }

    if (rp_rxpc_send(&s, body, w.len, 0) != 0) {
        fprintf(stderr, "  could not send the RSD handshake\n");
        close(fd);
        return -1;
    }

    /* The answer is whichever message carries Services or Properties; earlier ones are noise. */
    rp_xpc_obj peer, services;
    int replies = 0;
    for (;;) {
        if (rp_rxpc_recv(&s, &peer) != 0) {
            /* Say what came back before giving up. "No answer" and "an answer without the key we
             * wanted" are different faults and were previously indistinguishable. */
            fprintf(stderr, "  no RSD answer after %d message(s)\n", replies);
            close(fd);
            return -1;
        }
        replies++;
        if (rp_xpc_dict_get(&peer, "Services", &services) == 0) break;
        if (replies <= 3) {
            const char *k = NULL;
            rp_xpc_obj v;
            size_t cursor = 0;
            fprintf(stderr, "  (message %d has keys:", replies);
            while (rp_xpc_dict_next(&peer, &cursor, &k, &v) == 0 && cursor <= 6)
                fprintf(stderr, " %s", k);
            fprintf(stderr, ")\n");
        }
        if (replies > 20) {
            fprintf(stderr, "  gave up after %d messages without a Services map\n", replies);
            close(fd);
            return -1;
        }
    }

    int count = rp_xpc_dict_count(&services);
    printf("  %d services\n", count);

    /* RPLAY_DUMP_SERVICES=1 lists every advertised service and its port. The three below are
     * what mirroring needs, but the device offers far more -- pairing, restart, diagnostics,
     * app and process listing -- and there is no other way to see what this OS version
     * actually exposes. Names change between releases, so guessing them is not an option. */
    if (getenv("RPLAY_DUMP_SERVICES")) {
        size_t cursor = 0;
        const char *key = NULL;
        rp_xpc_obj entry;
        while (rp_xpc_dict_next(&services, &cursor, &key, &entry) == 0) {
            rp_xpc_obj portv;
            const char *ps = NULL;
            uint64_t p = 0;
            if (rp_xpc_dict_get(&entry, "Port", &portv) == 0) {
                if (rp_xpc_get_string(&portv, &ps) == 0) printf("    %-60s %s\n", key, ps);
                else if (rp_xpc_get_uint64(&portv, &p) == 0)
                    printf("    %-60s %llu\n", key, (unsigned long long)p);
                else printf("    %-60s (port unreadable)\n", key);
            } else {
                printf("    %-60s (no port)\n", key);
            }
        }
    }

    /* The three the mirroring product actually depends on, plus the Device Hub parity set:
     * diagnostics_relay (restart/shutdown/sleep) and syslog_relay are classic lockdown-style
     * services, appservice is CoreDevice like the first three. */
    static const char *want[] = {
        "com.apple.coredevice.screencaptureservice",
        "com.apple.coredevice.displayservice",
        "com.apple.coredevice.hid.universalhidservice",
        "com.apple.mobile.diagnostics_relay.shim.remote",
        "com.apple.coredevice.appservice",
        "com.apple.syslog_relay.shim.remote",
        "com.apple.mobile.installation_proxy.shim.remote",
        "com.apple.misagent.shim.remote",
        "com.apple.mobile.MCInstall.shim.remote",
        "com.apple.afc.shim.remote",
        "com.apple.crashreportcopymobile.shim.remote",
        "com.apple.crashreportmover.shim.remote",
        "com.apple.mobile.mobile_image_mounter.shim.remote",
        "com.apple.springboardservices.shim.remote",
    };
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        rp_xpc_obj svc, portv;
        uint64_t p = 0;
        if (rp_xpc_dict_get(&services, want[i], &svc) == 0 &&
            rp_xpc_dict_get(&svc, "Port", &portv) == 0) {
            const char *ps = NULL;
            long resolved = 0;
            if (rp_xpc_get_string(&portv, &ps) == 0) { resolved = strtol(ps, NULL, 10);
                printf("    %-46s port %s\n", want[i], ps); }
            else if (rp_xpc_get_uint64(&portv, &p) == 0) { resolved = (long)p;
                printf("    %-46s port %llu\n", want[i], (unsigned long long)p); }
            if (out) {
                if (i == 0) out->screenshot_port = resolved;
                else if (i == 1) out->display_port = resolved;
                else if (i == 2) out->hid_port = resolved;
                else if (i == 3) out->diag_port = resolved;
                else if (i == 4) out->app_port = resolved;
                else if (i == 5) out->syslog_port = resolved;
                else if (i == 6) out->instproxy_port = resolved;
                else if (i == 7) out->misagent_port = resolved;
                else if (i == 8) out->mcinstall_port = resolved;
                else if (i == 9) out->afc_port = resolved;
                else if (i == 10) out->crashcopy_port = resolved;
                else if (i == 11) out->crashmover_port = resolved;
                else if (i == 12) out->mounter_port = resolved;
                else out->sbservices_port = resolved;
            }
        } else {
            printf("    %-46s MISSING\n", want[i]);
        }
    }

    close(fd);
    return count;
}

/* Absolute path to this binary, captured at startup so a rebind can re-exec it. argv[0] is
 * relative when run as ./host-c/cdhost, and the working directory is not guaranteed later. */
static char g_self[PATH_MAX];

/* Every flag that takes a value, and the variable it sets.
 *
 * One table drives parsing, --help and the rebind re-exec together. Selecting a device in the app
 * re-executes this daemon, so a flag the rebind path does not know about is silently dropped at
 * that point and the session continues on the default -- which looks exactly like the device
 * ignoring the override. Adding a knob here carries it across a rebind by construction. */
static const struct { const char *flag, *env, *help; } g_flags[] = {
    { "--udid", "RPLAY_UDID",
      "which device to bind; a prefix is enough. Without it the USB device\n"
      "                    wins, which is wrong when the phone you want is on wifi." },
    { "--hevc-features", "RPLAY_HEVC_FEATURES",
      "override the offer's feature-list string. \"FLS;VRA:0;MVRA:0;RVRA1:0;SW:1;\"\n"
      "                    asks the encoder not to adapt resolution -- it does anyway, see\n"
      "                    doc/RVRA-AND-PORTABILITY.md." },
    { "--rctl", "RPLAY_RCTL",
      "target bitrate in bits/s to report in RTCP RCTL, 50 times a second.\n"
      "                    Default 6000000, the ceiling the device itself negotiates. \"0\"\n"
      "                    stops sending RCTL at all." },
    { "--max-bitrate", "RPLAY_MAX_BITRATE",
      "streamConfig TXMaxBitrate, in bits/s. Sent only when given, so the\n"
      "                    default offer stays byte-identical to Apple's." },
    { "--min-bitrate", "RPLAY_MIN_BITRATE", "streamConfig TXMinBitrate, in bits/s." },
};
#define N_FLAGS ((int)(sizeof g_flags / sizeof g_flags[0]))

void cdhost_rebind(const char *udid)
{
    if (!g_self[0] || !udid || !*udid) return;

    char *args[2 * N_FLAGS + 4];
    int n = 0;
    args[n++] = g_self;
    args[n++] = (char *)"--udid";
    args[n++] = (char *)udid;
    for (int i = 0; i < N_FLAGS; i++) {
        if (!strcmp(g_flags[i].flag, "--udid")) continue;   /* the caller's choice wins */
        const char *v = getenv(g_flags[i].env);
        if (!v || !*v) continue;
        args[n++] = (char *)g_flags[i].flag;
        args[n++] = (char *)v;
    }
    if (getenv("RPLAY_DUMP_SERVICES")) args[n++] = (char *)"--dump-services";
    args[n] = NULL;

    printf("\nrebinding to %s -- restarting\n", udid);
    fflush(stdout);

    /* Close everything before exec, or the new image inherits it.
     *
     * File descriptors survive execv unless they carry FD_CLOEXEC, and none of ours do. The
     * listening sockets on 9876 and 9877 came through still bound, so the replacement process
     * failed to bind them ("Address already in use") and then ran on with no API server at all --
     * pumping packets, answering nothing. The utun and the tunnel socket came through too.
     *
     * Closing from 3 upward is blunt and exactly right here: the process is about to be replaced,
     * so there is nothing left to preserve, and stdin/stdout/stderr must stay for the new image
     * to report anything. */
    int maxfd = getdtablesize();
    if (maxfd < 3 || maxfd > 65536) maxfd = 4096;
    for (int fd = 3; fd < maxfd; fd++) close(fd);

    execv(g_self, args);
    /* Only reached if exec failed; the caller has already replied, so say why and carry on
     * serving the device we still have rather than dying silently. */
    perror("execv");
}

/* Ctrl-C must end the daemon, every time.
 *
 * Default dispositions would do that, except that this process re-executes itself to switch
 * devices (cdhost_rebind) and an exec inherits both the signal mask and any SIG_IGN from the
 * image before it -- and once, a rebound daemon sat through Ctrl-C and a root `kill -INT`
 * (2026-08-23). Rather than argue about which ancestor ignored what, start from a known state:
 * unblock everything and own the terminating signals. The handler does not unwind: the kernel
 * releases the utun, the sockets and the threads on _exit, and there is nothing else to keep. */
static void on_terminate(int sig)
{
    static const char msg[] = "\n  signal received; cdhost exiting\n";
    write(2, msg, sizeof msg - 1);
    _exit(128 + sig);
}

static void own_signals(void)
{
    sigset_t none;
    sigemptyset(&none);
    pthread_sigmask(SIG_SETMASK, &none, NULL);
    signal(SIGINT, on_terminate);
    signal(SIGTERM, on_terminate);
    signal(SIGHUP, on_terminate);
    /* Ignore SIGPIPE here, at startup, not where the first socket is written.
     *
     * api_serve() sets this too, but that runs at Layer 4 -- after the packet pump threads have
     * been running since Layer 3a, through RSD enumeration and the DDI mount. A tunnel write to a
     * closed socket anywhere in that window raised SIGPIPE at its DEFAULT disposition and killed
     * the process instantly, with no "tunnel died" line and no other trace: a silent exit that
     * looks exactly like the bug #7 self-exit from outside but is not it. Disposition is
     * process-wide, so setting it once here covers every thread and every layer. */
    signal(SIGPIPE, SIG_IGN);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: a daemon's layer log should appear live */
    own_signals();

    /* Flags rather than environment only, because sudo strips the environment.
     *
     * `RPLAY_UDID=... sudo ./cdhost` sets the variable for sudo, which then discards it, so the
     * daemon binds whatever it would have anyway -- silently, and three runs were lost to that
     * before anyone noticed. `sudo env VAR=... ./cdhost` works, but a flag cannot be got wrong.
     * The environment variables still work for anything already using them. */
    /* The absolute path to THIS executable, for the device-switch re-exec (cdhost_rebind).
     * argv[0] is unreliable under launchd -- the SMAppService daemon is started with the relative
     * BundleProgram path "Contents/MacOS/cdhost" and an unknown CWD, so realpath(argv[0]) fails and
     * execv then gets "No such file or directory". _NSGetExecutablePath always returns the real
     * path. */
    {
#ifdef __APPLE__
        char raw[PATH_MAX];
        uint32_t sz = sizeof raw;
        if (_NSGetExecutablePath(raw, &sz) == 0 && realpath(raw, g_self)) {
            /* got it */
        } else
#else
        /* Linux: /proc/self/exe is the same always-real path the dyld call provides on macOS. */
        if (!realpath("/proc/self/exe", g_self))
#endif
        if (!realpath(argv[0], g_self)) {
            snprintf(g_self, sizeof g_self, "%s", argv[0]);
        }
    }

    for (int i = 1; i < argc; i++) {
        int matched = 0;
        for (int f = 0; f < N_FLAGS; f++) {
            if (strcmp(argv[i], g_flags[f].flag)) continue;
            if (i + 1 >= argc) {
                fprintf(stderr, "%s needs a value\n", g_flags[f].flag);
                return 2;
            }
            setenv(g_flags[f].env, argv[++i], 1);
            matched = 1;
            break;
        }
        if (matched) continue;
        if (!strcmp(argv[i], "--dump-services")) {
            setenv("RPLAY_DUMP_SERVICES", "1", 1);
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("usage: cdhost [--dump-services]");
            for (int f = 0; f < N_FLAGS; f++) printf(" [%s <value>]", g_flags[f].flag);
            printf("\n\n");
            for (int f = 0; f < N_FLAGS; f++)
                printf("  %-17s %s\n", g_flags[f].flag, g_flags[f].help);
            printf("  %-17s %s\n", "--dump-services",
                   "print every service RSD advertises, with ports.");
            return 0;
        } else {
            fprintf(stderr, "unknown argument: %s (try --help)\n", argv[i]);
            return 2;
        }
    }

    /* Say what is overridden, every time. A run whose override did not apply looks exactly like a
     * device ignoring it, and that mistake has already cost one wrong conclusion here. */
    for (int f = 0; f < N_FLAGS; f++) {
        const char *v = getenv(g_flags[f].env);
        if (v && *v) printf("%s = %s\n", g_flags[f].env, v);
    }

    /* Layers 0-2 now run on libimobiledevice (host-c/imd.c): usbmux discovery, the pair-record
     * TLS session, device queries and StartService(CoreDeviceProxy), returning a connected TLS'd
     * channel. This is the piece that did not port -- the usbmux unix socket and lockdown's TLS
     * are macOS-specific; libusbmuxd/libimobiledevice speak the same protocols on Linux/Windows.
     * The CoreDeviceProxy handshake and the packet pump below are unchanged: they run over conn_t,
     * which now wraps the idevice connection. */
    static char udid[128] = {0}, devname[256] = "?", prodver[64] = "?";
    void *idev_conn = NULL;
    if (imd_bringup(&idev_conn, udid, sizeof udid, devname, sizeof devname, prodver, sizeof prodver) != 0)
        return 1;

    conn_t tun = { -1, NULL, idev_conn };
    char addr[128] = "?", ours[128] = "?"; long rsd = -1, mtu = 0;
    if (tunnel_handshake_full(&tun, addr, sizeof addr, ours, sizeof ours, &rsd, &mtu) != 0) {
        fprintf(stderr, "  tunnel handshake failed\n");
        return 1;
    }
    printf("  handshake: us=%s device=%s serverRSDPort=%ld mtu=%ld\n", ours, addr, rsd, mtu);

    g_userspace = getenv("RPLAY_USERSPACE_NET") && getenv("RPLAY_USERSPACE_NET")[0] == '1';
    usernet_enable(g_userspace);

    if (g_userspace) {
        /* Userspace TCP/IP over the tunnel -- no root, no utun, no TUN driver (the Linux/Windows
         * and App Store path). lwIP runs the IP stack in-process over the CoreDeviceProxy
         * connection; every tunnel connection below opens an lwIP socket instead of a kernel one. */
        printf("\n== Layer 3a: userspace TCP/IP (lwIP, no root) ==\n");
        if (usernet_start(tun.idev, ours, addr) != 0) { fprintf(stderr, "  usernet_start failed\n"); return 1; }
        printf("  lwIP netif up over the tunnel\n");
    } else {
#ifndef __APPLE__
        /* The kernel tunnel path is the macOS utun; a Linux TUN equivalent has not been needed
         * because the userspace stack covers it without root. */
        fprintf(stderr, "  the kernel utun path is macOS-only — run with RPLAY_USERSPACE_NET=1\n");
        return 1;
#else
        printf("\n== Layer 3a: utun + packet pump ==\n");
        if (geteuid() != 0) {
            printf("  not root — cannot create a utun. Re-run with sudo, or set RPLAY_USERSPACE_NET=1\n");
            printf("      sudo %s\n", "./cdhost");
            printf("\nLayers 0-2 verified in C against the device.\n");
            return 0;
        }
        char ifname[64] = {0};
        int utun = utun_open(ifname, sizeof ifname);
        if (utun < 0) { perror("  utun_open"); return 1; }
        printf("  utun: %s\n", ifname);
        if (utun_configure(ifname, ours, addr, mtu) < 0) {
            fprintf(stderr, "  could not configure %s\n", ifname);
            return 1;
        }
        static pump_t pump;
        pump.utun = utun; pump.tun = &tun; pump.failed = 0; pump.reason = NULL;
        pthread_t t1, t2;
        pthread_create(&t1, NULL, pump_host_to_device, &pump);
        pthread_create(&t2, NULL, pump_device_to_host, &pump);
        printf("  pump running\n");
#endif /* __APPLE__ */
    }

    sleep(1);
    if (rsd_reachable(addr, rsd) == 0)
        printf("  ✅ RSD reachable at [%s]:%ld — the tunnel is live\n", addr, rsd);
    else
        printf("  ✗ could not connect to [%s]:%ld\n", addr, rsd);

    printf("\n== Layer 3b: RSD over RemoteXPC ==\n");
    static api_session session;
    session.udid = udid;
    session.device_name = devname;
    session.product_version = prodver;
    session.tunnel_addr = addr;
    session.our_addr = ours;
    session.rsd_port = rsd;
    /* ONE identity for this receiver. It goes into the offer as field 5.1, the device echoes it
     * back as RemoteSSRC, and every RTCP packet must carry it. Deriving both from this single
     * value is what stops them drifting apart -- when they did, the device silently discarded
     * every ack, PLI and FIR we sent. */
    session.ssrc = (uint32_t)(time(NULL) ^ (uintptr_t)&session);
    if (!session.ssrc) session.ssrc = 1;
    {
        const char *k = getenv("RPLAY_KEYFRAME_EVERY_S");
        session.keyframe_every_s = k ? atof(k) : 3.0;
    }
    if (rsd_enumerate(addr, rsd, &session) < 0)
        fprintf(stderr, "  service discovery failed\n");

    /* Self-activation: iOS 17+ discards the developer disk image on every reboot, and without it
     * the screen/control services stay silent. Mount it ourselves rather than requiring Xcode or
     * Device Hub -- the whole point of shipping this daemon (host/ddi_mount.py proved the flow,
     * ddi.c is the C port). Only when the phone is new enough to need it and does not already have
     * one; a locked phone is reported, not retried, since the mount needs it unlocked. */
    /* Where a shipped DDI would be: Contents/Resources/iOS_DDI, two dirs up from the executable
     * (Contents/MacOS/cdhost). NULL when that folder is absent (dev builds), which makes ddi.c
     * fall back to the system Xcode location. */
    static char bundle_ddi[1400];
    { char exe[1200]; snprintf(exe, sizeof exe, "%s", g_self);
      char *macos = strrchr(exe, '/'); if (macos) *macos = 0;      /* .../Contents/MacOS */
      char *contents = strrchr(exe, '/'); if (contents) *contents = 0;  /* .../Contents */
      snprintf(bundle_ddi, sizeof bundle_ddi, "%s/Resources/iOS_DDI", exe);
      struct stat st; char probe[1500];
      snprintf(probe, sizeof probe, "%s/Restore/BuildManifest.plist", bundle_ddi);
      session.ddi_dir = (stat(probe, &st) == 0) ? bundle_ddi : NULL; }

    if (session.mounter_port && prodver[0] && atoi(prodver) >= 17) {
        printf("\n== Layer 3c: developer disk image ==\n");
        int drc = cdhost_ddi_activate(addr, session.mounter_port, session.ddi_dir);
        if (drc == RP_DDI_ALREADY)      printf("  already mounted\n");
        else if (drc == RP_DDI_OK)      printf("  mounted (no Xcode/Device Hub needed)\n");
        else if (drc == RP_DDI_LOCKED)  printf("  the device is locked -- unlock it, then reconnect\n");
        else if (drc == RP_DDI_NO_DDI)  printf("  DDI files not bundled; mount via Xcode/Device Hub, or set RPLAY_DDI\n");
        else                            printf("  could not mount the DDI (rc=%d); mirroring may not work\n", drc);
    }

    printf("\n== Layer 4: daemon ==\n");
    /* Same ports and same JSON contract as the Python engine, so the existing app connects to
     * this without being told which engine it reached. That is what makes the port checkable a
     * method at a time rather than all at once. */
    /* A daemon that cannot serve is not a daemon. This return value used to be dropped, so a
     * failure to bind 9876 left it running the pump loop forever -- printing packet counts,
     * answering nothing, and looking alive to anyone reading the terminal. */
    if (api_serve(&session) < 0) {
        fprintf(stderr, "  api server did not start; shutting down\n");
        return 1;
    }
    /* api_serve blocks until the process is killed; a dead tunnel exits via pump_die (utun path)
     * and the kernel reclaims the utun on exit. Reaching here means the server stopped. */
    return 1;
}
