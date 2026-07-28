/*
 * rp_video_stream — portable Annex-B → access-unit assembly for H.264 and HEVC.
 *
 * This is the part of the video path that is NOT platform-specific, and it is in C so macOS,
 * Linux and Windows share one implementation instead of three. It does everything up to the
 * decoder's door:
 *
 *   - splits an Annex-B byte stream into NAL units, tolerating reads that land anywhere
 *   - classifies them for either codec (the two disagree on header length, on which types are
 *     parameter sets, and on which are keyframes)
 *   - caches parameter sets and reports when a complete set is available
 *   - groups VCL NALs into access units, one per picture
 *   - drops frames until a keyframe arrives, because feeding a decoder unanchored P-frames
 *     produces a black window with no error rather than a diagnosable failure
 *
 * What stays per-platform is only the decoder and the display surface: VideoToolbox on macOS,
 * ffmpeg/libavcodec on Linux, Media Foundation or ffmpeg on Windows. See app/PORTING.md.
 *
 * No allocation per NAL, no dependencies beyond libc, no I/O. Safe to call from one thread.
 */
#ifndef RP_VIDEO_STREAM_H
#define RP_VIDEO_STREAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RP_CODEC_HEVC = 0,
    RP_CODEC_H264 = 1
} rp_codec;

/* One NAL unit, pointing into the stream's internal buffer. Valid only for the callback's
 * duration — copy it if you need to keep it. */
typedef struct {
    const uint8_t *data;
    size_t         size;
    int            type;        /* codec-specific NAL unit type */
} rp_nal;

/* A complete set of parameter sets is available (SPS/PPS, or VPS/SPS/PPS). Build the decoder's
 * format description here. Called again if the device changes them mid-stream. */
typedef void (*rp_params_fn)(void *ctx, const rp_nal *sets, size_t count, rp_codec codec);

/* One picture, ready to decode. `is_keyframe` marks an IRAP/IDR. */
typedef void (*rp_access_unit_fn)(void *ctx, const rp_nal *nals, size_t count, int is_keyframe);

typedef struct {
    uint64_t nals_seen;
    uint64_t access_units;
    uint64_t keyframes;
    uint64_t frames_before_keyframe;   /* dropped for lack of an anchor */
    uint64_t bytes_in;
} rp_video_stats;

typedef struct rp_video_stream rp_video_stream;

rp_video_stream *rp_video_stream_create(rp_codec codec);
void rp_video_stream_destroy(rp_video_stream *s);

/* Changing codec resets parameter sets, the pending access unit and the keyframe gate. */
void rp_video_stream_set_codec(rp_video_stream *s, rp_codec codec);
rp_codec rp_video_stream_codec(const rp_video_stream *s);

void rp_video_stream_set_callbacks(rp_video_stream *s, void *ctx,
                                   rp_params_fn on_params,
                                   rp_access_unit_fn on_access_unit);

/* Feed Annex-B bytes. Any chunk boundary is fine, including mid-start-code.
 *
 * A NAL is only complete once the next start code arrives, so callbacks for the most recent NAL
 * lag by one. On a live stream that is invisible — frames follow immediately — but it means a
 * final NAL needs rp_video_stream_flush() to come out. */
void rp_video_stream_feed(rp_video_stream *s, const uint8_t *bytes, size_t n);

/* Emit whatever picture is pending. Call at end of stream. */
void rp_video_stream_flush(rp_video_stream *s);

/* Re-arm the keyframe gate — after a decoder failure, or when the stream restarts. */
void rp_video_stream_await_keyframe(rp_video_stream *s);
int  rp_video_stream_is_awaiting_keyframe(const rp_video_stream *s);

rp_video_stats rp_video_stream_stats(const rp_video_stream *s);

/* --- helpers for the platform backends --- */

/* NAL type for a codec: HEVC has a 2-byte header with the type in bits 1-6, H.264 a 1-byte
 * header with it in bits 0-4. */
int rp_nal_type(const uint8_t *nal, size_t size, rp_codec codec);
int rp_nal_is_parameter_set(int type, rp_codec codec);
int rp_nal_is_keyframe(int type, rp_codec codec);
int rp_nal_is_vcl(int type, rp_codec codec);

/* Pack an access unit for a decoder that wants 4-byte length prefixes (VideoToolbox's
 * nalUnitHeaderLength = 4). Returns the bytes written, or the size required when out is NULL. */
size_t rp_pack_length_prefixed(const rp_nal *nals, size_t count, uint8_t *out, size_t out_size);

/* Pack with Annex-B start codes, for decoders and files that want that (ffmpeg, .h264/.h265). */
size_t rp_pack_annexb(const rp_nal *nals, size_t count, uint8_t *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* RP_VIDEO_STREAM_H */
