#include "rp_coredevice.h"

#include <string.h>

/* "629.3" -> {components:[629,3], originalComponentsCount:2, stringValue:"629.3"} */
static void write_version(rp_xpc_writer *w, const char *version)
{
    rp_xpc_key(w, "CoreDevice.coreDeviceVersion");
    rp_xpc_dict_begin(w);

    rp_xpc_key(w, "components");
    rp_xpc_array_begin(w);
    int count = 0;
    const char *p = version;
    while (*p) {
        uint64_t n = 0;
        while (*p >= '0' && *p <= '9') { n = n * 10 + (uint64_t)(*p - '0'); p++; }
        rp_xpc_uint64(w, n);
        count++;
        if (*p == '.') p++;
        else break;
    }
    rp_xpc_array_end(w);

    /* A plain integer here, not a uint64: the reference encodes this one as INT64 and the device
     * is particular about it. */
    rp_xpc_set_int64(w, "originalComponentsCount", count);
    rp_xpc_set_string(w, "stringValue", version);

    rp_xpc_dict_end(w);
}

size_t rp_cd_build_request(const char *feature_identifier,
                           const char *action_identifier,
                           const uint8_t *input_body, size_t input_len,
                           const char uuid_a[37], const char uuid_b[37],
                           uint8_t *out, size_t out_cap)
{
    rp_xpc_writer w;
    rp_xpc_writer_init(&w, out, out_cap);
    rp_xpc_dict_begin(&w);

    /* INT64, not uint64 — same reasoning as originalComponentsCount above. */
    rp_xpc_set_int64(&w, "CoreDevice.CoreDeviceDDIProtocolVersion", 2);
    write_version(&w, RP_CD_VERSION_STRING);
    rp_xpc_set_string(&w, "CoreDevice.deviceIdentifier", uuid_a);

    rp_xpc_key(&w, "CoreDevice.input");
    if (input_body && input_len) {
        /* Splice the already-encoded input object in as-is. Re-encoding it here would mean this
         * file having to know every feature's argument shape. */
        rp_xpc_raw(&w, input_body, input_len);
    } else {
        rp_xpc_dict_begin(&w);
        rp_xpc_dict_end(&w);
    }

    rp_xpc_set_string(&w, "CoreDevice.invocationIdentifier", uuid_b);

    if (feature_identifier) {
        rp_xpc_set_string(&w, "CoreDevice.featureIdentifier", feature_identifier);
        rp_xpc_key(&w, "CoreDevice.action");
        rp_xpc_dict_begin(&w);
        rp_xpc_dict_end(&w);
    }
    if (action_identifier)
        rp_xpc_set_string(&w, "CoreDevice.actionIdentifier", action_identifier);

    rp_xpc_dict_end(&w);
    return w.overflow ? 0 : w.len;
}

int rp_cd_invoke(rp_rxpc_session *s,
                 const char *feature_identifier,
                 const char *action_identifier,
                 const uint8_t *input_body, size_t input_len,
                 const char uuid_a[37], const char uuid_b[37],
                 rp_xpc_obj *output, rp_xpc_obj *reply_out)
{
    static uint8_t req[8192];
    size_t n = rp_cd_build_request(feature_identifier, action_identifier,
                                   input_body, input_len, uuid_a, uuid_b, req, sizeof req);
    /* Distinct codes, because "could not send", "no answer" and "an answer without output" are
     * three different faults that were all reported as -1 and therefore indistinguishable. */
    if (!n) return RP_CD_ERR_BUILD;
    if (rp_rxpc_send(s, req, n, 1) != 0) return RP_CD_ERR_SEND;

    rp_xpc_obj reply;
    if (rp_rxpc_recv(s, &reply) != 0) return RP_CD_ERR_NO_REPLY;
    /* Hand the whole reply back too. A failure without it is untraceable: "no output" and "an
     * error the device explained" look identical to the caller. */
    if (reply_out) *reply_out = reply;

    /* No output key means the call failed, whatever else came back. Reporting that as an empty
     * success is how a caller ends up debugging the wrong layer. */
    return rp_xpc_dict_get(&reply, "CoreDevice.output", output) == 0 ? 0
                                                                    : RP_CD_ERR_NO_OUTPUT;
}
