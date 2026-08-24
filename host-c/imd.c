/* imd.c -- libimobiledevice-backed replacements for the hand-rolled usbmux/lockdown code.
 *
 * Stage 1 of the migration (2026-08-24): cdhost_device_info, the Info tab's lockdown query. The
 * spike proved the library does Layer 0-1.5 in a few calls; this is the first of those paths moved
 * off our usbmux+lockdown+CFLite and onto libplist + libimobiledevice, which is what makes the
 * Linux/Windows ports tractable (libusbmuxd talks to the open-source usbmuxd; no CFLite). */
#include "api_server.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>
#include <plist/plist.h>

/* ------------------------------------------------------------------ JSON helpers (libplist values) */

static void j_str(char **p, char *end, const char *s)
{
    if (*p < end) *(*p)++ = '"';
    for (; s && *s && *p + 8 < end; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { *(*p)++ = '\\'; *(*p)++ = (char)c; }
        else if (c == '\n') { *(*p)++ = '\\'; *(*p)++ = 'n'; }
        else if (c == '\r') { *(*p)++ = '\\'; *(*p)++ = 'r'; }
        else if (c == '\t') { *(*p)++ = '\\'; *(*p)++ = 't'; }
        else if (c < 0x20) *p += snprintf(*p, (size_t)(end - *p), "\\u%04x", c);
        else *(*p)++ = (char)c;
    }
    if (*p < end) *(*p)++ = '"';
    **p = 0;
}

/* Render a plist value as JSON. Returns 1 if it wrote a value, 0 for types with no row. */
static int j_plist(char **p, char *end, plist_t v)
{
    switch (plist_get_node_type(v)) {
    case PLIST_STRING: { char *s = NULL; plist_get_string_val(v, &s); j_str(p, end, s ? s : ""); free(s); return 1; }
    case PLIST_BOOLEAN: { uint8_t b = 0; plist_get_bool_val(v, &b); *p += snprintf(*p, (size_t)(end - *p), b ? "true" : "false"); return 1; }
    case PLIST_UINT: { uint64_t n = 0; plist_get_uint_val(v, &n); *p += snprintf(*p, (size_t)(end - *p), "%llu", (unsigned long long)n); return 1; }
    case PLIST_REAL: { double d = 0; plist_get_real_val(v, &d); *p += snprintf(*p, (size_t)(end - *p), "%g", d); return 1; }
    default: return 0;
    }
}

/* ------------------------------------------------------------------ device_info */

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

    idevice_t dev = NULL;
    if (idevice_new_with_options(&dev, udid, IDEVICE_LOOKUP_USBMUX | IDEVICE_LOOKUP_NETWORK) != IDEVICE_E_SUCCESS) {
        snprintf(json, cap, "{\"error\":\"device %s is not attached\"}", udid);
        return -1;
    }

    /* A full session (with_handshake) is needed for the battery and disk domains; the default
     * domain answers without one. Fall back to a session-less client and report why, exactly as
     * the old code did -- so the panel still shows the basics for an unpaired device. */
    const char *session_error = NULL;
    lockdownd_client_t lk = NULL;
    if (lockdownd_client_new_with_handshake(dev, &lk, "rplay-hub") != LOCKDOWN_E_SUCCESS || !lk) {
        session_error = "no session (device not paired/trusted, or handshake refused)";
        if (lockdownd_client_new(dev, &lk, "rplay-hub") != LOCKDOWN_E_SUCCESS) {
            idevice_free(dev);
            snprintf(json, cap, "{\"error\":\"lockdown unreachable\"}");
            return -1;
        }
    }

    p += snprintf(p, (size_t)(end - p), "{");
    char unavailable[2048] = "";
    size_t ulen = 0;
    for (size_t d = 0; d < sizeof domains / sizeof domains[0]; d++) {
        if (d > 0 && session_error) break;      /* the extra domains need a session */
        int wrote = 0;
        for (const char **k = domains[d].keys; *k; k++) {
            plist_t v = NULL;
            lockdownd_error_t e = lockdownd_get_value(lk, domains[d].domain, *k, &v);
            if (e != LOCKDOWN_E_SUCCESS || !v || plist_get_node_type(v) == PLIST_NONE) {
                ulen += (size_t)snprintf(unavailable + ulen, sizeof unavailable - ulen,
                                         "%s\"%s.%s\":\"no value\"", ulen ? "," : "",
                                         domains[d].label, *k);
                if (v) plist_free(v);
                continue;
            }
            if (!wrote) {
                p += snprintf(p, (size_t)(end - p), "%s", p[-1] == '{' ? "" : ",");
                j_str(&p, end, domains[d].label);
                p += snprintf(p, (size_t)(end - p), ":{");
            } else {
                p += snprintf(p, (size_t)(end - p), ",");
            }
            j_str(&p, end, *k);
            p += snprintf(p, (size_t)(end - p), ":");
            if (!j_plist(&p, end, v)) p += snprintf(p, (size_t)(end - p), "null");
            if (!strcmp(*k, "UniqueChipID") && plist_get_node_type(v) == PLIST_UINT) {
                uint64_t ecid = 0; plist_get_uint_val(v, &ecid);
                p += snprintf(p, (size_t)(end - p), ",\"ECID\":\"%llX\"", (unsigned long long)ecid);
            }
            wrote++;
            plist_free(v);
        }
        if (wrote) p += snprintf(p, (size_t)(end - p), "}");
    }
    p += snprintf(p, (size_t)(end - p), "%s\"unavailable\":{%s}", p[-1] == '{' ? "" : ",", unavailable);
    if (session_error) {
        p += snprintf(p, (size_t)(end - p), ",\"session_error\":");
        j_str(&p, end, session_error);
    }
    p += snprintf(p, (size_t)(end - p), "}");

    lockdownd_client_free(lk);
    idevice_free(dev);
    return 0;
}

/* ------------------------------------------------------------------ device list + names (Stage 2)
 *
 * Replaces the hand-rolled usbmux ListDevices + per-device lockdown naming. `idevice_get_device_
 * list_extended` gives udid + transport; DeviceName/ProductVersion/ProductType read from the
 * default lockdown domain without a session, so every device is named -- paired or not -- as
 * Device Hub shows them. USB and Network entries for one phone collapse to a single row (USB
 * preferred). device_id is no longer needed (the library keys on udid). */
int usbmux_enumerate(api_device *out, int max)
{
    idevice_info_t *list = NULL;
    int count = 0;
    if (idevice_get_device_list_extended(&list, &count) != IDEVICE_E_SUCCESS) return 0;

    int n = 0;
    for (int i = 0; i < count && n < max; i++) {
        const char *udid = list[i]->udid;
        const char *conn = list[i]->conn_type == CONNECTION_USBMUXD ? "USB" : "Network";
        int existing = -1;
        for (int k = 0; k < n; k++) if (!strcmp(out[k].udid, udid)) { existing = k; break; }
        if (existing >= 0) {
            if (!strcmp(conn, "USB")) snprintf(out[existing].connection, sizeof out[existing].connection, "USB");
            continue;
        }
        memset(&out[n], 0, sizeof out[n]);
        snprintf(out[n].udid, sizeof out[n].udid, "%s", udid);
        snprintf(out[n].connection, sizeof out[n].connection, "%s", conn);
        n++;
    }
    idevice_device_list_extended_free(list);

    for (int i = 0; i < n; i++) {
        idevice_t dev = NULL;
        if (idevice_new_with_options(&dev, out[i].udid, IDEVICE_LOOKUP_USBMUX | IDEVICE_LOOKUP_NETWORK) != IDEVICE_E_SUCCESS)
            continue;
        lockdownd_client_t lk = NULL;
        if (lockdownd_client_new(dev, &lk, "rplay-hub") == LOCKDOWN_E_SUCCESS && lk) {
            plist_t v = NULL;
            if (lockdownd_get_value(lk, NULL, "DeviceName", &v) == LOCKDOWN_E_SUCCESS && v) {
                char *s = NULL; plist_get_string_val(v, &s);
                if (s) { snprintf(out[i].name, sizeof out[i].name, "%s", s); free(s); }
                plist_free(v); v = NULL;
            }
            if (lockdownd_get_value(lk, NULL, "ProductVersion", &v) == LOCKDOWN_E_SUCCESS && v) {
                char *s = NULL; plist_get_string_val(v, &s);
                if (s) { snprintf(out[i].version, sizeof out[i].version, "%s", s); free(s); }
                plist_free(v); v = NULL;
            }
            if (lockdownd_get_value(lk, NULL, "ProductType", &v) == LOCKDOWN_E_SUCCESS && v) {
                char *s = NULL; plist_get_string_val(v, &s);
                if (s) { snprintf(out[i].product_type, sizeof out[i].product_type, "%s", s); free(s); }
                plist_free(v); v = NULL;
            }
            lockdownd_client_free(lk);
        }
        idevice_free(dev);
    }
    return n;
}

/* ------------------------------------------------------------------ tunnel bringup (Stage 3)
 *
 * Layers 0-2 via libimobiledevice, replacing the hand-rolled usbmux + lockdown + our-TLS path
 * (the piece that does not port: /var/run/usbmuxd is a macOS unix socket, and lockdown's TLS is
 * ours). idevice_new + lockdownd_client_new_with_handshake do usbmux discovery, the pair-record
 * TLS session, and device queries; lockdownd_start_service + idevice_connect (+ enable_ssl) hand
 * back a connected, TLS'd CoreDeviceProxy channel. cdhost.c wraps that idevice_connection_t in a
 * conn_t and runs the existing CoreDeviceProxy handshake + packet pump over it -- unchanged.
 *
 * The idevice_t is deliberately kept alive for the daemon's lifetime (the connection rides on it),
 * and the daemon runs until killed, so it is not freed.
 */
#include <libimobiledevice/lockdown.h>

/* libimobiledevice's connect to a network device has no short timeout: if the target phone is not
 * reachable when a bringup runs (a wifi device that dropped, a re-exec onto the wrong device), the
 * whole daemon hangs for the OS TCP timeout or longer. A watchdog bounds it -- a stuck bringup
 * exits cleanly with a clear message rather than locking up (under launchd, that restarts on the
 * default device; a manual run just exits). 30s is generous: a healthy bringup takes ~1-2s. */
static void imd_bringup_timeout(int sig)
{
    (void)sig;
    static const char msg[] = "\n  bringup timed out (30s): the device is not reachable. "
                              "Is it on this network and unlocked?\n";
    if (write(2, msg, sizeof msg - 1)) { /* ignore */ }
    _exit(2);
}

int imd_bringup(void **out_conn, char *udid_out, size_t udidlen,
                char *devname, size_t dnlen, char *prodver, size_t pvlen)
{
    signal(SIGALRM, imd_bringup_timeout);
    alarm(30);
    printf("== Layer 0: usbmux (libusbmuxd) ==\n");
    api_device devs[API_MAX_DEVICES];
    int n = usbmux_enumerate(devs, API_MAX_DEVICES);
    if (n <= 0) { fprintf(stderr, "no devices\n"); return -1; }
    const char *want = getenv("RPLAY_UDID");
    int chosen = -1;
    for (int i = 0; i < n; i++) {
        printf("  udid=%s conn=%s\n", devs[i].udid, devs[i].connection);
        if (want && *want) { if (!strncmp(devs[i].udid, want, strlen(want)) && chosen < 0) chosen = i; }
        else if (chosen < 0 || (!strcmp(devs[i].connection, "USB") && strcmp(devs[chosen].connection, "USB")))
            chosen = i;   /* prefer USB when a phone is on both transports (wifi displayservice is flakier) */
    }
    if (chosen < 0) { fprintf(stderr, "no matching device\n"); return -1; }
    snprintf(udid_out, udidlen, "%s", devs[chosen].udid);
    printf("  -> using the %s entry (%s)\n", devs[chosen].connection, udid_out);

    idevice_t dev = NULL;
    if (idevice_new_with_options(&dev, udid_out, IDEVICE_LOOKUP_USBMUX | IDEVICE_LOOKUP_NETWORK) != IDEVICE_E_SUCCESS) {
        fprintf(stderr, "idevice_new failed\n"); return -1;
    }

    printf("\n== Layer 1-1.5: lockdown + TLS session (libimobiledevice) ==\n");
    lockdownd_client_t lk = NULL;
    if (lockdownd_client_new_with_handshake(dev, &lk, "rplay-hub") != LOCKDOWN_E_SUCCESS || !lk) {
        fprintf(stderr, "lockdown handshake failed (device paired & trusted?)\n");
        idevice_free(dev); return -1;
    }
    const char *keys[] = { "DeviceName", "ProductType", "ProductVersion", "BuildVersion", "UniqueChipID" };
    snprintf(devname, dnlen, "?"); snprintf(prodver, pvlen, "?");
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        plist_t v = NULL;
        char out[128] = "?";
        if (lockdownd_get_value(lk, NULL, keys[i], &v) == LOCKDOWN_E_SUCCESS && v) {
            if (plist_get_node_type(v) == PLIST_STRING) { char *s = NULL; plist_get_string_val(v, &s); if (s) { snprintf(out, sizeof out, "%s", s); free(s); } }
            else if (plist_get_node_type(v) == PLIST_UINT) { uint64_t u = 0; plist_get_uint_val(v, &u); snprintf(out, sizeof out, "%llu", (unsigned long long)u); }
            plist_free(v);
        }
        printf("  %-15s = %s\n", keys[i], out);
        if (!strcmp(keys[i], "DeviceName")) snprintf(devname, dnlen, "%s", out);
        if (!strcmp(keys[i], "ProductVersion")) snprintf(prodver, pvlen, "%s", out);
    }
    if (atoi(prodver) > 0 && atoi(prodver) < 27)
        printf("\n  ⚠️  iOS %s cannot mirror (screen viewing needs iOS 27). Everything else works.\n", prodver);

    printf("\n== Layer 2: CoreDevice tunnel (CoreDeviceProxy) ==\n");
    lockdownd_service_descriptor_t svc = NULL;
    if (lockdownd_start_service(lk, "com.apple.internal.devicecompute.CoreDeviceProxy", &svc) != LOCKDOWN_E_SUCCESS || !svc) {
        fprintf(stderr, "CoreDeviceProxy StartService failed\n");
        lockdownd_client_free(lk); idevice_free(dev); return -1;
    }
    printf("  CoreDeviceProxy port=%d ssl=%d\n", svc->port, svc->ssl_enabled);
    idevice_connection_t conn = NULL;
    if (idevice_connect(dev, svc->port, &conn) != IDEVICE_E_SUCCESS || !conn) {
        fprintf(stderr, "CoreDeviceProxy connect failed\n");
        lockdownd_service_descriptor_free(svc); lockdownd_client_free(lk); idevice_free(dev); return -1;
    }
    if (svc->ssl_enabled && idevice_connection_enable_ssl(conn) != IDEVICE_E_SUCCESS) {
        fprintf(stderr, "CoreDeviceProxy TLS failed\n");
        idevice_disconnect(conn); lockdownd_service_descriptor_free(svc); lockdownd_client_free(lk); idevice_free(dev); return -1;
    }
    lockdownd_service_descriptor_free(svc);
    lockdownd_client_free(lk);          /* the service is started; the client is no longer needed */
    /* dev is intentionally NOT freed: the connection rides on it for the daemon's life. */
    alarm(0);            /* bringup done; the pump has its own (blocking) timeout handling */
    *out_conn = conn;
    return 0;
}

/* Blocking send/recv over the idevice connection, for conn_t in cdhost.c (which cannot include
 * libimobiledevice.h without pulling plist into everything). Loop to exact byte counts. */
int imd_conn_send(void *conn, const void *buf, size_t n)
{
    size_t off = 0;
    while (off < n) {
        uint32_t sent = 0;
        if (idevice_connection_send((idevice_connection_t)conn, (const char *)buf + off, (uint32_t)(n - off), &sent) != IDEVICE_E_SUCCESS || sent == 0)
            return -1;
        off += sent;
    }
    return 0;
}

int imd_conn_recv(void *conn, void *buf, size_t n)
{
    /* Block until exactly n bytes arrive, like the raw-socket recv the pump used to use.
     * idevice_connection_receive_timeout returns SUCCESS with 0 bytes on a timeout (a quiet
     * tunnel between packets is normal), so a 0-byte read is NOT end-of-stream -- keep waiting.
     * Only a non-success error (connection dropped) ends it. Treating a timeout as fatal killed
     * the packet pump the moment the tunnel went quiet after the startup RSD burst. */
    size_t off = 0;
    while (off < n) {
        uint32_t got = 0;
        idevice_error_t e = idevice_connection_receive_timeout((idevice_connection_t)conn, (char *)buf + off, (uint32_t)(n - off), &got, 30000);
        if (e != IDEVICE_E_SUCCESS) return -1;
        off += got;
    }
    return 0;
}
