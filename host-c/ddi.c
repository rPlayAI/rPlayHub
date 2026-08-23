/* ddi.c -- see ddi.h. A direct C port of host/ddi_mount.py, which worked end to end on a rebooted
 * iPhone 13 (2026-08-23). Plists are built and parsed with CoreFoundation; the mounter is a
 * classic shim service (u32-be length + plist, after RSDCheckin); the TSS request goes to
 * gs.apple.com over OpenSSL HTTPS. */
#include "ddi.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

/* ------------------------------------------------------------------ CF helpers */

static CFStringRef cfstr(const char *s) { return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8); }

static void dict_set_str(CFMutableDictionaryRef d, const char *k, const char *v)
{ CFStringRef ks = cfstr(k), vs = cfstr(v); CFDictionarySetValue(d, ks, vs); CFRelease(ks); CFRelease(vs); }

static void dict_set_bool(CFMutableDictionaryRef d, const char *k, int v)
{ CFStringRef ks = cfstr(k); CFDictionarySetValue(d, ks, v ? kCFBooleanTrue : kCFBooleanFalse); CFRelease(ks); }

static void dict_set_int(CFMutableDictionaryRef d, const char *k, long long v)
{ CFStringRef ks = cfstr(k); CFNumberRef n = CFNumberCreate(NULL, kCFNumberLongLongType, &v);
  CFDictionarySetValue(d, ks, n); CFRelease(ks); CFRelease(n); }

static void dict_set_data(CFMutableDictionaryRef d, const char *k, const uint8_t *b, size_t n)
{ CFStringRef ks = cfstr(k); CFDataRef v = CFDataCreate(NULL, b, (CFIndex)n);
  CFDictionarySetValue(d, ks, v); CFRelease(ks); CFRelease(v); }

static void dict_set_obj(CFMutableDictionaryRef d, const char *k, CFTypeRef v)
{ CFStringRef ks = cfstr(k); CFDictionarySetValue(d, ks, v); CFRelease(ks); }

static CFTypeRef dget(CFDictionaryRef d, const char *k)
{ if (!d) return NULL; CFStringRef ks = cfstr(k); CFTypeRef v = CFDictionaryGetValue(d, ks); CFRelease(ks); return v; }

static int dget_str(CFDictionaryRef d, const char *k, char *out, size_t cap)
{ CFTypeRef v = dget(d, k); if (!v || CFGetTypeID(v) != CFStringGetTypeID()) return -1;
  return CFStringGetCString((CFStringRef)v, out, cap, kCFStringEncodingUTF8) ? 0 : -1; }

/* ------------------------------------------------------------------ framed plist over the tunnel */

static int recvn(int fd, uint8_t *buf, size_t n)
{ size_t g = 0; while (g < n) { ssize_t r = recv(fd, buf + g, n - g, 0); if (r <= 0) return -1; g += (size_t)r; } return 0; }

static int mounter_connect(const char *addr, long port)
{
    struct sockaddr_in6 sa;
    memset(&sa, 0, sizeof sa);
    sa.sin6_family = AF_INET6;
    sa.sin6_port = htons((uint16_t)port);
    if (inet_pton(AF_INET6, addr, &sa.sin6_addr) != 1) return -1;
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = { .tv_sec = 60, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return -1; }
    return fd;
}

static int send_plist(int fd, CFDictionaryRef d)
{
    CFDataRef body = CFPropertyListCreateData(NULL, d, kCFPropertyListXMLFormat_v1_0, 0, NULL);
    if (!body) return -1;
    CFIndex n = CFDataGetLength(body);
    uint8_t hdr[4] = { (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n };
    int rc = (send(fd, hdr, 4, 0) == 4 && send(fd, CFDataGetBytePtr(body), n, 0) == (ssize_t)n) ? 0 : -1;
    CFRelease(body);
    return rc;
}

static CFDictionaryRef recv_plist(int fd)
{
    uint8_t len[4];
    if (recvn(fd, len, 4) != 0) return NULL;
    size_t n = ((size_t)len[0] << 24) | ((size_t)len[1] << 16) | ((size_t)len[2] << 8) | len[3];
    if (n == 0 || n > (64u << 20)) return NULL;
    uint8_t *buf = malloc(n);
    if (!buf || recvn(fd, buf, n) != 0) { free(buf); return NULL; }
    CFDataRef data = CFDataCreateWithBytesNoCopy(NULL, buf, (CFIndex)n, kCFAllocatorNull);
    CFPropertyListRef pl = CFPropertyListCreateWithData(NULL, data, kCFPropertyListImmutable, NULL, NULL);
    CFRelease(data);
    free(buf);
    if (pl && CFGetTypeID(pl) != CFDictionaryGetTypeID()) { CFRelease(pl); return NULL; }
    return pl;
}

/* RSDCheckin preamble, same two exchanges the other shim services need. */
static int rsd_checkin(int fd)
{
    CFMutableDictionaryRef req = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    dict_set_str(req, "Label", "rplay-hub");
    dict_set_str(req, "ProtocolVersion", "2");
    dict_set_str(req, "Request", "RSDCheckin");
    int ok = send_plist(fd, req) == 0;
    CFRelease(req);
    if (!ok) return -1;
    CFDictionaryRef r = recv_plist(fd);
    if (!r) return -1;
    char v[32]; int good = dget_str(r, "Request", v, sizeof v) == 0 && !strcmp(v, "RSDCheckin");
    CFRelease(r);
    if (!good) return -1;
    r = recv_plist(fd);
    if (!r) return -1;
    good = dget_str(r, "Request", v, sizeof v) == 0 && !strcmp(v, "StartService");
    CFRelease(r);
    return good ? 0 : -1;
}

/* One command with keyword args already in `req` (Command is set by caller). Returns the reply. */
static CFDictionaryRef mounter_cmd(int fd, CFDictionaryRef req)
{
    if (send_plist(fd, req) != 0) return NULL;
    return recv_plist(fd);
}

static CFDictionaryRef simple_cmd(int fd, const char *command, const char *k2, const char *v2)
{
    CFMutableDictionaryRef req = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    dict_set_str(req, "Command", command);
    if (k2) dict_set_str(req, k2, v2);
    CFDictionaryRef r = mounter_cmd(fd, req);
    CFRelease(req);
    return r;
}

/* ------------------------------------------------------------------ mount status */

static int copy_devices_has_personalized(int fd)
{
    CFDictionaryRef r = simple_cmd(fd, "CopyDevices", NULL, NULL);
    if (!r) return 0;
    int found = 0;
    CFArrayRef list = dget(r, "EntryList");
    for (CFIndex i = 0; list && CFGetTypeID(list) == CFArrayGetTypeID() && i < CFArrayGetCount(list); i++) {
        CFDictionaryRef e = CFArrayGetValueAtIndex(list, i);
        char t[32];
        if (dget_str(e, "DiskImageType", t, sizeof t) == 0 && !strcmp(t, "Personalized")) { found = 1; break; }
    }
    CFRelease(r);
    return found;
}

int cdhost_ddi_is_mounted(const char *addr, long port)
{
    int fd = mounter_connect(addr, port);
    if (fd < 0) return 0;
    int m = rsd_checkin(fd) == 0 && copy_devices_has_personalized(fd);
    close(fd);
    return m;
}

/* ------------------------------------------------------------------ DDI files on disk */

static int read_file(const char *path, uint8_t **out, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return -1; }
    uint8_t *b = malloc((size_t)sz + 1);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return -1; }
    fclose(f);
    b[sz] = 0;
    *out = b; *n = (size_t)sz;
    return 0;
}

static CFDictionaryRef load_manifest(const char *ddi_dir)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/Restore/BuildManifest.plist", ddi_dir);
    uint8_t *b; size_t n;
    if (read_file(path, &b, &n) != 0) return NULL;
    CFDataRef data = CFDataCreateWithBytesNoCopy(NULL, b, (CFIndex)n, kCFAllocatorNull);
    CFPropertyListRef pl = CFPropertyListCreateWithData(NULL, data, kCFPropertyListImmutable, NULL, NULL);
    CFRelease(data);
    free(b);
    if (pl && CFGetTypeID(pl) != CFDictionaryGetTypeID()) { CFRelease(pl); return NULL; }
    return pl;
}

/* The BuildIdentity whose ApChipID/ApBoardID (hex strings) match the phone. */
static CFDictionaryRef pick_identity(CFDictionaryRef manifest, long chip, long board)
{
    CFArrayRef ids = dget(manifest, "BuildIdentities");
    for (CFIndex i = 0; ids && i < CFArrayGetCount(ids); i++) {
        CFDictionaryRef bi = CFArrayGetValueAtIndex(ids, i);
        char cs[16], bs[16];
        if (dget_str(bi, "ApChipID", cs, sizeof cs) == 0 && dget_str(bi, "ApBoardID", bs, sizeof bs) == 0
            && strtol(cs, NULL, 16) == chip && strtol(bs, NULL, 16) == board)
            return bi;
    }
    return NULL;
}

/* ------------------------------------------------------------------ TSS request (see host/ddi_mount.py) */

static void apply_restore_rules(CFMutableDictionaryRef e, CFArrayRef rules, int production, int security, int img4)
{
    for (CFIndex i = 0; i < CFArrayGetCount(rules); i++) {
        CFDictionaryRef rule = CFArrayGetValueAtIndex(rules, i);
        CFDictionaryRef cond = dget(rule, "Conditions");
        CFDictionaryRef acts = dget(rule, "Actions");
        if (!cond || !acts) continue;
        int ok = 1;
        CFIndex nc = CFDictionaryGetCount(cond);
        CFStringRef *ck = malloc(sizeof(CFStringRef) * nc); CFTypeRef *cv = malloc(sizeof(CFTypeRef) * nc);
        CFDictionaryGetKeysAndValues(cond, (const void **)ck, (const void **)cv);
        for (CFIndex j = 0; j < nc && ok; j++) {
            char key[64]; CFStringGetCString(ck[j], key, sizeof key, kCFStringEncodingUTF8);
            int actual;
            if (!strcmp(key, "ApRawProductionMode") || !strcmp(key, "ApCurrentProductionMode")) actual = production;
            else if (!strcmp(key, "ApRawSecurityMode")) actual = security;
            else if (!strcmp(key, "ApRequiresImage4")) actual = img4;
            else if (!strcmp(key, "ApInRomDFU")) actual = 0;
            else { ok = 0; break; }
            int want = (cv[j] == kCFBooleanTrue);
            if (actual != want) ok = 0;
        }
        free(ck); free(cv);
        if (!ok) continue;
        CFIndex na = CFDictionaryGetCount(acts);
        CFStringRef *ak = malloc(sizeof(CFStringRef) * na); CFTypeRef *av = malloc(sizeof(CFTypeRef) * na);
        CFDictionaryGetKeysAndValues(acts, (const void **)ak, (const void **)av);
        for (CFIndex j = 0; j < na; j++)
            if (av[j] == kCFBooleanTrue || av[j] == kCFBooleanFalse)
                CFDictionarySetValue(e, ak[j], av[j]);
        free(ak); free(av);
    }
}

static CFDictionaryRef build_tss_request(CFDictionaryRef bi, CFDictionaryRef ids, const uint8_t *nonce, size_t nlen)
{
    const int production = 1, security = 1, img4 = 1;
    CFMutableDictionaryRef req = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    dict_set_str(req, "@HostPlatformInfo", "mac");
    dict_set_str(req, "@VersionInfo", "libauthinstall-1049.100.23");
    dict_set_str(req, "@UUID", "00000000-0000-4000-8000-000000000000");

    /* Ap,* identifiers the device reports go in verbatim. */
    CFIndex ni = CFDictionaryGetCount(ids);
    CFStringRef *ik = malloc(sizeof(CFStringRef) * ni); CFTypeRef *iv = malloc(sizeof(CFTypeRef) * ni);
    CFDictionaryGetKeysAndValues(ids, (const void **)ik, (const void **)iv);
    for (CFIndex i = 0; i < ni; i++) {
        char k[64]; CFStringGetCString(ik[i], k, sizeof k, kCFStringEncodingUTF8);
        if (!strncmp(k, "Ap,", 3)) CFDictionarySetValue(req, ik[i], iv[i]);
    }
    free(ik); free(iv);

    /* One entry per manifest component (tss_request_add_ap_tags). */
    CFDictionaryRef manifest = dget(bi, "Manifest");
    CFIndex nm = manifest ? CFDictionaryGetCount(manifest) : 0;
    CFStringRef *mk = malloc(sizeof(CFStringRef) * nm); CFTypeRef *mv = malloc(sizeof(CFTypeRef) * nm);
    if (manifest) CFDictionaryGetKeysAndValues(manifest, (const void **)mk, (const void **)mv);
    for (CFIndex i = 0; i < nm; i++) {
        CFDictionaryRef entry = mv[i];
        CFDictionaryRef info = dget(entry, "Info");
        if (!info) continue;
        if (dget(info, "IsFTAB") == kCFBooleanTrue) continue;
        int trusted = (dget(entry, "Trusted") == kCFBooleanTrue);
        CFArrayRef rules = dget(info, "RestoreRequestRules");
        if (img4 && !rules && !trusted) continue;
        CFMutableDictionaryRef e = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFIndex ne = CFDictionaryGetCount(entry);
        CFStringRef *ek = malloc(sizeof(CFStringRef) * ne); CFTypeRef *ev = malloc(sizeof(CFTypeRef) * ne);
        CFDictionaryGetKeysAndValues(entry, (const void **)ek, (const void **)ev);
        for (CFIndex j = 0; j < ne; j++) {
            char k[64]; CFStringGetCString(ek[j], k, sizeof k, kCFStringEncodingUTF8);
            if (strcmp(k, "Info")) CFDictionarySetValue(e, ek[j], ev[j]);
        }
        free(ek); free(ev);
        if (rules) apply_restore_rules(e, rules, production, security, img4);
        else if (img4) { dict_set_bool(e, "EPRO", production); dict_set_bool(e, "ESEC", security); }
        if (trusted && !dget(entry, "Digest")) dict_set_data(e, "Digest", NULL, 0);
        if (CFDictionaryGetCount(e) > 0) CFDictionarySetValue(req, mk[i], e);
        CFRelease(e);
    }
    free(mk); free(mv);

    /* common + img4 tags */
    CFTypeRef ecid = dget(ids, "UniqueChipID"); if (ecid) dict_set_obj(req, "ApECID", ecid);
    CFTypeRef ubid = dget(bi, "UniqueBuildID"); if (ubid) dict_set_obj(req, "UniqueBuildID", ubid);
    char cs[16], bs[16];
    if (dget_str(bi, "ApChipID", cs, sizeof cs) == 0) dict_set_int(req, "ApChipID", strtol(cs, NULL, 16));
    if (dget_str(bi, "ApBoardID", bs, sizeof bs) == 0) dict_set_int(req, "ApBoardID", strtol(bs, NULL, 16));
    char ds[16]; if (dget_str(bi, "ApSecurityDomain", ds, sizeof ds) == 0) dict_set_int(req, "ApSecurityDomain", strtol(ds, NULL, 16));
    const char *img4_keys[] = { "Ap,OSLongVersion","Ap,OSReleaseType","Ap,ProductMarketingVersion","Ap,ProductType","Ap,SDKPlatform","Ap,Target","Ap,TargetType","Ap,Timestamp", NULL };
    for (int i = 0; img4_keys[i]; i++) { CFTypeRef v = dget(bi, img4_keys[i]); if (v) dict_set_obj(req, img4_keys[i], v); }
    dict_set_data(req, "ApNonce", nonce, nlen);
    dict_set_bool(req, "@ApImg4Ticket", 1);
    dict_set_bool(req, "ApSecurityMode", security);
    dict_set_bool(req, "ApProductionMode", production);
    static const uint8_t zero20[20] = {0};
    dict_set_data(req, "SepNonce", zero20, 20);
    CFTypeRef pearl = dget(bi, "PearlCertificationRootPub"); if (pearl) dict_set_obj(req, "PearlCertificationRootPub", pearl);
    dict_set_bool(req, "UID_MODE", 0);
    return req;
}

/* ------------------------------------------------------------------ HTTPS POST to gs.apple.com */

/* POST `body` to gs.apple.com/TSS/controller?action=2, return the response body (caller frees).
 * Server cert not verified -- same as libtatsu (CURLOPT_SSL_VERIFYPEER 0); the ticket is what is
 * trusted, and it is checked by the phone's kernel, not by us. */
static char *tss_post(const uint8_t *body, size_t blen, size_t *rlen)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo("gs.apple.com", "443", &hints, &res) != 0) return NULL;
    int fd = -1;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return NULL;

    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    SSL *ssl = ctx ? SSL_new(ctx) : NULL;
    if (!ssl) { if (ctx) SSL_CTX_free(ctx); close(fd); return NULL; }
    SSL_set_tlsext_host_name(ssl, "gs.apple.com");
    SSL_set_fd(ssl, fd);
    if (SSL_connect(ssl) != 1) { SSL_free(ssl); SSL_CTX_free(ctx); close(fd); return NULL; }

    char hdr[512];
    int hn = snprintf(hdr, sizeof hdr,
        "POST /TSS/controller?action=2 HTTP/1.1\r\nHost: gs.apple.com\r\n"
        "User-Agent: InetURL/1.0\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n"
        "Cache-Control: no-cache\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", blen);
    char *resp = NULL; size_t cap = 0, got = 0;
    int ok = SSL_write(ssl, hdr, hn) == hn && SSL_write(ssl, body, (int)blen) == (int)blen;
    while (ok) {
        if (got + 65536 + 1 > cap) { cap = cap ? cap * 2 : 1 << 17; char *np = realloc(resp, cap); if (!np) { ok = 0; break; } resp = np; }
        int r = SSL_read(ssl, resp + got, 65536);
        if (r <= 0) break;
        got += (size_t)r;
    }
    if (resp) resp[got] = 0;
    SSL_free(ssl); SSL_CTX_free(ctx); close(fd);
    if (!ok) { free(resp); return NULL; }
    *rlen = got;
    return resp;
}

/* The ticket, from the &-joined form after STATUS=0. Returns malloc'd DER, caller frees. */
static uint8_t *tss_extract_ticket(const char *resp, size_t rlen, size_t *tlen)
{
    (void)rlen;
    const char *body = strstr(resp, "\r\n\r\n");
    body = body ? body + 4 : resp;
    if (!strstr(body, "STATUS=0")) { fprintf(stderr, "  TSS refused: %.120s\n", body); return NULL; }
    const char *rs = strstr(body, "REQUEST_STRING=");
    if (!rs) return NULL;
    rs += strlen("REQUEST_STRING=");
    size_t xlen = strlen(rs);
    CFDataRef data = CFDataCreateWithBytesNoCopy(NULL, (const uint8_t *)rs, (CFIndex)xlen, kCFAllocatorNull);
    CFDictionaryRef pl = CFPropertyListCreateWithData(NULL, data, kCFPropertyListImmutable, NULL, NULL);
    CFRelease(data);
    if (!pl || CFGetTypeID(pl) != CFDictionaryGetTypeID()) { if (pl) CFRelease(pl); return NULL; }
    CFDataRef ticket = dget(pl, "ApImg4Ticket");
    uint8_t *out = NULL;
    if (ticket && CFGetTypeID(ticket) == CFDataGetTypeID()) {
        *tlen = (size_t)CFDataGetLength(ticket);
        out = malloc(*tlen);
        if (out) memcpy(out, CFDataGetBytePtr(ticket), *tlen);
    }
    CFRelease(pl);
    return out;
}

/* ------------------------------------------------------------------ the whole flow */

static const char *find_ddi_dir(const char *given, char *buf, size_t cap)
{
    const char *candidates[3]; int n = 0;
    if (given && *given) candidates[n++] = given;
    const char *env = getenv("RPLAY_DDI"); if (env && *env) candidates[n++] = env;
    candidates[n++] = "/Library/Developer/DeveloperDiskImages/iOS_DDI";
    for (int i = 0; i < n; i++) {
        char p[1200]; snprintf(p, sizeof p, "%s/Restore/BuildManifest.plist", candidates[i]);
        struct stat st;
        if (stat(p, &st) == 0) { snprintf(buf, cap, "%s", candidates[i]); return buf; }
    }
    return NULL;
}

int cdhost_ddi_activate(const char *addr, long port, const char *ddi_dir_in)
{
    if (!port) return RP_DDI_NO_SERVICE;

    char ddi_dir[1200];
    if (!find_ddi_dir(ddi_dir_in, ddi_dir, sizeof ddi_dir)) {
        fprintf(stderr, "  DDI files not found (set RPLAY_DDI to the iOS_DDI directory)\n");
        return RP_DDI_NO_DDI;
    }

    int fd = mounter_connect(addr, port);
    if (fd < 0) return RP_DDI_NO_SERVICE;
    if (rsd_checkin(fd) != 0) { close(fd); return RP_DDI_ERR; }

    if (copy_devices_has_personalized(fd)) { close(fd); return RP_DDI_ALREADY; }

    /* identity + nonce */
    CFDictionaryRef ir = simple_cmd(fd, "QueryPersonalizationIdentifiers", "PersonalizedImageType", "DeveloperDiskImage");
    CFDictionaryRef ids = ir ? dget(ir, "PersonalizationIdentifiers") : NULL;
    if (!ids) { if (ir) CFRelease(ir); close(fd); fprintf(stderr, "  QueryPersonalizationIdentifiers failed\n"); return RP_DDI_ERR; }
    long chip = 0, board = 0;
    { CFNumberRef v = (CFNumberRef)dget(ids, "ChipID"); if (v) CFNumberGetValue(v, kCFNumberLongType, &chip);
      v = (CFNumberRef)dget(ids, "BoardId"); if (v) CFNumberGetValue(v, kCFNumberLongType, &board); }

    CFDictionaryRef nr = simple_cmd(fd, "QueryNonce", "PersonalizedImageType", "DeveloperDiskImage");
    CFDataRef nonce = nr ? dget(nr, "PersonalizationNonce") : NULL;
    if (!nonce || CFGetTypeID(nonce) != CFDataGetTypeID()) { if (nr) CFRelease(nr); CFRelease(ir); close(fd); fprintf(stderr, "  QueryNonce failed\n"); return RP_DDI_ERR; }

    CFDictionaryRef manifest = load_manifest(ddi_dir);
    CFDictionaryRef bi = manifest ? pick_identity(manifest, chip, board) : NULL;
    if (!bi) { if (manifest) CFRelease(manifest); CFRelease(nr); CFRelease(ir); close(fd);
               fprintf(stderr, "  no BuildIdentity for chip %#lx board %#lx\n", chip, board); return RP_DDI_ERR; }

    /* ticket from Apple */
    fprintf(stderr, "  requesting a DDI ticket from Apple...\n");
    CFDictionaryRef tss = build_tss_request(bi, ids, CFDataGetBytePtr(nonce), (size_t)CFDataGetLength(nonce));
    CFDataRef tss_xml = CFPropertyListCreateData(NULL, tss, kCFPropertyListXMLFormat_v1_0, 0, NULL);
    size_t rlen = 0;
    char *resp = tss_xml ? tss_post(CFDataGetBytePtr(tss_xml), (size_t)CFDataGetLength(tss_xml), &rlen) : NULL;
    size_t tlen = 0;
    uint8_t *ticket = resp ? tss_extract_ticket(resp, rlen, &tlen) : NULL;
    if (tss_xml) CFRelease(tss_xml);
    CFRelease(tss);
    free(resp);
    if (!ticket) { CFRelease(manifest); CFRelease(nr); CFRelease(ir); close(fd); return RP_DDI_TSS_FAILED; }

    /* the .dmg and trust cache paths from the manifest */
    char dmg_rel[256] = "", tc_rel[256] = "";
    { CFDictionaryRef man = dget(bi, "Manifest");
      CFDictionaryRef pd = dget(man, "PersonalizedDMG"), lt = dget(man, "LoadableTrustCache");
      dget_str(dget(pd, "Info"), "Path", dmg_rel, sizeof dmg_rel);
      dget_str(dget(lt, "Info"), "Path", tc_rel, sizeof tc_rel); }
    char dmg_path[1400], tc_path[1400];
    snprintf(dmg_path, sizeof dmg_path, "%s/Restore/%s", ddi_dir, dmg_rel);
    snprintf(tc_path, sizeof tc_path, "%s/Restore/%s", ddi_dir, tc_rel);
    uint8_t *dmg = NULL, *tc = NULL; size_t dmg_n = 0, tc_n = 0;
    if (read_file(dmg_path, &dmg, &dmg_n) != 0 || read_file(tc_path, &tc, &tc_n) != 0) {
        free(ticket); free(dmg); free(tc); CFRelease(manifest); CFRelease(nr); CFRelease(ir); close(fd);
        fprintf(stderr, "  could not read the DDI image files\n"); return RP_DDI_NO_DDI;
    }

    /* ReceiveBytes -> upload -> MountImage */
    int rc = RP_DDI_ERR;
    CFMutableDictionaryRef rb = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    dict_set_str(rb, "Command", "ReceiveBytes");
    dict_set_str(rb, "ImageType", "Personalized");
    dict_set_int(rb, "ImageSize", (long long)dmg_n);
    dict_set_data(rb, "ImageSignature", ticket, tlen);
    CFDictionaryRef ack = mounter_cmd(fd, rb);
    CFRelease(rb);
    char st[32] = "";
    if (!ack || dget_str(ack, "Status", st, sizeof st) != 0 || strcmp(st, "ReceiveBytesAck") != 0) {
        /* A locked phone closes the connection here with no reply. */
        if (ack) CFRelease(ack);
        fprintf(stderr, "  ReceiveBytes refused -- is the phone unlocked?\n");
        rc = RP_DDI_LOCKED;
        goto done;
    }
    CFRelease(ack);
    if (send(fd, dmg, dmg_n, 0) != (ssize_t)dmg_n) { fprintf(stderr, "  upload failed\n"); goto done; }
    { CFDictionaryRef up = recv_plist(fd); char s2[32] = "";
      int good = up && dget_str(up, "Status", s2, sizeof s2) == 0 && !strcmp(s2, "Complete");
      if (up) CFRelease(up);
      if (!good) { fprintf(stderr, "  upload not acknowledged\n"); goto done; } }

    { CFMutableDictionaryRef mi = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
      dict_set_str(mi, "Command", "MountImage");
      dict_set_str(mi, "ImagePath", "/private/var/mobile/Media/PublicStaging/staging.dimage");
      dict_set_data(mi, "ImageSignature", ticket, tlen);
      dict_set_str(mi, "ImageType", "Personalized");
      dict_set_data(mi, "ImageTrustCache", tc, tc_n);
      CFDictionaryRef mr = mounter_cmd(fd, mi);
      CFRelease(mi);
      char s3[32] = "";
      int good = mr && dget_str(mr, "Status", s3, sizeof s3) == 0 && !strcmp(s3, "Complete");
      if (mr && !good) { char e[64] = ""; dget_str(mr, "Error", e, sizeof e); fprintf(stderr, "  MountImage failed: %s\n", e[0] ? e : "?"); }
      if (mr) CFRelease(mr);
      rc = good ? RP_DDI_OK : RP_DDI_ERR; }

done:
    free(ticket); free(dmg); free(tc);
    CFRelease(manifest); CFRelease(nr); CFRelease(ir);
    close(fd);
    if (rc == RP_DDI_OK) fprintf(stderr, "  DDI mounted -- developer services are live.\n");
    return rc;
}
