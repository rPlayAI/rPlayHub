/*
 * rp_media_offer.h — the startmediastream offer.
 *
 * A protobuf describing what we can decode, zlib-compressed at level 9, wrapped in a binary
 * plist. Every layer of that is load-bearing: the device rejects any other compression level, and
 * a single wrong protobuf field gets a stream that either never starts or arrives unusable.
 *
 * The values here are not guesses. A tcpdump of Device Hub's own session was decoded field by
 * field and diffed against this, and the result is identical: same codec banks, same resolution
 * entries, same feature strings, all ten bitrate tiers, same decoder name. Two fields once
 * differed and both were wrong on our side -- field 7 (ltrpEnabled) was 0 where Apple sends 1,
 * and field 10 was ours alone.
 *
 * Ported from host/screen.py. Where they disagree the Python is right, because it is the one
 * proven against real phones.
 */
#ifndef RP_MEDIA_OFFER_H
#define RP_MEDIA_OFFER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RP_OFFER_CODEC_AUTO = 0,   /* both banks; the device picks, and picks HEVC */
    RP_OFFER_CODEC_HEVC,
    RP_OFFER_CODEC_H264,
} rp_offer_codec;

typedef struct {
    /* This receiver's SSRC. It goes into field 5.1 of the blob AND must be the SSRC of every
     * RTCP packet we send: the device echoes it back as RemoteSSRC and silently discards
     * feedback that arrives from any other source. Sending acks, PLI or FIR from a different
     * SSRC makes all three appear to be ignored -- which is exactly what happened. */
    uint32_t ssrc;
    rp_offer_codec codec;

    /* Host identity, echoed by the device and not otherwise acted on. */
    const char *model;        /* "Mac15,9" */
    const char *os_version;   /* "2205.3.1" */
    const char *build;        /* "25F80" */
    const char *call_id;      /* uppercase UUID string */

    /* Long-term reference pictures. 1 matches Apple. Only meaningful if LTR acks are actually
     * sent, and from the right SSRC -- enabling it without them is worse than leaving it off,
     * because errors then anchor instead of washing out. */
    int ltrp_enabled;

    /* Ask for the device's sound instead of its screen: negotiator mode 6 and an audio-settings
     * blob. Everything else -- the plist, the tiers, the decoder name -- is shared, exactly as it
     * is in Device Hub's own pair of offers. */
    int audio;
} rp_offer_params;

/* Build the uncompressed protobuf blob. Returns its length, or 0 on overflow. Exposed separately
 * from the plist so it can be compared byte-for-byte against the Python and against captures. */
size_t rp_media_blob_video(const rp_offer_params *p, uint8_t *out, size_t cap);

/* The audio variant, from Device Hub 27's capture. Field 3 replaces field 5's video settings:
 * {1: our SSRC, 4: 24191 -- the codec set Device Hub offers}. The device answers AAC-ELD,
 * 48 kHz stereo, 480 samples per packet, on RTP payload type 101. */
size_t rp_media_blob_audio(const rp_offer_params *p, uint8_t *out, size_t cap);

/* Build the complete offer: blob -> zlib level 9 -> binary plist. Returns length, or 0. */
size_t rp_build_offer(const rp_offer_params *p, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* RP_MEDIA_OFFER_H */
