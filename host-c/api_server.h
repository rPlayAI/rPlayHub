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
} api_session;

/* Serve until the process is killed. Returns non-zero if the listeners could not be created. */
int api_serve(api_session *session);

#endif /* API_SERVER_H */
