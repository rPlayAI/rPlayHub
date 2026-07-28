#include "rp_geometry.h"

static double clamp01(double v)
{
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

rp_rect rp_fit_rect(rp_size view, rp_size content)
{
    rp_rect r;
    if (content.w <= 0.0 || content.h <= 0.0 || view.w <= 0.0 || view.h <= 0.0) {
        r.x = 0.0; r.y = 0.0; r.w = view.w; r.h = view.h;
        return r;
    }
    double sx = view.w / content.w;
    double sy = view.h / content.h;
    double scale = sx < sy ? sx : sy;
    r.w = content.w * scale;
    r.h = content.h * scale;
    r.x = (view.w - r.w) / 2.0;
    r.y = (view.h - r.h) / 2.0;
    return r;
}

rp_size rp_visible_fraction(rp_size video, rp_size device)
{
    rp_size f = { 1.0, 1.0 };
    if (video.w <= 0.0 || video.h <= 0.0 || device.w <= 0.0 || device.h <= 0.0) return f;
    f.w = device.w / video.w;
    f.h = device.h / video.h;
    if (f.w > 1.0) f.w = 1.0;
    if (f.h > 1.0) f.h = 1.0;
    return f;
}

rp_rect rp_video_layer_frame(rp_rect screen, rp_size video, rp_size device, int origin_top_left)
{
    rp_size f = rp_visible_fraction(video, device);
    rp_rect r;
    if (f.w <= 0.0 || f.h <= 0.0) {
        r.x = 0.0; r.y = 0.0; r.w = screen.w; r.h = screen.h;
        return r;
    }
    /* Scale the whole coded frame so its top-left screen region exactly fills `screen`. */
    r.w = screen.w / f.w;
    r.h = screen.h / f.h;
    r.x = 0.0;
    /* Anchor the frame's TOP edge to the clip's top edge. With a top-left origin that is y=0;
     * with a bottom-left origin the top edge is at the far end, so the frame starts negative. */
    r.y = origin_top_left ? 0.0 : (screen.h - r.h);
    return r;
}

int rp_normalize_point(double px, double py, rp_rect screen, int origin_top_left,
                       double *fx, double *fy)
{
    if (screen.w <= 0.0 || screen.h <= 0.0) return 0;
    if (px < screen.x || px > screen.x + screen.w) return 0;
    if (py < screen.y || py > screen.y + screen.h) return 0;

    double x = (px - screen.x) / screen.w;
    double y = (py - screen.y) / screen.h;
    if (!origin_top_left) y = 1.0 - y;      /* device origin is top-left */

    if (fx) *fx = clamp01(x);
    if (fy) *fy = clamp01(y);
    return 1;
}

void rp_fractions_to_hid(double fx, double fy, unsigned *x, unsigned *y)
{
    if (x) *x = (unsigned)(clamp01(fx) * 65535.0);
    if (y) *y = (unsigned)(clamp01(fy) * 65535.0);
}

void rp_fractions_to_pixels(double fx, double fy, rp_size device, double *px, double *py)
{
    if (px) *px = clamp01(fx) * device.w;
    if (py) *py = clamp01(fy) * device.h;
}
