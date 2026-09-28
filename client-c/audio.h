/*
 * audio.h — the phone's sound, played through SDL.
 *
 * The engine serves it on 127.0.0.1:9878 (app/api/PROTOCOL.md): records of
 * <u16 BE length><u32 BE RTP timestamp><AAC-ELD frame>, 48 kHz stereo, 480 samples a frame.
 * This decodes them with FFmpeg's AAC decoder and queues the PCM on an SDL audio device. Same
 * shape as the macOS app's AudioStream.swift, so the two can be compared frame for frame.
 */
#ifndef RPLAY_AUDIO_H
#define RPLAY_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start the audio thread. It connects, reconnects every 2 s when the engine is not there, and
 * never blocks the caller. Returns 0, or -1 when SDL has no audio device to open. */
int  audio_start(const char *host, int port);

/* Muted: the connection stays up (so unmuting is instant) but nothing is queued. */
void audio_set_muted(int muted);
int  audio_muted(void);

/* frames received, frames that failed to decode, frames skipped to stay within 200 ms. */
void audio_stats(uint64_t *received, uint64_t *undecodable, uint64_t *dropped);

#ifdef __cplusplus
}
#endif

#endif
