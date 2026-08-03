// coredevice-c — from-scratch CoreDevice host in C, on the CarPlay SDK.
//
// Layers 0-2, the C port of scripts/coredevice-host/*.py, verified live against the same iPhone.
//   L0 usbmux · L1 lockdown · L1.5 TLS session (OpenSSL) · L2 CoreDevice tunnel (CDTunnel)
// Uses the SDK's CF plist API (CFLite = CoreFoundation-compatible) + OpenSSL (as the SDK's
// MFiClientPlatformOpenSSL does). The network RemotePairing door (SRP/ChaCha20/Curve25519 +
// BonjourBrowser) is the next module — see remotepairing.c.
//
// Build:  make            (macOS)      Run:  ./cdhost

#include <CoreFoundation/CoreFoundation.h>
#include <arpa/inet.h>
#include "tls.h"
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <stdarg.h>
#include <sys/kern_control.h>
#include <sys/sys_domain.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <pthread.h>

#include "../core/rp_remotexpc.h"
#include "api_server.h"
#include "../core/rp_xpc.h"

#define USBMUXD_SOCKET "/var/run/usbmuxd"
#define LOCKDOWN_PORT 62078
#define USBMUX_TYPE_PLIST 8
#define COREDEVICE_PROXY "com.apple.internal.devicecompute.CoreDeviceProxy"
#define CDTUNNEL_MAGIC "CDTunnel"

// ============================ connection (raw fd or TLS) ============================
typedef struct { int fd; rp_tls_conn *tls; } conn_t;

static int cwrite(conn_t *c, const void *buf, size_t n) {
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
    size_t off = 0;
    while (off < n) {
        ssize_t r = c->tls ? rp_tls_read(c->tls, (char *)buf + off, n - off)
                           : recv(c->fd, (char *)buf + off, n - off, 0);
        if (r <= 0) return -1;
        off += (size_t)r;
    }
    return 0;
}

// ============================ CF plist helpers (SDK CFLite API) ============================
static CFDataRef plist_to_xml(CFPropertyListRef pl) {
    return CFPropertyListCreateData(NULL, pl, kCFPropertyListXMLFormat_v1_0, 0, NULL);
}
static CFPropertyListRef xml_to_plist(const void *bytes, CFIndex len) {
    CFDataRef d = CFDataCreate(NULL, bytes, len);
    CFPropertyListRef pl = CFPropertyListCreateWithData(NULL, d, kCFPropertyListMutableContainers, NULL, NULL);
    CFRelease(d);
    return pl;
}
static CFMutableDictionaryRef dict_new(void) {
    return CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
}
static void dict_set_str(CFMutableDictionaryRef d, const char *k, const char *v) {
    CFStringRef ks = CFStringCreateWithCString(NULL, k, kCFStringEncodingUTF8);
    CFStringRef vs = CFStringCreateWithCString(NULL, v, kCFStringEncodingUTF8);
    CFDictionarySetValue(d, ks, vs); CFRelease(ks); CFRelease(vs);
}
static void dict_set_int(CFMutableDictionaryRef d, const char *k, int64_t v) {
    CFStringRef ks = CFStringCreateWithCString(NULL, k, kCFStringEncodingUTF8);
    CFNumberRef vn = CFNumberCreate(NULL, kCFNumberSInt64Type, &v);
    CFDictionarySetValue(d, ks, vn); CFRelease(ks); CFRelease(vn);
}
static CFTypeRef dict_get(CFDictionaryRef d, const char *key) {
    if (!d) return NULL;
    CFStringRef ks = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFTypeRef v = CFDictionaryGetValue(d, ks); CFRelease(ks);
    return v;
}
static int dict_get_cstr(CFDictionaryRef d, const char *key, char *out, size_t outlen) {
    CFTypeRef v = dict_get(d, key);
    if (!v) return -1;
    if (CFGetTypeID(v) == CFStringGetTypeID())
        return CFStringGetCString((CFStringRef)v, out, outlen, kCFStringEncodingUTF8) ? 0 : -1;
    if (CFGetTypeID(v) == CFNumberGetTypeID()) {
        long long n = 0; CFNumberGetValue((CFNumberRef)v, kCFNumberLongLongType, &n);
        snprintf(out, outlen, "%lld", n); return 0;
    }
    return -1;
}
static int dict_get_bool(CFDictionaryRef d, const char *key) {
    CFTypeRef v = dict_get(d, key);
    return (v && CFGetTypeID(v) == CFBooleanGetTypeID()) ? CFBooleanGetValue((CFBooleanRef)v) : 0;
}
// PEM bytes (cert/key stored as <data> in the pair record)
static const uint8_t *dict_get_bytes(CFDictionaryRef d, const char *key, CFIndex *len) {
    CFTypeRef v = dict_get(d, key);
    if (!v || CFGetTypeID(v) != CFDataGetTypeID()) return NULL;
    *len = CFDataGetLength((CFDataRef)v);
    return CFDataGetBytePtr((CFDataRef)v);
}

// ============================ Layer 0: usbmux ============================
static int usbmux_connect(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un a = {0};
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, USBMUXD_SOCKET, sizeof(a.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
    return fd;
}
static CFPropertyListRef usbmux_request(int fd, CFDictionaryRef payload, uint32_t tag) {
    CFDataRef body = plist_to_xml(payload);
    uint32_t blen = (uint32_t)CFDataGetLength(body);
    uint32_t hdr[4] = {blen + 16, 1, USBMUX_TYPE_PLIST, tag};
    conn_t c = {fd, NULL};
    if (cwrite(&c, hdr, sizeof hdr) < 0 || cwrite(&c, CFDataGetBytePtr(body), blen) < 0) {
        CFRelease(body); return NULL;
    }
    CFRelease(body);
    uint32_t rhdr[4];
    if (cread_n(&c, rhdr, sizeof rhdr) < 0) return NULL;
    uint32_t plen = rhdr[0] - 16;
    void *buf = malloc(plen);
    if (cread_n(&c, buf, plen) < 0) { free(buf); return NULL; }
    CFPropertyListRef pl = xml_to_plist(buf, plen);
    free(buf);
    return pl;
}
static CFMutableDictionaryRef usbmux_msg(const char *type) {
    CFMutableDictionaryRef d = dict_new();
    dict_set_str(d, "MessageType", type);
    dict_set_str(d, "ClientVersionString", "coredevice-c");
    dict_set_str(d, "ProgName", "coredevice-c");
    return d;
}
static int usbmux_list_devices(int fd, char *udid_out, size_t udid_len) {
    CFMutableDictionaryRef req = usbmux_msg("ListDevices");
    CFDictionaryRef reply = usbmux_request(fd, req, 1);
    CFRelease(req);
    if (!reply) return -1;
    CFArrayRef list = dict_get(reply, "DeviceList");
    int first_id = -1;
    int chose_usb = 0;
    for (CFIndex i = 0; list && i < CFArrayGetCount(list); i++) {
        CFDictionaryRef dev = CFArrayGetValueAtIndex(list, i);
        CFDictionaryRef props = dict_get(dev, "Properties");
        char id[32] = "?", ser[128] = "?", conn[32] = "?";
        dict_get_cstr(dev, "DeviceID", id, sizeof id);
        dict_get_cstr(props, "SerialNumber", ser, sizeof ser);
        dict_get_cstr(props, "ConnectionType", conn, sizeof conn);
        printf("  DeviceID=%s udid=%s conn=%s\n", id, ser, conn);
        /* Prefer USB when the same phone appears twice.
         *
         * usbmuxd lists a device once per connection type and proxies both identically, so either
         * works -- but not equally well. Over wifi displayservice is unreliable: it times out
         * starting a media stream far more often, and a tunnel that dies mid-session is common.
         * Taking whichever entry happened to be first made that luck rather than a choice. */
        /* RPLAY_UDID picks the device explicitly. Without it the USB preference below decides,
         * which is fine with one phone attached and wrong the moment there are two: the phone
         * you want may be the one on wifi, and there was no way to say so. A prefix match is
         * enough -- nobody wants to type a full UDID. */
        const char *want = getenv("RPLAY_UDID");
        if (want && *want) {
            if (strncmp(ser, want, strlen(want)) != 0) continue;
            long long v = 0; CFNumberGetValue(dict_get(dev, "DeviceID"), kCFNumberLongLongType, &v);
            first_id = (int)v;
            strncpy(udid_out, ser, udid_len - 1);
            chose_usb = strcmp(conn, "USB") == 0;
            continue;
        }
        int is_usb = strcmp(conn, "USB") == 0;
        if (first_id < 0 || (is_usb && !chose_usb)) {
            long long v = 0; CFNumberGetValue(dict_get(dev, "DeviceID"), kCFNumberLongLongType, &v);
            first_id = (int)v;
            strncpy(udid_out, ser, udid_len - 1);
            chose_usb = is_usb;
        }
    }
    if (first_id >= 0)
        printf("  -> using the %s entry\n", chose_usb ? "USB" : "Network");
    CFRelease(reply);
    return first_id;
}
/* Every attached device, for the sidebar. Separate from usbmux_list_devices above, which
 * chooses ONE device to bind the session to; this one reports them all and judges none. A phone
 * appearing twice (USB and Network) is collapsed to a single entry preferring USB, because it is
 * one phone and showing it twice would be a bug rather than a feature. */
int usbmux_enumerate(api_device *out, int max)
{
    int fd = usbmux_connect();
    if (fd < 0) return 0;
    CFMutableDictionaryRef req = usbmux_msg("ListDevices");
    CFDictionaryRef reply = usbmux_request(fd, req, 1);
    CFRelease(req);
    if (!reply) { close(fd); return 0; }

    int n = 0;
    CFArrayRef list = dict_get(reply, "DeviceList");
    for (CFIndex i = 0; list && i < CFArrayGetCount(list) && n < max; i++) {
        CFDictionaryRef dev = CFArrayGetValueAtIndex(list, i);
        CFDictionaryRef props = dict_get(dev, "Properties");
        char ser[128] = "", conn[32] = "";
        dict_get_cstr(props, "SerialNumber", ser, sizeof ser);
        dict_get_cstr(props, "ConnectionType", conn, sizeof conn);
        if (!ser[0]) continue;

        int existing = -1;
        for (int k = 0; k < n; k++) if (!strcmp(out[k].udid, ser)) { existing = k; break; }
        if (existing >= 0) {
            /* Same phone on both transports: keep USB, which is the more reliable one. */
            if (!strcmp(conn, "USB")) snprintf(out[existing].connection,
                                               sizeof out[existing].connection, "%s", conn);
            continue;
        }
        snprintf(out[n].udid, sizeof out[n].udid, "%s", ser);
        snprintf(out[n].connection, sizeof out[n].connection, "%s", conn[0] ? conn : "USB");
        n++;
    }
    CFRelease(reply);
    close(fd);
    return n;
}

static int usbmux_connect_port(int fd, int device_id, int port) {
    CFMutableDictionaryRef req = usbmux_msg("Connect");
    dict_set_int(req, "DeviceID", device_id);
    dict_set_int(req, "PortNumber", ((port << 8) & 0xFF00) | (port >> 8));
    CFDictionaryRef reply = usbmux_request(fd, req, 2);
    CFRelease(req);
    if (!reply) return -1;
    long long num = -1;
    CFNumberRef n = dict_get(reply, "Number");
    if (n) CFNumberGetValue(n, kCFNumberLongLongType, &num);
    CFRelease(reply);
    return num == 0 ? 0 : -1;
}
static CFDictionaryRef usbmux_read_pair_record(const char *udid) {
    int fd = usbmux_connect();
    if (fd < 0) return NULL;
    CFMutableDictionaryRef req = usbmux_msg("ReadPairRecord");
    dict_set_str(req, "PairRecordID", udid);
    CFDictionaryRef reply = usbmux_request(fd, req, 1);
    CFRelease(req); close(fd);
    if (!reply) return NULL;
    CFDataRef data = dict_get(reply, "PairRecordData");
    CFDictionaryRef pr = data ? xml_to_plist(CFDataGetBytePtr(data), CFDataGetLength(data)) : NULL;
    CFRelease(reply);
    return pr;
}

// ============================ Layer 1.5: TLS via OpenSSL ============================
static rp_tls_ctx *ctx_from_pairrecord(CFDictionaryRef pr) {
    CFIndex clen = 0, klen = 0;
    const uint8_t *cert = dict_get_bytes(pr, "HostCertificate", &clen);
    const uint8_t *key = dict_get_bytes(pr, "HostPrivateKey", &klen);
    if (!cert || !key) { fprintf(stderr, "pair record missing host cert/key\n"); return NULL; }
    return rp_tls_ctx_new(cert, (size_t)clen, key, (size_t)klen);
}
static int tls_upgrade(conn_t *c, rp_tls_ctx *ctx) {
    c->tls = rp_tls_connect(ctx, c->fd);
    return c->tls ? 0 : -1;
}

// ============================ Layer 1: lockdown ============================
static CFPropertyListRef lockdown_request(conn_t *c, CFDictionaryRef req) {
    CFDataRef body = plist_to_xml(req);
    uint32_t len = htonl((uint32_t)CFDataGetLength(body));
    if (cwrite(c, &len, 4) < 0 || cwrite(c, CFDataGetBytePtr(body), CFDataGetLength(body)) < 0) {
        CFRelease(body); return NULL;
    }
    CFRelease(body);
    uint32_t rlen;
    if (cread_n(c, &rlen, 4) < 0) return NULL;
    rlen = ntohl(rlen);
    void *buf = malloc(rlen);
    if (cread_n(c, buf, rlen) < 0) { free(buf); return NULL; }
    CFPropertyListRef pl = xml_to_plist(buf, rlen);
    free(buf);
    return pl;
}
static int lockdown_simple(conn_t *c, const char *request, const char *key, const char *value,
                           const char *out_key, char *out, size_t outlen) {
    CFMutableDictionaryRef req = dict_new();
    dict_set_str(req, "Request", request);
    if (key) dict_set_str(req, key, value);
    CFDictionaryRef r = lockdown_request(c, req);
    CFRelease(req);
    if (!r) return -1;
    int rc = out ? dict_get_cstr(r, out_key, out, outlen) : 0;
    CFRelease(r);
    return rc;
}
static int lockdown_get_value(conn_t *c, const char *key, char *out, size_t outlen) {
    return lockdown_simple(c, "GetValue", "Key", key, "Value", out, outlen);
}
static int lockdown_start_session(conn_t *c, CFDictionaryRef pr, rp_tls_ctx *ctx) {
    char hostid[128] = {0}, buid[128] = {0};
    dict_get_cstr(pr, "HostID", hostid, sizeof hostid);
    dict_get_cstr(pr, "SystemBUID", buid, sizeof buid);
    CFMutableDictionaryRef req = dict_new();
    dict_set_str(req, "Request", "StartSession");
    dict_set_str(req, "HostID", hostid);
    dict_set_str(req, "SystemBUID", buid);
    CFDictionaryRef r = lockdown_request(c, req);
    CFRelease(req);
    if (!r) return -1;
    int want_ssl = dict_get_bool(r, "EnableSessionSSL");
    CFRelease(r);
    if (want_ssl && tls_upgrade(c, ctx) < 0) return -1;
    return 0;
}
static int lockdown_start_service(conn_t *c, const char *name, int *port_out, int *ssl_out) {
    CFMutableDictionaryRef req = dict_new();
    dict_set_str(req, "Request", "StartService");
    dict_set_str(req, "Service", name);
    CFDictionaryRef r = lockdown_request(c, req);
    CFRelease(req);
    if (!r) return -1;
    char err[128];
    if (dict_get_cstr(r, "Error", err, sizeof err) == 0) {
        fprintf(stderr, "StartService(%s) error: %s\n", name, err); CFRelease(r); return -1;
    }
    long long p = 0; CFNumberGetValue(dict_get(r, "Port"), kCFNumberLongLongType, &p);
    *port_out = (int)p;
    *ssl_out = dict_get_bool(r, "EnableServiceSSL");
    CFRelease(r);
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

static void *pump_host_to_device(void *arg) {
    pump_t *p = arg;
    uint8_t buf[70000];
    for (;;) {
        ssize_t n = read(p->utun, buf, sizeof buf);
        if (n <= 4) { p->reason = "utun read ended"; p->failed = 1; return NULL; }
        if (cwrite(p->tun, buf + 4, (size_t)(n - 4)) < 0) {   /* strip the 4-byte AF prefix */
            p->reason = "tunnel write failed"; p->failed = 1; return NULL;
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
            p->reason = "tunnel read ended"; p->failed = 1; return NULL;
        }
        uint16_t plen = ntohs(*(uint16_t *)(frame + 4 + 4));
        if (plen && cread_n(p->tun, frame + 4 + IPV6_HDR_LEN, plen) < 0) {
            p->reason = "tunnel read ended mid-packet"; p->failed = 1; return NULL;
        }
        memcpy(frame, &af, 4);
        if (write(p->utun, frame, 4 + IPV6_HDR_LEN + plen) < 0) {
            p->reason = "utun write failed"; p->failed = 1; return NULL;
        }
        p->rx++;
    }
}

/* Prove the routing works: an ordinary TCP connect to the device's RSD port. */
static int rsd_reachable(const char *device, long rsd_port) {
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
static long sock_read(void *ctx, void *buf, size_t len)
{
    return (long)recv(*(int *)ctx, buf, len, 0);
}

static long sock_write(void *ctx, const void *buf, size_t len)
{
    return (long)send(*(int *)ctx, buf, len, 0);
}

static int rsd_connect(const char *addr, long port)
{
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

    /* The three the mirroring product actually depends on. */
    static const char *want[] = {
        "com.apple.coredevice.screencaptureservice",
        "com.apple.coredevice.displayservice",
        "com.apple.coredevice.hid.universalhidservice",
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
                else out->hid_port = resolved;
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

void cdhost_rebind(const char *udid)
{
    if (!g_self[0] || !udid || !*udid) return;

    const char *features = getenv("RPLAY_HEVC_FEATURES");
    char *args[8];
    int n = 0;
    args[n++] = g_self;
    args[n++] = (char *)"--udid";
    args[n++] = (char *)udid;
    if (features && *features) {
        args[n++] = (char *)"--hevc-features";
        args[n++] = (char *)features;
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

int main(int argc, char **argv) {

    /* Flags rather than environment only, because sudo strips the environment.
     *
     * `RPLAY_UDID=... sudo ./cdhost` sets the variable for sudo, which then discards it, so the
     * daemon binds whatever it would have anyway -- silently, and three runs were lost to that
     * before anyone noticed. `sudo env VAR=... ./cdhost` works, but a flag cannot be got wrong.
     * The environment variables still work for anything already using them. */
    if (!realpath(argv[0], g_self)) snprintf(g_self, sizeof g_self, "%s", argv[0]);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--udid") && i + 1 < argc) {
            setenv("RPLAY_UDID", argv[++i], 1);
        } else if (!strcmp(argv[i], "--hevc-features") && i + 1 < argc) {
            setenv("RPLAY_HEVC_FEATURES", argv[++i], 1);
        } else if (!strcmp(argv[i], "--dump-services")) {
            setenv("RPLAY_DUMP_SERVICES", "1", 1);
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("usage: cdhost [--udid <prefix>] [--hevc-features <string>] [--dump-services]\n"
                   "\n"
                   "  --udid            which device to bind; a prefix is enough. Without it the\n"
                   "                    USB device wins, which is wrong when the phone you want\n"
                   "                    is the one on wifi.\n"
                   "  --hevc-features   override the offer's feature-list string. Try\n"
                   "                    \"FLS;VRA:0;MVRA:0;RVRA1:0;SW:1;\" to ask the encoder not\n"
                   "                    to adapt resolution -- see doc/RVRA-AND-PORTABILITY.md.\n"
                   "  --dump-services   print every service RSD advertises, with ports.\n");
            return 0;
        } else {
            fprintf(stderr, "unknown argument: %s (try --help)\n", argv[i]);
            return 2;
        }
    }

    if (getenv("RPLAY_UDID"))
        printf("binding device matching \"%s\"\n", getenv("RPLAY_UDID"));
    if (getenv("RPLAY_HEVC_FEATURES"))
        printf("HEVC features overridden: %s\n", getenv("RPLAY_HEVC_FEATURES"));

    printf("== Layer 0: usbmux ==\n");
    int mux = usbmux_connect();
    if (mux < 0) { fprintf(stderr, "cannot reach usbmuxd\n"); return 1; }
    char udid[128] = {0};
    int dev = usbmux_list_devices(mux, udid, sizeof udid);
    if (dev < 0) { fprintf(stderr, "no devices\n"); return 1; }
    printf("  -> using DeviceID=%d udid=%s\n", dev, udid);

    /* Warn when the chosen device cannot mirror, and name the alternatives.
     *
     * iOS 26 does not support screen viewing -- Apple's own Device Hub says so, and the device
     * simply answers startmediastream with nothing. Preferring USB then picks an iOS 26 phone
     * over an iOS 27 one attached at the same time, fails, and says only "media stream failed to
     * start", which reads as a bug in this daemon rather than a device limitation. Several
     * sessions were lost to exactly that. */
    {
        api_device all[API_MAX_DEVICES];
        int n = usbmux_enumerate(all, API_MAX_DEVICES);
        if (n > 1) {
            printf("  %d devices attached:\n", n);
            for (int i = 0; i < n; i++)
                printf("      %s  %-8s%s\n", all[i].udid, all[i].connection,
                       strcmp(all[i].udid, udid) ? "" : "   <- bound");
            printf("  pass --udid <prefix> to bind a different one\n");
        }
    }

    printf("\n== Layer 1: lockdown ==\n");
    conn_t lk = {mux, NULL};
    if (usbmux_connect_port(mux, dev, LOCKDOWN_PORT) < 0) { fprintf(stderr, "lockdown connect failed\n"); return 1; }
    char type[128];
    lockdown_simple(&lk, "QueryType", NULL, NULL, "Type", type, sizeof type);
    printf("  QueryType: %s\n", type);
    static char devname[256] = "?", prodver[64] = "?";
    const char *keys[] = {"DeviceName", "ProductType", "ProductVersion", "BuildVersion", "UniqueChipID"};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        char val[256] = "?"; lockdown_get_value(&lk, keys[i], val, sizeof val);
        printf("  %-15s = %s\n", keys[i], val);
        if (!strcmp(keys[i], "DeviceName")) snprintf(devname, sizeof devname, "%s", val);
        if (!strcmp(keys[i], "ProductVersion")) snprintf(prodver, sizeof prodver, "%s", val);
    }
    if (atoi(prodver) > 0 && atoi(prodver) < 27) {
        printf("\n  ⚠️  iOS %s cannot mirror. Screen viewing is unsupported before iOS 27 --\n"
               "      Apple's Device Hub reports the same, and the device answers\n"
               "      startmediastream with nothing. Everything else here still works;\n"
               "      the screen will stay black. Use --udid to bind an iOS 27 device.\n",
               prodver);
    }

    printf("\n== Layer 1.5: TLS session ==\n");
    CFDictionaryRef pr = usbmux_read_pair_record(udid);
    if (!pr) { fprintf(stderr, "no pair record\n"); return 1; }
    rp_tls_ctx *ctx = ctx_from_pairrecord(pr);
    if (!ctx || lockdown_start_session(&lk, pr, ctx) < 0) { fprintf(stderr, "session failed\n"); return 1; }
    printf("  session up, TLS=%s via %s\n", lk.tls ? "yes" : "no", rp_tls_backend());

    printf("\n== Layer 2: CoreDevice tunnel ==\n");
    int port = 0, ssl = 0;
    if (lockdown_start_service(&lk, COREDEVICE_PROXY, &port, &ssl) < 0) return 1;
    printf("  CoreDeviceProxy port=%d ssl=%d\n", port, ssl);

    int sfd = usbmux_connect();
    if (usbmux_connect_port(sfd, dev, port) < 0) { fprintf(stderr, "service connect failed\n"); return 1; }
    conn_t tun = {sfd, NULL};
    if (ssl && tls_upgrade(&tun, ctx) < 0) return 1;
    char addr[128] = "?", ours[128] = "?"; long rsd = -1, mtu = 0;
    if (tunnel_handshake_full(&tun, addr, sizeof addr, ours, sizeof ours, &rsd, &mtu) != 0) {
        fprintf(stderr, "  tunnel handshake failed\n");
        return 1;
    }
    printf("  handshake: us=%s device=%s serverRSDPort=%ld mtu=%ld\n", ours, addr, rsd, mtu);

    printf("\n== Layer 3a: utun + packet pump ==\n");
    if (geteuid() != 0) {
        printf("  not root — cannot create a utun. Re-run with sudo to bring the tunnel up:\n");
        printf("      sudo %s\n", "./cdhost");
        close(sfd); close(mux);
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

    sleep(1);
    if (rsd_reachable(addr, rsd) == 0)
        printf("  ✅ RSD reachable at [%s]:%ld with an ordinary socket — the tunnel is live\n",
               addr, rsd);
    else
        printf("  ✗ could not connect to [%s]:%ld (%s)\n", addr, rsd,
               pump.failed ? pump.reason : strerror(errno));

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

    while (!pump.failed) {
        sleep(2);
        printf("  pump: tx=%lu rx=%lu\n", pump.tx, pump.rx);
    }
    fprintf(stderr, "  tunnel died: %s\n", pump.reason ? pump.reason : "unknown");

    run_cmd("ifconfig %s destroy >/dev/null 2>&1", ifname);
    close(utun); close(sfd); close(mux);
    return 1;
}
