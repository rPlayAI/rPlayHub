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
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <stdarg.h>
#include <sys/kern_control.h>
#include <sys/sys_domain.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <pthread.h>
#include <signal.h>

#include "../core/rp_remotexpc.h"
#include "api_server.h"
#include "../core/rp_xpc.h"
#include "ddi.h"

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
static int usbmux_connect_port(int fd, int device_id, int port);
static int lockdown_get_value(conn_t *c, const char *key, char *out, size_t outlen);

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
        memset(&out[n], 0, sizeof out[n]);
        snprintf(out[n].udid, sizeof out[n].udid, "%s", ser);
        snprintf(out[n].connection, sizeof out[n].connection, "%s", conn[0] ? conn : "USB");
        long long did = 0;
        CFNumberRef idn = dict_get(dev, "DeviceID");
        if (idn) CFNumberGetValue(idn, kCFNumberLongLongType, &did);
        out[n].device_id = (int)did;
        n++;
    }
    CFRelease(reply);
    close(fd);

    /* Name every device, not just the bound one. DeviceName, ProductVersion and ProductType are
     * readable from lockdown before any session (Layer 1 reads them the same way), so each costs
     * one usbmux connection and three round trips. A device that does not answer -- unpaired,
     * or a wifi phone that has gone to sleep -- is listed by udid as before. Device Hub shows
     * names for all of them, and a row reading "DEVICE-UDID-REDACTED" was mistaken for a
     * missing device on the first side-by-side comparison (2026-08-23). */
    for (int i = 0; i < n; i++) {
        int lfd = usbmux_connect();
        if (lfd < 0) continue;
        struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
        setsockopt(lfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        if (usbmux_connect_port(lfd, out[i].device_id, LOCKDOWN_PORT) == 0) {
            conn_t lk = { lfd, NULL };
            lockdown_get_value(&lk, "DeviceName", out[i].name, sizeof out[i].name);
            lockdown_get_value(&lk, "ProductVersion", out[i].version, sizeof out[i].version);
            lockdown_get_value(&lk, "ProductType", out[i].product_type, sizeof out[i].product_type);
        }
        close(lfd);
    }
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
                else out->mounter_port = resolved;
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

// ============================ device_info (the Info tab, in C) ============================
//
// What host/deviceinfo.py did: a lockdown session, then GetValue for a fixed set of keys in the
// default, battery and disk_usage domains. Restricting the keys matters -- a whole-domain query
// returns dozens of internal keys that change between releases, and one unreadable key can fail
// the request rather than the key. The JSON shape is the script's, so the app's parser did not
// change. This removed the last Python in the runtime path (2026-08-23).

static void di_json_str(char **p, char *end, const char *s)
{
    if (*p < end) *(*p)++ = '"';
    for (; *s && *p + 6 < end; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') { *(*p)++ = '\\'; *(*p)++ = (char)ch; }
        else if (ch < 0x20) *p += snprintf(*p, (size_t)(end - *p), "\\u%04x", ch);
        else *(*p)++ = (char)ch;
    }
    if (*p < end) *(*p)++ = '"';
    **p = 0;
}

/* One lockdown value as JSON. Dates and data have no panel row and render as strings. */
static void json_cf(char **p, char *end, CFTypeRef v)
{
    char tmp[512];
    if (CFGetTypeID(v) == CFStringGetTypeID()) {
        CFStringGetCString((CFStringRef)v, tmp, sizeof tmp, kCFStringEncodingUTF8);
        di_json_str(p, end, tmp);
    } else if (CFGetTypeID(v) == CFBooleanGetTypeID()) {
        *p += snprintf(*p, (size_t)(end - *p), CFBooleanGetValue((CFBooleanRef)v) ? "true" : "false");
    } else if (CFGetTypeID(v) == CFNumberGetTypeID()) {
        if (CFNumberIsFloatType((CFNumberRef)v)) {
            double d = 0; CFNumberGetValue((CFNumberRef)v, kCFNumberDoubleType, &d);
            *p += snprintf(*p, (size_t)(end - *p), "%.3f", d);
        } else {
            long long n = 0; CFNumberGetValue((CFNumberRef)v, kCFNumberLongLongType, &n);
            *p += snprintf(*p, (size_t)(end - *p), "%lld", n);
        }
    } else {
        CFStringRef desc = CFCopyDescription(v);
        CFStringGetCString(desc, tmp, sizeof tmp, kCFStringEncodingUTF8);
        CFRelease(desc);
        di_json_str(p, end, tmp);
    }
}

int cdhost_device_info(const char *udid, char *json, size_t cap)
{
    static const char *default_keys[] = {
        "DeviceName", "ProductType", "ProductVersion", "BuildVersion", "ProductName",
        "UniqueDeviceID", "SerialNumber", "ModelNumber", "RegionInfo", "HardwareModel",
        "CPUArchitecture", "DeviceClass", "DeviceColor", "ChipID", "UniqueChipID",
        "ActivationState", "PasswordProtected", "TimeIntervalSince1970", "TimeZone",
        "WiFiAddress", "BluetoothAddress", "EthernetAddress", "FirmwareVersion", NULL };
    static const char *battery_keys[] = {
        "BatteryCurrentCapacity", "BatteryIsCharging", "ExternalConnected",
        "ExternalChargeCapable", NULL };
    static const char *disk_keys[] = {
        "TotalDiskCapacity", "TotalDataCapacity", "TotalDataAvailable", "TotalSystemCapacity",
        "TotalSystemAvailable", "AmountDataAvailable", "AmountDataReserved", NULL };
    struct { const char *domain; const char *label; const char **keys; } domains[] = {
        { NULL, "default", default_keys },
        { "com.apple.mobile.battery", "com.apple.mobile.battery", battery_keys },
        { "com.apple.disk_usage", "com.apple.disk_usage", disk_keys },
    };

    char *p = json, *end = json + cap - 1;
    *p = 0;

    /* Which usbmux handle is this udid, right now? Handles change when a phone reconnects. */
    api_device devs[API_MAX_DEVICES];
    int n = usbmux_enumerate(devs, API_MAX_DEVICES), dev = -1;
    for (int i = 0; i < n; i++) if (!strcmp(devs[i].udid, udid)) dev = devs[i].device_id;
    if (dev < 0) { snprintf(json, cap, "{\"error\":\"device %s is not attached\"}", udid); return -1; }

    int fd = usbmux_connect();
    if (fd < 0) { snprintf(json, cap, "{\"error\":\"usbmuxd is not reachable\"}"); return -1; }
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (usbmux_connect_port(fd, dev, LOCKDOWN_PORT) < 0) {
        close(fd);
        snprintf(json, cap, "{\"error\":\"lockdown connect failed\"}");
        return -1;
    }
    conn_t lk = { fd, NULL };

    /* Without a session only the default domain answers; battery and storage come back empty.
     * That is reported as session_error rather than treated as fatal, as the script did. */
    const char *session_error = NULL;
    CFDictionaryRef pr = usbmux_read_pair_record(udid);
    rp_tls_ctx *ctx = pr ? ctx_from_pairrecord(pr) : NULL;
    if (!pr) session_error = "no pair record (trust the Mac on the device first)";
    else if (!ctx) session_error = "pair record has no usable host certificate";
    else if (lockdown_start_session(&lk, pr, ctx) < 0) session_error = "StartSession refused";

    p += snprintf(p, (size_t)(end - p), "{");
    char unavailable[2048] = "";
    size_t ulen = 0;
    char product_type[64] = "";
    for (size_t d = 0; d < sizeof domains / sizeof domains[0]; d++) {
        if (d > 0 && session_error) break;
        int wrote = 0;
        for (const char **k = domains[d].keys; *k; k++) {
            CFMutableDictionaryRef req = dict_new();
            dict_set_str(req, "Request", "GetValue");
            dict_set_str(req, "Key", *k);
            if (domains[d].domain) dict_set_str(req, "Domain", domains[d].domain);
            CFDictionaryRef r = lockdown_request(&lk, req);
            CFRelease(req);
            CFTypeRef v = r ? dict_get(r, "Value") : NULL;
            if (!v) {
                char err[64] = "no value";
                if (r) dict_get_cstr(r, "Error", err, sizeof err);
                ulen += (size_t)snprintf(unavailable + ulen, sizeof unavailable - ulen,
                                         "%s\"%s.%s\":\"%s\"", ulen ? "," : "",
                                         domains[d].label, *k, err);
                if (r) CFRelease(r);
                if (!r) break;           /* the connection is gone; stop asking */
                continue;
            }
            if (!wrote) {
                p += snprintf(p, (size_t)(end - p), "%s", p[-1] == '{' ? "" : ",");
                di_json_str(&p, end, domains[d].label);
                p += snprintf(p, (size_t)(end - p), ":{");
            } else {
                p += snprintf(p, (size_t)(end - p), ",");
            }
            di_json_str(&p, end, *k);
            p += snprintf(p, (size_t)(end - p), ":");
            json_cf(&p, end, v);
            if (!strcmp(*k, "ProductType") && CFGetTypeID(v) == CFStringGetTypeID())
                CFStringGetCString((CFStringRef)v, product_type, sizeof product_type, kCFStringEncodingUTF8);
            if (!strcmp(*k, "UniqueChipID") && CFGetTypeID(v) == CFNumberGetTypeID()) {
                long long ecid = 0; CFNumberGetValue((CFNumberRef)v, kCFNumberLongLongType, &ecid);
                p += snprintf(p, (size_t)(end - p), ",\"ECID\":\"%llX\"", ecid);
            }
            wrote++;
            CFRelease(r);
        }
        if (wrote) p += snprintf(p, (size_t)(end - p), "}");
    }
    p += snprintf(p, (size_t)(end - p), "%s\"unavailable\":{%s}", p[-1] == '{' ? "" : ",", unavailable);
    if (session_error) {
        p += snprintf(p, (size_t)(end - p), ",\"session_error\":");
        di_json_str(&p, end, session_error);
    }
    p += snprintf(p, (size_t)(end - p), "}");

    if (lk.tls) rp_tls_close(lk.tls);
    close(fd);
    if (ctx) rp_tls_ctx_free(ctx);
    if (pr) CFRelease(pr);
    (void)product_type;
    return 0;
}

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
}

int main(int argc, char **argv) {
    own_signals();

    /* Flags rather than environment only, because sudo strips the environment.
     *
     * `RPLAY_UDID=... sudo ./cdhost` sets the variable for sudo, which then discards it, so the
     * daemon binds whatever it would have anyway -- silently, and three runs were lost to that
     * before anyone noticed. `sudo env VAR=... ./cdhost` works, but a flag cannot be got wrong.
     * The environment variables still work for anything already using them. */
    if (!realpath(argv[0], g_self)) snprintf(g_self, sizeof g_self, "%s", argv[0]);

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

    while (!pump.failed) {
        sleep(2);
        printf("  pump: tx=%lu rx=%lu\n", pump.tx, pump.rx);
    }
    fprintf(stderr, "  tunnel died: %s\n", pump.reason ? pump.reason : "unknown");

    run_cmd("ifconfig %s destroy >/dev/null 2>&1", ifname);
    close(utun); close(sfd); close(mux);
    return 1;
}
