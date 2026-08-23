/*
 * api_server.h — the daemon's side of the contract the app already speaks.
 *
 * The Swift player talks newline-delimited JSON on one port and reads a raw Annex-B byte stream
 * on another. That contract is defined in app/api/PROTOCOL.md and is already implemented by the
 * Python engine, so serving exactly the same thing from C means the app connects to either
 * without knowing the difference — which is what makes the port verifiable one method at a time
 * instead of as a single flag day.
 *
 * Unimplemented methods return an explicit error naming themselves. A daemon that answers
 * plausibly to something it cannot do is worse than one that refuses.
 */
#ifndef API_SERVER_H
#define API_SERVER_H

#include "../core/rp_remotexpc.h"

#define API_PORT    9876
#define STREAM_PORT 9877

/* One attached device, as usbmuxd reports it.
 *
 * The daemon streams from exactly one device, but the sidebar should show every device the Mac
 * can see -- Device Hub does, and a list that hides the phone you are looking for is worse than
 * no list. Only the bound one can be mirrored; the rest are listed so they can be selected. */
typedef struct {
    char udid[64];
    char connection[16];          /* "USB" or "Network", as usbmuxd spells it */
    int  device_id;               /* usbmuxd's handle, for connecting to the device's lockdown */
    char name[128];               /* DeviceName, ProductVersion, ProductType from lockdown; */
    char version[32];             /* empty when the device did not answer (not paired, asleep) */
    char product_type[32];
} api_device;

#define API_MAX_DEVICES 16

/* Fills `out` with the attached devices, returning how many. Implemented in cdhost.c because
 * that is where the usbmux client lives; declared here because the API server is what needs it. */
int usbmux_enumerate(api_device *out, int max);

/* The Info tab's data for `udid`: lockdown GetValue over a session, rendered as JSON in the shape
 * host/deviceinfo.py produced ({"default":{..},"com.apple.mobile.battery":{..},...,
 * "unavailable":{..},"session_error":..}). Returns non-zero with {"error":..} in `json`. */
int cdhost_device_info(const char *udid, char *json, size_t cap);

/* Bind a different device. Does not return on success.
 *
 * Implemented by re-executing this daemon with the new udid, rather than by unwinding the session
 * in place. Startup is a linear sequence -- lockdown, TLS, CoreDeviceProxy, utun, two pump
 * threads, RSD, media -- with no teardown path, and writing one only to use it here would be a
 * lot of new code whose failure mode is leaked utun interfaces and threads.
 *
 * Re-exec is not a dodge: binding another device needs a fresh tunnel and a fresh media session
 * anyway, since the device permits one stream per session and will not restart it. The disconnect
 * is inherent to the operation, not a cost of doing it this way, and the kernel releases every
 * resource for free. */
void cdhost_rebind(const char *udid);

/* Everything the server needs to answer questions about the session it belongs to. */
typedef struct {
    const char *udid;
    const char *device_name;
    const char *product_version;
    const char *tunnel_addr;      /* device address inside the tunnel */
    long        rsd_port;
    int         screen_w, screen_h;

    /* RSD-discovered service ports, 0 when the device did not offer them. */
    long        screenshot_port;
    long        display_port;
    long        hid_port;
    long        diag_port;            /* diagnostics_relay.shim.remote: restart/shutdown/sleep */
    long        app_port;             /* coredevice.appservice: list/launch/terminate apps */
    long        syslog_port;          /* syslog_relay.shim.remote: the console stream */
    long        instproxy_port;       /* installation_proxy.shim.remote: the app list */
    long        misagent_port;        /* misagent.shim.remote: provisioning profiles */
    long        mcinstall_port;       /* MCInstall.shim.remote: configuration profiles */
    long        afc_port;             /* afc.shim.remote: the Media partition */
    long        crashcopy_port;       /* crashreportcopymobile.shim.remote: AFC over crash reports */
    long        crashmover_port;      /* crashreportmover.shim.remote: poke before listing */
    long        mounter_port;         /* mobile_image_mounter.shim.remote: DDI self-activation */
    const char *ddi_dir;             /* where the bundled DDI lives, or NULL to search defaults */

    /* Needed to negotiate the media stream: the offer carries our address, and the SSRC in it
     * must be the same value the RTCP session uses. */
    const char *our_addr;
    uint32_t    ssrc;
    double      keyframe_every_s;
} api_session;

/* Serve until the process is killed. Returns non-zero if the listeners could not be created. */
int api_serve(api_session *session);

#endif /* API_SERVER_H */
