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
