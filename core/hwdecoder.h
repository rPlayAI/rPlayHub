/*****************************************************************************
 * hwdecoder.h
 *
 * supporting audio output
 *****************************************************************************/

#ifndef _libAirPlay2_HWDECODER_H
#define _libAirPlay2_HWDECODER_H

#include <stdbool.h>

//#define VIDEO_LAYER 10 // this has trouble in jmgo before launcher runs
#define VIDEO_LAYER INT_MAX
#define VIDEO_BACKGROUND_LAYER INT_MAX - 1

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    kProtocolUnknown =0,
    kProtocolAirPlay = 1,
    kProtocolLeLink = 2,//·LEBO's custom protocol simiar to airplay
    kProtocolGCast = 3,
    kProtocolMiracast=4,
    kProtocolIOSUsbMirroring =5,
    kProtocolAndroidUsbMirroring =6,
}YOUCAST_PROTOCOL;

void initHwDecoder();

void* hw_create_video_decoder(YOUCAST_PROTOCOL protocol,
                              void* video_decoder,
                              void **ppWindow,
                              const char *device_name_utf8,
                              const char* fixed_osName,
                              const char* fixed_osVersion,
                              const char* fixed_osBuildVersion,
                              const char* fixed_model,
                              const char *client_ip_addr,
                              const char* extradata,
                              int extradata_size,
                              int width,
                              int height,
                              int transform,
                              UInt32* keepRunning);

void fixed_hw_free_decoder(void *ptr);

int hw_decode(void *decoder, void *window, const char* data, int data_size, int flags);

void hw_rotate_window(void* decoder, void *window, int transform);

void delete_window(void* window, bool for_resizing);

void hw_on_mirroring_connected(void *context, int protocol);

void hw_on_mirroring_disconnected(void *context);

void hw_set_mirroring_protocol(int protocol);

#ifdef __cplusplus
}
#endif

#endif // _HWDECODER_H
