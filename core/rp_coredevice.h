/*
 * rp_coredevice.h — the RPC envelope every coredevice.* feature is wrapped in.
 *
 * Reaching a service is not enough: each call has to arrive inside a dictionary carrying a
 * protocol version, a version structure, two UUIDs, and — depending on the feature — a feature
 * identifier, an action identifier, or both. Getting any of it wrong produces a reply with no
 * `CoreDevice.output` and no explanation of why.
 *
 * Ported from host/coredevice.py, whose envelope was verified against real devices.
 */
#ifndef RP_COREDEVICE_H
#define RP_COREDEVICE_H

#include <stddef.h>
#include <stdint.h>

#include "rp_remotexpc.h"
#include "rp_xpc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RP_CD_VERSION_STRING "629.3"

/* Feature and action identifiers we call. */
#define RP_CD_FEATURE_SCREENSHOT  "com.apple.coredevice.feature.capturescreenshot"
#define RP_CD_ACTION_SCREENSHOT   "com.apple.coredevice.action.capturescreenshot"
#define RP_CD_FEATURE_STARTSTREAM "com.apple.coredevice.feature.startmediastream"
#define RP_CD_ACTION_STARTSTREAM  "com.apple.coredevice.action.mediastreamstart"
#define RP_CD_FEATURE_STOPSTREAM  "com.apple.coredevice.feature.stopmediastream"
#define RP_CD_ACTION_STOPSTREAM   "com.apple.coredevice.action.mediastreamstop"

/* Write the envelope around an already-encoded `CoreDevice.input` object.
 *
 * `input_body`/`input_len` may be NULL/0 for features that take no arguments; an empty dictionary
 * is written in that case, which is what the device expects rather than an absent key.
 * Either identifier may be NULL. Returns the encoded length, or 0 on overflow.
 *
 * `uuid_a` and `uuid_b` are the device and invocation identifiers. They are passed in rather than
 * generated here so this file stays free of platform randomness and remains testable: the same
 * inputs must always produce the same bytes.
 */
size_t rp_cd_build_request(const char *feature_identifier,
                           const char *action_identifier,
                           const uint8_t *input_body, size_t input_len,
                           const char uuid_a[37], const char uuid_b[37],
                           uint8_t *out, size_t out_cap);

/* Send a request and wait for the reply, returning `CoreDevice.output`.
 *
 * Returns 0 on success. A reply without that key is a failure however healthy it looks, so it is
 * reported as one rather than handed back as an empty result. */
int rp_cd_invoke(rp_rxpc_session *s,
                 const char *feature_identifier,
                 const char *action_identifier,
                 const uint8_t *input_body, size_t input_len,
                 const char uuid_a[37], const char uuid_b[37],
                 rp_xpc_obj *output);

#ifdef __cplusplus
}
#endif

#endif /* RP_COREDEVICE_H */
