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
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <stdio.h>
#include <stdlib.h>
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

#define USBMUXD_SOCKET "/var/run/usbmuxd"
#define LOCKDOWN_PORT 62078
#define USBMUX_TYPE_PLIST 8
#define COREDEVICE_PROXY "com.apple.internal.devicecompute.CoreDeviceProxy"
#define CDTUNNEL_MAGIC "CDTunnel"

// ============================ connection (raw fd or TLS) ============================
typedef struct { int fd; SSL *ssl; } conn_t;

static int cwrite(conn_t *c, const void *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t r = c->ssl ? SSL_write(c->ssl, (const char *)buf + off, (int)(n - off))
                           : send(c->fd, (const char *)buf + off, n - off, 0);
        if (r <= 0) return -1;
        off += (size_t)r;
    }
    return 0;
}
static int cread_n(conn_t *c, void *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t r = c->ssl ? SSL_read(c->ssl, (char *)buf + off, (int)(n - off))
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
    for (CFIndex i = 0; list && i < CFArrayGetCount(list); i++) {
        CFDictionaryRef dev = CFArrayGetValueAtIndex(list, i);
        CFDictionaryRef props = dict_get(dev, "Properties");
        char id[32] = "?", ser[128] = "?", conn[32] = "?";
        dict_get_cstr(dev, "DeviceID", id, sizeof id);
        dict_get_cstr(props, "SerialNumber", ser, sizeof ser);
        dict_get_cstr(props, "ConnectionType", conn, sizeof conn);
        printf("  DeviceID=%s udid=%s conn=%s\n", id, ser, conn);
        if (first_id < 0) {
            long long v = 0; CFNumberGetValue(dict_get(dev, "DeviceID"), kCFNumberLongLongType, &v);
            first_id = (int)v; strncpy(udid_out, ser, udid_len - 1);
        }
    }
    CFRelease(reply);
    return first_id;
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
static SSL_CTX *ctx_from_pairrecord(CFDictionaryRef pr) {
    CFIndex clen = 0, klen = 0;
    const uint8_t *cert = dict_get_bytes(pr, "HostCertificate", &clen);
    const uint8_t *key = dict_get_bytes(pr, "HostPrivateKey", &klen);
    if (!cert || !key) { fprintf(stderr, "pair record missing host cert/key\n"); return NULL; }

    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_min_proto_version(ctx, TLS1_VERSION);   // lockdown speaks old TLS
    SSL_CTX_set_security_level(ctx, 0);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);

    BIO *cb = BIO_new_mem_buf(cert, (int)clen);
    X509 *x = PEM_read_bio_X509(cb, NULL, NULL, NULL);
    BIO *kb = BIO_new_mem_buf(key, (int)klen);
    EVP_PKEY *pk = PEM_read_bio_PrivateKey(kb, NULL, NULL, NULL);
    if (!x || !pk || SSL_CTX_use_certificate(ctx, x) != 1 || SSL_CTX_use_PrivateKey(ctx, pk) != 1) {
        fprintf(stderr, "failed to load host identity into SSL_CTX\n");
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx); ctx = NULL;
    }
    if (x) X509_free(x);
    if (pk) EVP_PKEY_free(pk);
    BIO_free(cb); BIO_free(kb);
    return ctx;
}
static int tls_upgrade(conn_t *c, SSL_CTX *ctx) {
    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, c->fd);
    if (SSL_connect(ssl) != 1) {
        fprintf(stderr, "SSL_connect failed\n"); ERR_print_errors_fp(stderr);
        SSL_free(ssl); return -1;
    }
    c->ssl = ssl;
    return 0;
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
static int lockdown_start_session(conn_t *c, CFDictionaryRef pr, SSL_CTX *ctx) {
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

int main(void) {
    SSL_library_init();

    printf("== Layer 0: usbmux ==\n");
    int mux = usbmux_connect();
    if (mux < 0) { fprintf(stderr, "cannot reach usbmuxd\n"); return 1; }
    char udid[128] = {0};
    int dev = usbmux_list_devices(mux, udid, sizeof udid);
    if (dev < 0) { fprintf(stderr, "no devices\n"); return 1; }
    printf("  -> using DeviceID=%d udid=%s\n", dev, udid);

    printf("\n== Layer 1: lockdown ==\n");
    conn_t lk = {mux, NULL};
    if (usbmux_connect_port(mux, dev, LOCKDOWN_PORT) < 0) { fprintf(stderr, "lockdown connect failed\n"); return 1; }
    char type[128];
    lockdown_simple(&lk, "QueryType", NULL, NULL, "Type", type, sizeof type);
    printf("  QueryType: %s\n", type);
    const char *keys[] = {"DeviceName", "ProductType", "ProductVersion", "BuildVersion", "UniqueChipID"};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        char val[256] = "?"; lockdown_get_value(&lk, keys[i], val, sizeof val);
        printf("  %-15s = %s\n", keys[i], val);
    }

    printf("\n== Layer 1.5: TLS session ==\n");
    CFDictionaryRef pr = usbmux_read_pair_record(udid);
    if (!pr) { fprintf(stderr, "no pair record\n"); return 1; }
    SSL_CTX *ctx = ctx_from_pairrecord(pr);
    if (!ctx || lockdown_start_session(&lk, pr, ctx) < 0) { fprintf(stderr, "session failed\n"); return 1; }
    printf("  session up, TLS=%s\n", lk.ssl ? "yes" : "no");

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

    printf("\n  Ctrl-C to tear down. tx/rx packets shown every 2s.\n");
    while (!pump.failed) {
        sleep(2);
        printf("  pump: tx=%lu rx=%lu\n", pump.tx, pump.rx);
    }
    fprintf(stderr, "  tunnel died: %s\n", pump.reason ? pump.reason : "unknown");

    run_cmd("ifconfig %s destroy >/dev/null 2>&1", ifname);
    close(utun); close(sfd); close(mux);
    return 1;
}
