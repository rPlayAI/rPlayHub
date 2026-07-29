//
//  rPlayHub-Bridging-Header.h
//  The portable C protocol core, used directly by the app.
//
//  These are the same files the daemon compiles — no fork, no reimplementation. They contain no
//  sockets and no Apple headers, which is what lets one verified copy serve the daemon, the app,
//  and the Linux and Windows ports.
//
//  The app links them so it can talk to the device itself: negotiate the media stream, bind its
//  own RTP port, and receive video with nothing in the data path between the tunnel and the
//  decoder. See media.h for why that boundary is where it is.
//
#import "../../core/rp_xpc.h"
#import "../../core/rp_http2.h"
#import "../../core/rp_remotexpc.h"
#import "../../core/rp_coredevice.h"
#import "../../core/rp_media_offer.h"
#import "../../core/rp_rtp.h"
#import "../../core/rp_rtcp.h"
#import "../../host-c/media.h"
