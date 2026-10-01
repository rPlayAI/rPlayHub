/* ddi.c -- see ddi.h. A direct C port of host/ddi_mount.py, which worked end to end on a rebooted
 * iPhone 13 (2026-08-23). Plists via libplist (migrated off CoreFoundation 2026-08-24); the mounter
 * is a classic shim service (u32-be length + plist, after RSDCheckin); the TSS request goes to
 * gs.apple.com over OpenSSL HTTPS. The TSS request build stays byte-identical to the Python (and to
 * the pre-migration CoreFoundation version) -- verified by diff. */
#include "ddi.h"

#include "compat.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <plist/plist.h>
#include "usernet.h"
#include <openssl/ssl.h>
#include <openssl/err.h>

/* ------------------------------------------------------------------ plist helpers */

static void dset_str(plist_t d, const char *k, const char *v) { plist_dict_set_item(d, k, plist_new_string(v)); }
static void dset_bool(plist_t d, const char *k, int v) { plist_dict_set_item(d, k, plist_new_bool(v)); }
static void dset_int(plist_t d, const char *k, long long v) { plist_dict_set_item(d, k, plist_new_uint((uint64_t)v)); }
static void dset_data(plist_t d, const char *k, const uint8_t *b, size_t n) { plist_dict_set_item(d, k, plist_new_data((const char *)b, n)); }
/* Put a COPY of a borrowed node (owned by another plist) into d. */
static void dset_copy(plist_t d, const char *k, plist_t v) { plist_dict_set_item(d, k, plist_copy(v)); }

static plist_t dget(plist_t d, const char *k) { return d ? plist_dict_get_item(d, k) : NULL; }

static int dget_str(plist_t d, const char *k, char *out, size_t cap)
{
    plist_t v = dget(d, k);
    if (!v || plist_get_node_type(v) != PLIST_STRING) return -1;
    char *s = NULL;
    plist_get_string_val(v, &s);
    if (!s) return -1;
    snprintf(out, cap, "%s", s);
    free(s);
    return 0;
}

/* ------------------------------------------------------------------ framed plist over the tunnel */

static int recvn(int fd, uint8_t *buf, size_t n)
{ size_t g = 0; while (g < n) { long r = tun_read(fd, buf + g, n - g); if (r <= 0) return -1; g += (size_t)r; } return 0; }

static int write_all(int fd, const void *buf, size_t n)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < n) {
        long w = tun_write(fd, p + sent, n - sent);
        if (w <= 0) return -1;
        sent += (size_t)w;
    }
    return 0;
}

static int mounter_connect(const char *addr, long port)
{
    return tun_connect(addr, (int)port, 15);   /* tunnel: kernel socket or lwIP per userspace mode */
}

static int send_plist(int fd, plist_t d)
{
    char *xml = NULL;
    uint32_t xlen = 0;
    plist_to_xml(d, &xml, &xlen);
    if (!xml) return -1;
    uint8_t hdr[4] = { (uint8_t)(xlen >> 24), (uint8_t)(xlen >> 16), (uint8_t)(xlen >> 8), (uint8_t)xlen };
    int rc = (write_all(fd, hdr, 4) == 0 && write_all(fd, xml, xlen) == 0) ? 0 : -1;
    plist_mem_free(xml);
    return rc;
}

static plist_t recv_plist(int fd)
{
    uint8_t len[4];
    if (recvn(fd, len, 4) != 0) return NULL;
    size_t n = ((size_t)len[0] << 24) | ((size_t)len[1] << 16) | ((size_t)len[2] << 8) | len[3];
    if (n == 0 || n > (64u << 20)) return NULL;
    uint8_t *buf = malloc(n);
    if (!buf || recvn(fd, buf, n) != 0) { free(buf); return NULL; }
    plist_t pl = NULL;
    plist_from_memory((const char *)buf, (uint32_t)n, &pl, NULL);
    free(buf);
    if (pl && plist_get_node_type(pl) != PLIST_DICT) { plist_free(pl); return NULL; }
    return pl;
}

static int rsd_checkin(int fd)
{
    plist_t req = plist_new_dict();
    dset_str(req, "Label", "rplay-hub");
    dset_str(req, "ProtocolVersion", "2");
    dset_str(req, "Request", "RSDCheckin");
    int ok = send_plist(fd, req) == 0;
    plist_free(req);
    if (!ok) { fprintf(stderr, "  rsd_checkin: send_plist failed\n"); return -1; }
    plist_t r = recv_plist(fd);
    if (!r) { fprintf(stderr, "  rsd_checkin: recv_plist 1 failed\n"); return -1; }
    char v[32] = ""; int good = dget_str(r, "Request", v, sizeof v) == 0 && !strcmp(v, "RSDCheckin");
    if (!good) { fprintf(stderr, "  rsd_checkin: expected RSDCheckin, got '%s'\n", v); }
    plist_free(r);
    if (!good) return -1;
    r = recv_plist(fd);
    if (!r) { fprintf(stderr, "  rsd_checkin: recv_plist 2 failed\n"); return -1; }
    v[0] = 0;
    good = dget_str(r, "Request", v, sizeof v) == 0 && !strcmp(v, "StartService");
    if (!good) { fprintf(stderr, "  rsd_checkin: expected StartService, got '%s'\n", v); }
    plist_free(r);
    return good ? 0 : -1;
}

static plist_t mounter_cmd(int fd, plist_t req)
{
    if (send_plist(fd, req) != 0) return NULL;
    return recv_plist(fd);
}

static plist_t simple_cmd(int fd, const char *command, const char *k2, const char *v2)
{
    plist_t req = plist_new_dict();
    dset_str(req, "Command", command);
    if (k2) dset_str(req, k2, v2);
    plist_t r = mounter_cmd(fd, req);
    plist_free(req);
    return r;
}

/* ------------------------------------------------------------------ mount status */

static int copy_devices_has_personalized(int fd)
{
    plist_t r = simple_cmd(fd, "CopyDevices", NULL, NULL);
    if (!r) return 0;
    int found = 0;
    plist_t list = dget(r, "EntryList");
    for (uint32_t i = 0; list && plist_get_node_type(list) == PLIST_ARRAY && i < plist_array_get_size(list); i++) {
        plist_t e = plist_array_get_item(list, i);
        char t[32];
        if (dget_str(e, "DiskImageType", t, sizeof t) == 0 && !strcmp(t, "Personalized")) { found = 1; break; }
    }
    plist_free(r);
    return found;
}

int cdhost_ddi_is_mounted(const char *addr, long port)
{
    int fd = mounter_connect(addr, port);
    if (fd < 0) return 0;
    int m = rsd_checkin(fd) == 0 && copy_devices_has_personalized(fd);
    tun_close(fd);
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

static plist_t load_manifest(const char *ddi_dir)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/Restore/BuildManifest.plist", ddi_dir);
    uint8_t *b; size_t n;
    if (read_file(path, &b, &n) != 0) return NULL;
    plist_t pl = NULL;
    plist_from_memory((const char *)b, (uint32_t)n, &pl, NULL);
    free(b);
    if (pl && plist_get_node_type(pl) != PLIST_DICT) { plist_free(pl); return NULL; }
    return pl;
}

/* The BuildIdentity whose ApChipID/ApBoardID (hex strings) match the phone. Borrowed reference. */
static plist_t pick_identity(plist_t manifest, long chip, long board)
{
    plist_t ids = dget(manifest, "BuildIdentities");
    for (uint32_t i = 0; ids && plist_get_node_type(ids) == PLIST_ARRAY && i < plist_array_get_size(ids); i++) {
        plist_t bi = plist_array_get_item(ids, i);
        char cs[16], bs[16];
        if (dget_str(bi, "ApChipID", cs, sizeof cs) == 0 && dget_str(bi, "ApBoardID", bs, sizeof bs) == 0
            && strtol(cs, NULL, 16) == chip && strtol(bs, NULL, 16) == board)
            return bi;
    }
    return NULL;
}

/* ------------------------------------------------------------------ TSS request (see host/ddi_mount.py) */

static int node_is_true(plist_t v) { uint8_t b = 0; if (v && plist_get_node_type(v) == PLIST_BOOLEAN) plist_get_bool_val(v, &b); return b; }

static void apply_restore_rules(plist_t e, plist_t rules, int production, int security, int img4)
{
    for (uint32_t i = 0; i < plist_array_get_size(rules); i++) {
        plist_t rule = plist_array_get_item(rules, i);
        plist_t cond = dget(rule, "Conditions");
        plist_t acts = dget(rule, "Actions");
        if (!cond || !acts) continue;
        int ok = 1;
        plist_dict_iter it = NULL;
        plist_dict_new_iter(cond, &it);
        char *key = NULL;
        plist_t cval = NULL;
        for (;;) {
            plist_dict_next_item(cond, it, &key, &cval);
            if (!key) break;
            int actual;
            if (!strcmp(key, "ApRawProductionMode") || !strcmp(key, "ApCurrentProductionMode")) actual = production;
            else if (!strcmp(key, "ApRawSecurityMode")) actual = security;
            else if (!strcmp(key, "ApRequiresImage4")) actual = img4;
            else if (!strcmp(key, "ApInRomDFU")) actual = 0;
            else { ok = 0; free(key); key = NULL; break; }
            int want = node_is_true(cval);
            free(key); key = NULL;
            if (actual != want) { ok = 0; break; }
        }
        free(it);
        if (!ok) continue;
        plist_dict_iter ait = NULL;
        plist_dict_new_iter(acts, &ait);
        char *akey = NULL;
        plist_t aval = NULL;
        for (;;) {
            plist_dict_next_item(acts, ait, &akey, &aval);
            if (!akey) break;
            if (aval && plist_get_node_type(aval) == PLIST_BOOLEAN)
                plist_dict_set_item(e, akey, plist_copy(aval));
            free(akey); akey = NULL;
        }
        free(ait);
    }
}

static plist_t build_tss_request(plist_t bi, plist_t ids, const uint8_t *nonce, size_t nlen)
{
    const int production = 1, security = 1, img4 = 1;
    plist_t req = plist_new_dict();
    dset_str(req, "@HostPlatformInfo", "mac");
    dset_str(req, "@VersionInfo", "libauthinstall-1049.100.23");
    dset_str(req, "@UUID", "00000000-0000-4000-8000-000000000000");

    /* Ap,* identifiers the device reports go in verbatim. */
    plist_dict_iter iit = NULL;
    plist_dict_new_iter(ids, &iit);
    char *ik = NULL;
    plist_t iv = NULL;
    for (;;) {
        plist_dict_next_item(ids, iit, &ik, &iv);
        if (!ik) break;
        if (!strncmp(ik, "Ap,", 3)) plist_dict_set_item(req, ik, plist_copy(iv));
        free(ik); ik = NULL;
    }
    free(iit);

    /* One entry per manifest component (tss_request_add_ap_tags). */
    plist_t manifest = dget(bi, "Manifest");
    plist_dict_iter mit = NULL;
    if (manifest) plist_dict_new_iter(manifest, &mit);
    char *mk = NULL;
    plist_t entry = NULL;
    while (manifest) {
        plist_dict_next_item(manifest, mit, &mk, &entry);
        if (!mk) break;
        plist_t info = dget(entry, "Info");
        if (!info) { free(mk); mk = NULL; continue; }
        if (node_is_true(dget(info, "IsFTAB"))) { free(mk); mk = NULL; continue; }
        int trusted = node_is_true(dget(entry, "Trusted"));
        plist_t rules = dget(info, "RestoreRequestRules");
        if (img4 && !rules && !trusted) { free(mk); mk = NULL; continue; }
        /* Copy the entry minus Info. */
        plist_t e = plist_new_dict();
        plist_dict_iter eit = NULL;
        plist_dict_new_iter(entry, &eit);
        char *ek = NULL;
        plist_t ev = NULL;
        for (;;) {
            plist_dict_next_item(entry, eit, &ek, &ev);
            if (!ek) break;
            if (strcmp(ek, "Info")) plist_dict_set_item(e, ek, plist_copy(ev));
            free(ek); ek = NULL;
        }
        free(eit);
        if (rules) apply_restore_rules(e, rules, production, security, img4);
        else if (img4) { dset_bool(e, "EPRO", production); dset_bool(e, "ESEC", security); }
        if (trusted && !dget(entry, "Digest")) dset_data(e, "Digest", NULL, 0);
        if (plist_dict_get_size(e) > 0) plist_dict_set_item(req, mk, e);
        else plist_free(e);
        free(mk); mk = NULL;
    }
    free(mit);

    /* common + img4 tags */
    plist_t ecid = dget(ids, "UniqueChipID"); if (ecid) dset_copy(req, "ApECID", ecid);
    plist_t ubid = dget(bi, "UniqueBuildID"); if (ubid) dset_copy(req, "UniqueBuildID", ubid);
    char cs[16], bs[16];
    if (dget_str(bi, "ApChipID", cs, sizeof cs) == 0) dset_int(req, "ApChipID", strtol(cs, NULL, 16));
    if (dget_str(bi, "ApBoardID", bs, sizeof bs) == 0) dset_int(req, "ApBoardID", strtol(bs, NULL, 16));
    char ds[16]; if (dget_str(bi, "ApSecurityDomain", ds, sizeof ds) == 0) dset_int(req, "ApSecurityDomain", strtol(ds, NULL, 16));
    const char *img4_keys[] = { "Ap,OSLongVersion", "Ap,OSReleaseType", "Ap,ProductMarketingVersion", "Ap,ProductType", "Ap,SDKPlatform", "Ap,Target", "Ap,TargetType", "Ap,Timestamp", NULL };
    for (int i = 0; img4_keys[i]; i++) { plist_t v = dget(bi, img4_keys[i]); if (v) dset_copy(req, img4_keys[i], v); }
    dset_data(req, "ApNonce", nonce, nlen);
    dset_bool(req, "@ApImg4Ticket", 1);
    dset_bool(req, "ApSecurityMode", security);
    dset_bool(req, "ApProductionMode", production);
    static const uint8_t zero20[20] = {0};
    dset_data(req, "SepNonce", zero20, 20);
    plist_t pearl = dget(bi, "PearlCertificationRootPub"); if (pearl) dset_copy(req, "PearlCertificationRootPub", pearl);
    dset_bool(req, "UID_MODE", 0);
    return req;
}

/* ------------------------------------------------------------------ HTTPS POST to gs.apple.com */

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
#ifdef _WIN32
        DWORD ms = 30000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof ms);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof ms);
#else
        struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
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

static uint8_t *tss_extract_ticket(const char *resp, size_t rlen, size_t *tlen)
{
    (void)rlen;
    const char *body = strstr(resp, "\r\n\r\n");
    body = body ? body + 4 : resp;
    if (!strstr(body, "STATUS=0")) { fprintf(stderr, "  TSS refused: %.120s\n", body); return NULL; }
    const char *rs = strstr(body, "REQUEST_STRING=");
    if (!rs) return NULL;
    rs += strlen("REQUEST_STRING=");
    plist_t pl = NULL;
    plist_from_memory(rs, (uint32_t)strlen(rs), &pl, NULL);
    if (!pl || plist_get_node_type(pl) != PLIST_DICT) { if (pl) plist_free(pl); return NULL; }
    plist_t ticket = dget(pl, "ApImg4Ticket");
    uint8_t *out = NULL;
    if (ticket && plist_get_node_type(ticket) == PLIST_DATA) {
        uint64_t dl = 0;
        const char *bytes = plist_get_data_ptr(ticket, &dl);
        if (bytes) { *tlen = (size_t)dl; out = malloc(*tlen); if (out) memcpy(out, bytes, *tlen); }
    }
    plist_free(pl);
    return out;
}

/* ------------------------------------------------------------------ the whole flow */

static const char *find_ddi_dir(const char *given, char *buf, size_t cap)
{
    const char *candidates[4]; int n = 0;
    if (given && *given) candidates[n++] = given;
    const char *env = getenv("RPLAY_DDI"); if (env && *env) candidates[n++] = env;
    /* Where scripts/fetch-ddi.sh puts it -- the way to get one without Xcode, e.g. on Linux. */
    char fetched[1024] = "";
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(fetched, sizeof fetched, "%s/rplayhub/iOS_DDI", xdg);
    else if (home && *home) snprintf(fetched, sizeof fetched, "%s/.local/share/rplayhub/iOS_DDI", home);
    if (fetched[0]) candidates[n++] = fetched;
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
        fprintf(stderr, "  DDI files not found: run scripts/fetch-ddi.sh, or set RPLAY_DDI to an iOS_DDI directory\n");
        return RP_DDI_NO_DDI;
    }

    int fd = mounter_connect(addr, port);
    if (fd < 0) return RP_DDI_NO_SERVICE;
    if (rsd_checkin(fd) != 0) { tun_close(fd); return RP_DDI_ERR; }

    if (copy_devices_has_personalized(fd)) { tun_close(fd); return RP_DDI_ALREADY; }

    /* identity + nonce */
    plist_t ir = simple_cmd(fd, "QueryPersonalizationIdentifiers", "PersonalizedImageType", "DeveloperDiskImage");
    plist_t ids = ir ? dget(ir, "PersonalizationIdentifiers") : NULL;
    if (!ids) { if (ir) plist_free(ir); tun_close(fd); fprintf(stderr, "  QueryPersonalizationIdentifiers failed\n"); return RP_DDI_ERR; }
    long chip = 0, board = 0;
    { plist_t v = dget(ids, "ChipID"); if (v) { uint64_t u = 0; plist_get_uint_val(v, &u); chip = (long)u; }
      v = dget(ids, "BoardId"); if (v) { uint64_t u = 0; plist_get_uint_val(v, &u); board = (long)u; } }

    plist_t nr = simple_cmd(fd, "QueryNonce", "PersonalizedImageType", "DeveloperDiskImage");
    plist_t nonce = nr ? dget(nr, "PersonalizationNonce") : NULL;
    if (!nonce || plist_get_node_type(nonce) != PLIST_DATA) { if (nr) plist_free(nr); plist_free(ir); tun_close(fd); fprintf(stderr, "  QueryNonce failed\n"); return RP_DDI_ERR; }
    uint64_t nonce_len = 0;
    const char *nonce_bytes = plist_get_data_ptr(nonce, &nonce_len);

    plist_t manifest = load_manifest(ddi_dir);
    plist_t bi = manifest ? pick_identity(manifest, chip, board) : NULL;
    if (!bi) { if (manifest) plist_free(manifest); plist_free(nr); plist_free(ir); tun_close(fd);
               fprintf(stderr, "  no BuildIdentity for chip %#lx board %#lx\n", chip, board); return RP_DDI_ERR; }

    /* ticket from Apple */
    fprintf(stderr, "  requesting a DDI ticket from Apple...\n");
    plist_t tss = build_tss_request(bi, ids, (const uint8_t *)nonce_bytes, (size_t)nonce_len);
    char *tss_xml = NULL; uint32_t tss_len = 0;
    plist_to_xml(tss, &tss_xml, &tss_len);
    size_t rlen = 0;
    char *resp = tss_xml ? tss_post((const uint8_t *)tss_xml, (size_t)tss_len, &rlen) : NULL;
    size_t tlen = 0;
    uint8_t *ticket = resp ? tss_extract_ticket(resp, rlen, &tlen) : NULL;
    if (tss_xml) plist_mem_free(tss_xml);
    plist_free(tss);
    free(resp);
    if (!ticket) { plist_free(manifest); plist_free(nr); plist_free(ir); tun_close(fd); return RP_DDI_TSS_FAILED; }

    /* the .dmg and trust cache paths from the manifest */
    char dmg_rel[256] = "", tc_rel[256] = "";
    { plist_t man = dget(bi, "Manifest");
      plist_t pd = dget(man, "PersonalizedDMG"), lt = dget(man, "LoadableTrustCache");
      dget_str(dget(pd, "Info"), "Path", dmg_rel, sizeof dmg_rel);
      dget_str(dget(lt, "Info"), "Path", tc_rel, sizeof tc_rel); }
    char dmg_path[1400], tc_path[1400];
    snprintf(dmg_path, sizeof dmg_path, "%s/Restore/%s", ddi_dir, dmg_rel);
    snprintf(tc_path, sizeof tc_path, "%s/Restore/%s", ddi_dir, tc_rel);
    uint8_t *dmg = NULL, *tc = NULL; size_t dmg_n = 0, tc_n = 0;
    if (read_file(dmg_path, &dmg, &dmg_n) != 0 || read_file(tc_path, &tc, &tc_n) != 0) {
        free(ticket); free(dmg); free(tc); plist_free(manifest); plist_free(nr); plist_free(ir); tun_close(fd);
        fprintf(stderr, "  could not read the DDI image files\n"); return RP_DDI_NO_DDI;
    }

    /* ReceiveBytes -> upload -> MountImage */
    int rc = RP_DDI_ERR;
    plist_t rb = plist_new_dict();
    dset_str(rb, "Command", "ReceiveBytes");
    dset_str(rb, "ImageType", "Personalized");
    dset_int(rb, "ImageSize", (long long)dmg_n);
    dset_data(rb, "ImageSignature", ticket, tlen);
    plist_t ack = mounter_cmd(fd, rb);
    plist_free(rb);
    char st[32] = "";
    if (!ack || dget_str(ack, "Status", st, sizeof st) != 0 || strcmp(st, "ReceiveBytesAck") != 0) {
        if (ack) plist_free(ack);
        fprintf(stderr, "  ReceiveBytes refused -- is the phone unlocked?\n");
        rc = RP_DDI_LOCKED;
        goto done;
    }
    plist_free(ack);
    if (write_all(fd, dmg, dmg_n) != 0) { fprintf(stderr, "  upload failed\n"); goto done; }
    { plist_t up = recv_plist(fd); char s2[32] = "";
      int good = up && dget_str(up, "Status", s2, sizeof s2) == 0 && !strcmp(s2, "Complete");
      if (up) plist_free(up);
      if (!good) { fprintf(stderr, "  upload not acknowledged\n"); goto done; } }

    { plist_t mi = plist_new_dict();
      dset_str(mi, "Command", "MountImage");
      dset_str(mi, "ImagePath", "/private/var/mobile/Media/PublicStaging/staging.dimage");
      dset_data(mi, "ImageSignature", ticket, tlen);
      dset_str(mi, "ImageType", "Personalized");
      dset_data(mi, "ImageTrustCache", tc, tc_n);
      plist_t mr = mounter_cmd(fd, mi);
      plist_free(mi);
      char s3[32] = "";
      int good = mr && dget_str(mr, "Status", s3, sizeof s3) == 0 && !strcmp(s3, "Complete");
      if (mr && !good) { char e[64] = ""; dget_str(mr, "Error", e, sizeof e); fprintf(stderr, "  MountImage failed: %s\n", e[0] ? e : "?"); }
      if (mr) plist_free(mr);
      rc = good ? RP_DDI_OK : RP_DDI_ERR; }

done:
    free(ticket); free(dmg); free(tc);
    plist_free(manifest); plist_free(nr); plist_free(ir);
    tun_close(fd);
    if (rc == RP_DDI_OK) fprintf(stderr, "  DDI mounted -- developer services are live.\n");
    return rc;
}
