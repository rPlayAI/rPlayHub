/*
 * rp_geometry — portable coordinate mapping between a window, the coded video, and the device.
 *
 * The input path is routed the same way on every platform: capture a pointer event, turn it into
 * a device coordinate, send it. Only the capture and the send are platform- or protocol-specific
 * (NSEvent vs SDL vs Win32; universalhidservice vs iAP HID). The maths in between is identical
 * everywhere, so it lives here rather than in each UI.
 *
 * Two facts drive all of it, both verified against a real iPhone 13 Pro:
 *
 *  1. The coded video is LARGER than the screen. displayservice encodes at 16-pixel alignment
 *     with no conformance-window crop, so a 1170x2532 screen arrives as 1184x2576 with black
 *     padding on the right and bottom. Mapping a click against the full frame drifts it toward
 *     the top-left by up to 1.7% — about 44px at the bottom of the screen.
 *  2. The device's origin is top-left. A UI whose origin is bottom-left (AppKit unflipped) must
 *     invert y; one that is already top-left must not.
 */
#ifndef RP_GEOMETRY_H
#define RP_GEOMETRY_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double x, y, w, h;
} rp_rect;

typedef struct {
    double w, h;
} rp_size;

/* Where the device screen sits inside a view, aspect-fit and centred. Clicks map against THIS,
 * never the raw view bounds — that is the classic off-by-a-letterbox bug. */
rp_rect rp_fit_rect(rp_size view, rp_size content);

/* The fraction of the coded frame that is real screen, anchored top-left. Returns {1,1} when the
 * device size is unknown, i.e. show everything rather than guess a crop. */
rp_size rp_visible_fraction(rp_size video, rp_size device);

/* Frame for a video layer inside `screen` such that the frame's top-left screen region exactly
 * fills it and the alignment padding falls outside. `origin_top_left` selects the coordinate
 * convention of the UI: 1 for top-left origins (flipped AppKit views, SDL, Win32), 0 for
 * bottom-left (unflipped AppKit). */
rp_rect rp_video_layer_frame(rp_rect screen, rp_size video, rp_size device, int origin_top_left);

/* Pointer position -> device fractions in 0..1. Returns 0 and leaves fx/fy alone if the point is
 * outside the screen rectangle (a click on the letterbox is not a tap). */
int rp_normalize_point(double px, double py, rp_rect screen, int origin_top_left,
                       double *fx, double *fy);

/* Fractions -> the 0..65535 range a HID report uses. Clamps: the report masks to 16 bits, so an
 * out-of-range value would silently land somewhere else on screen. */
void rp_fractions_to_hid(double fx, double fy, unsigned *x, unsigned *y);

/* Fractions -> device pixels, for logging and for APIs that speak pixels. */
void rp_fractions_to_pixels(double fx, double fy, rp_size device, double *px, double *py);

#ifdef __cplusplus
}
#endif

#endif /* RP_GEOMETRY_H */
