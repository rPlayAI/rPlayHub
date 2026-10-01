#include "audio.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef intptr_t ssize_t;
#define sleep(x) Sleep((x)*1000)
#define close closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif
#include <stdio.h>
#include <string.h>

#include <SDL.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>

#define RATE      48000
#define CHANNELS  2
#define FRAME     480
/* Bytes of queued float PCM: start after 30 ms, skip frames beyond 200 ms. The phone's clock and
 * ours never agree exactly, and without a ceiling the difference becomes ever-growing delay. */
#define BYTES_PER_MS  (RATE / 1000 * CHANNELS * (int)sizeof(float))
#define START_AFTER   (30 * BYTES_PER_MS)
#define DROP_ABOVE    (200 * BYTES_PER_MS)

static const char *g_host;
static int g_port;
static SDL_AudioDeviceID g_dev;
static volatile int g_muted;
static volatile uint64_t g_received, g_undecodable, g_dropped;

void audio_set_muted(int muted) { g_muted = muted; if (muted && g_dev) SDL_ClearQueuedAudio(g_dev); }
int  audio_muted(void) { return g_muted; }

void audio_stats(uint64_t *received, uint64_t *undecodable, uint64_t *dropped)
{
    if (received) *received = g_received;
    if (undecodable) *undecodable = g_undecodable;
    if (dropped) *dropped = g_dropped;
}

static int connect_to(const char *host, int port)
{
#ifdef _WIN32
    static int wsa_init = 0;
    if (!wsa_init) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        wsa_init = 1;
    }
#endif
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, ps, &hints, &res) != 0) return -1;
    int fd = -1;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        fd = (int)socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, a->ai_addr, (int)a->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    }
    return fd;
}

/* ER AAC-ELD, 48 kHz, two channels, 480-sample frames: FFmpeg takes the AudioSpecificConfig
 * bare, where AudioToolbox wants it inside an ES descriptor. */
static AVCodecContext *open_decoder(void)
{
    static const uint8_t asc[4] = { 0xf8, 0xe6, 0x50, 0x00 };
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (!codec) { fprintf(stderr, "audio: libavcodec has no AAC decoder\n"); return NULL; }
    AVCodecContext *dec = avcodec_alloc_context3(codec);
    if (!dec) return NULL;
    dec->extradata = av_mallocz(sizeof asc + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(dec->extradata, asc, sizeof asc);
    dec->extradata_size = sizeof asc;
    dec->sample_rate = RATE;
    av_channel_layout_default(&dec->ch_layout, CHANNELS);
    if (avcodec_open2(dec, codec, NULL) != 0) { avcodec_free_context(&dec); return NULL; }
    return dec;
}

/* Planar float (what the AAC decoder produces) to the interleaved float SDL was opened with. */
static void play(const AVFrame *f)
{
    static float pcm[FRAME * 4 * CHANNELS];
    int n = f->nb_samples;
    if (n <= 0 || n > FRAME * 4) return;
    int ch = f->ch_layout.nb_channels;
    for (int i = 0; i < n; i++)
        for (int c = 0; c < CHANNELS; c++) {
            const float *src = (const float *)f->extended_data[c < ch ? c : 0];
            pcm[i * CHANNELS + c] = src[i];
        }
    Uint32 queued = SDL_GetQueuedAudioSize(g_dev);
    if (queued > DROP_ABOVE) { g_dropped++; return; }
    SDL_QueueAudio(g_dev, pcm, (Uint32)(n * CHANNELS * sizeof(float)));
    /* Hold playback until there is a cushion, and again whenever it runs dry. */
    if (queued + (Uint32)(n * CHANNELS * sizeof(float)) >= START_AFTER)
        SDL_PauseAudioDevice(g_dev, 0);
    else if (queued == 0)
        SDL_PauseAudioDevice(g_dev, 1);
}

static void run_connection(int fd, AVCodecContext *dec, AVPacket *pkt, AVFrame *frame)
{
    static uint8_t buf[1 << 16];
    size_t have = 0;
    for (;;) {
        ssize_t r = recv(fd, (char *)buf + have, (int)(sizeof buf - have), 0);
        if (r <= 0) return;
        have += (size_t)r;
        size_t off = 0;
        while (have - off >= 6) {
            size_t len = (size_t)buf[off] << 8 | buf[off + 1];
            if (have - off < 6 + len) break;
            const uint8_t *data = buf + off + 6;
            off += 6 + len;
            g_received++;
            if (g_muted || !len) continue;
            if (av_new_packet(pkt, (int)len) != 0) continue;
            memcpy(pkt->data, data, len);
            int ok = avcodec_send_packet(dec, pkt) == 0;
            av_packet_unref(pkt);
            if (!ok) { g_undecodable++; continue; }
            while (avcodec_receive_frame(dec, frame) == 0) play(frame);
        }
        memmove(buf, buf + off, have - off);
        have -= off;
        if (have == sizeof buf) have = 0;   /* a record longer than the buffer: resync */
    }
}

static int SDLCALL audio_thread_fn(void *arg)
{
    (void)arg;
    AVCodecContext *dec = open_decoder();
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (!dec || !pkt || !frame) return 0;
    int said = 0;
    for (;;) {
        int fd = connect_to(g_host, g_port);
        if (fd < 0) { sleep(2); continue; }
        if (!said) { fprintf(stderr, "audio: playing the device's sound from %s:%d\n", g_host, g_port); said = 1; }
        run_connection(fd, dec, pkt, frame);
        close(fd);
        SDL_ClearQueuedAudio(g_dev);
        avcodec_flush_buffers(dec);
        sleep(2);
    }
    return 0;
}

int audio_start(const char *host, int port)
{
    g_host = host;
    g_port = port;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "audio: SDL audio unavailable: %s\n", SDL_GetError());
        return -1;
    }
    SDL_AudioSpec want = { 0 }, have;
    want.freq = RATE;
    want.format = AUDIO_F32SYS;
    want.channels = CHANNELS;
    want.samples = 512;
    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_dev) {
        fprintf(stderr, "audio: no output device: %s\n", SDL_GetError());
        return -1;
    }
    SDL_Thread *t = SDL_CreateThread(audio_thread_fn, "audio_thread", NULL);
    if (!t) return -1;
    SDL_DetachThread(t);
    return 0;
}
