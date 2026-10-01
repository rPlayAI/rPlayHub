#include "twin_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

namespace rplayhub {

namespace {

constexpr float kPi = 3.14159265358979323846f;

float smoothstep01(float x) {
    float t = std::clamp(x, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Parse a float value for `"key": <number>` from a simple JSON string (RPLAYHUB_FOLD_STYLE).
void parseJsonFloat(const char* json, const char* key, float& out) {
    if (!json) return;
    std::string pat = std::string("\"") + key + "\"";
    const char* p = std::strstr(json, pat.c_str());
    if (!p) return;
    p = std::strchr(p + pat.size(), ':');
    if (!p) return;
    char* end = nullptr;
    float v = std::strtof(p + 1, &end);
    if (end != p + 1 && std::isfinite(v)) out = v;
}

struct PbrMaterial {
    float r, g, b;
    float metalness;
    float roughness;
};

// HeroFinish.graphite materials from HeroComposer.swift:
// - rail:    sRGB(0.42, 0.43, 0.46), metalness 0.98, roughness 0.08 (polished metal band)
// - body:    sRGB(0.14, 0.15, 0.16), metalness 0.15, roughness 0.55 (matte charcoal glass back)
// - chamfer: rail blended 34% white -> sRGB(0.62, 0.62, 0.64), metalness 1.0, roughness 0.04 (razor-thin bright line)
constexpr PbrMaterial kMatRail        = { 0.42f, 0.43f, 0.46f, 0.98f, 0.08f };
constexpr PbrMaterial kMatFace        = { 0.14f, 0.15f, 0.16f, 0.15f, 0.55f };
constexpr PbrMaterial kMatEdge        = { 0.62f, 0.63f, 0.65f, 1.00f, 0.04f };
constexpr PbrMaterial kMatGlass       = { 0.02f, 0.02f, 0.025f, 0.00f, 0.15f };
constexpr PbrMaterial kMatMatte       = { 0.09f, 0.10f, 0.105f, 0.00f, 0.85f };
constexpr PbrMaterial kMatButton      = { 0.46f, 0.47f, 0.50f, 0.96f, 0.12f };
constexpr PbrMaterial kMatIslandShell = { 0.05f, 0.052f, 0.058f, 0.65f, 0.22f };
constexpr PbrMaterial kMatIslandGlass = { 0.015f, 0.016f, 0.020f, 0.00f, 0.06f };
constexpr PbrMaterial kMatRingMetal   = { 0.26f, 0.27f, 0.30f, 0.90f, 0.16f };
constexpr PbrMaterial kMatLensGlass   = { 0.02f, 0.03f, 0.06f, 0.35f, 0.03f };
constexpr PbrMaterial kMatGoogleG     = { 0.25f, 0.26f, 0.28f, 0.35f, 0.35f };

// Analytical studio environment reflection matching HeroComposer.studioEnvironment() (1024x512):
// - Sky vertical gradient: 0.30 at top -> 0.10 at 0.42 -> 0.03 at 0.52 -> 0.01 at bottom
// - Three bright studio softbox strips above the horizon:
//   1. Key strip (1.0): x in [150, 620]/1024, y in [322, 335]/512
//   2. Opposite strip (0.72): x in [700, 950]/1024, y in [300, 308]/512
//   3. Corner glint square (1.0): x in [60, 106]/1024, y in [360, 406]/512
float sampleStudioEnvironment(float rx, float ry, float rz, float roughness) {
    float len = std::sqrt(rx * rx + ry * ry + rz * rz);
    if (len < 1e-6f) return 0.08f;
    rx /= len; ry /= len; rz /= len;

    // v_top: 0 at zenith (ry = +1), 1 at nadir (ry = -1)
    float v_top = std::clamp(0.5f - 0.5f * ry, 0.0f, 1.0f);
    float sky = 0.01f;
    if (v_top < 0.42f) {
        float t = v_top / 0.42f;
        sky = 0.30f * (1.0f - t) + 0.10f * t;
    } else if (v_top < 0.52f) {
        float t = (v_top - 0.42f) / 0.10f;
        sky = 0.10f * (1.0f - t) + 0.03f * t;
    } else {
        float t = (v_top - 0.52f) / 0.48f;
        sky = 0.03f * (1.0f - t) + 0.01f * t;
    }

    // Equirectangular u in [0, 1] (0.5 when looking toward +z, <0.5 left, >0.5 right)
    float u = 0.5f + std::atan2(rx, std::fabs(rz) + 0.15f) / (2.0f * kPi);
    // Softness kernel grows with roughness so polished chamfers (0.04) and rails (0.08)
    // catch razor-sharp bright lines while matte glass (0.55) gets a soft studio wash.
    float blur = 0.018f + roughness * roughness * 0.28f;
    auto box1d = [&](float val, float a, float b) {
        float s0 = smoothstep01((val - (a - blur)) / (2.0f * blur));
        float s1 = smoothstep01(((b + blur) - val) / (2.0f * blur));
        return s0 * s1;
    };
    // Convert AppKit bottom-origin y (out of 512) to v_top: v_top = 1 - y / 512
    float strip1 = 1.00f * box1d(u, 0.146f, 0.605f) * box1d(v_top, 1.0f - 335.0f / 512.0f, 1.0f - 322.0f / 512.0f);
    float strip2 = 0.72f * box1d(u, 0.684f, 0.928f) * box1d(v_top, 1.0f - 308.0f / 512.0f, 1.0f - 300.0f / 512.0f);
    float glint  = 1.00f * box1d(u, 0.058f, 0.104f) * box1d(v_top, 1.0f - 406.0f / 512.0f, 1.0f - 360.0f / 512.0f);
    // Attenuate sharpness slightly as roughness increases
    float peak_atten = 1.0f / (1.0f + roughness * 3.2f);
    return (sky + (strip1 + strip2 + glint) * peak_atten) * 1.7f;
}

ImU32 shadePBR(float px, float py, float pz,
               float nx, float ny, float nz,
               float cam_z,
               const PbrMaterial& mat,
               uint8_t alpha = 255) {
    float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (nl < 1e-6f) { nx = 0.0f; ny = 0.0f; nz = 1.0f; }
    else { nx /= nl; ny /= nl; nz /= nl; }

    float vx = 0.0f - px, vy = 0.0f - py, vz = cam_z - pz;
    float vl = std::sqrt(vx * vx + vy * vy + vz * vz);
    if (vl < 1e-6f) { vx = 0.0f; vy = 0.0f; vz = 1.0f; }
    else { vx /= vl; vy /= vl; vz /= vl; }

    float ndotv = std::max(0.0f, nx * vx + ny * vy + nz * vz);
    float rx = 2.0f * ndotv * nx - vx;
    float ry = 2.0f * ndotv * ny - vy;
    float rz = 2.0f * ndotv * nz - vz;

    // Studio environment specular + diffuse irradiance
    float env_spec = sampleStudioEnvironment(rx, ry, rz, mat.roughness);
    float env_diff = sampleStudioEnvironment(nx, ny, nz, 0.85f);

    // Three HeroComposer directional lights (key, rim, bevel) + gentle back-view bounce
    struct DirLight { float lx, ly, lz, intensity; };
    static const DirLight kLights[4] = {
        {  0.34f,  0.46f,  0.82f, 0.62f }, // key light (euler -0.5, 0.4, 0)
        { -0.78f,  0.30f,  0.55f, 0.72f }, // rim light (raking upper-left)
        { -0.51f,  0.77f,  0.38f, 1.22f }, // bevel light (-2, 3, 1.5): catches the 45° chamfer line
        { -0.32f,  0.48f, -0.82f, 0.45f }  // rear studio fill when phone turns around
    };

    float diff_sum = 0.16f + 0.22f * env_diff; // ambient (intensity 70) + studio irradiance
    float spec_sum = env_spec;
    float rough2 = std::max(0.025f, mat.roughness * mat.roughness);
    float shininess = std::min(180.0f, 2.0f / (rough2 * rough2));
    float spec_norm = (shininess + 2.0f) * 0.045f;

    for (const auto& L : kLights) {
        float ndotl = std::max(0.0f, nx * L.lx + ny * L.ly + nz * L.lz);
        if (ndotl <= 0.0f) continue;
        diff_sum += ndotl * L.intensity;
        float hx = L.lx + vx, hy = L.ly + vy, hz = L.lz + vz;
        float hl = std::sqrt(hx * hx + hy * hy + hz * hz);
        if (hl > 1e-6f) {
            hx /= hl; hy /= hl; hz /= hl;
            float ndoth = std::max(0.0f, nx * hx + ny * hy + nz * hz);
            spec_sum += L.intensity * spec_norm * std::pow(ndoth, shininess) * ndotl;
        }
    }

    // Schlick Fresnel: F0 = mix(0.04, baseColor, metalness)
    float f_pow = std::pow(1.0f - ndotv, 5.0f);
    float f0_r = 0.04f * (1.0f - mat.metalness) + mat.r * mat.metalness;
    float f0_g = 0.04f * (1.0f - mat.metalness) + mat.g * mat.metalness;
    float f0_b = 0.04f * (1.0f - mat.metalness) + mat.b * mat.metalness;
    float fr = f0_r + (1.0f - f0_r) * f_pow;
    float fg = f0_g + (1.0f - f0_g) * f_pow;
    float fb = f0_b + (1.0f - f0_b) * f_pow;

    float kd = (1.0f - mat.metalness);
    float out_r = kd * mat.r * diff_sum + fr * spec_sum;
    float out_g = kd * mat.g * diff_sum + fg * spec_sum;
    float out_b = kd * mat.b * diff_sum + fb * spec_sum;

    // Mild filmic tone mapping so hot chamfer glints roll off smoothly to white
    auto tonemap = [](float c) -> uint8_t {
        c = std::max(0.0f, c);
        c = c / (1.0f + 0.22f * c);
        return static_cast<uint8_t>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f));
    };
    return IM_COL32(tonemap(out_r), tonemap(out_g), tonemap(out_b), alpha);
}

// Build a CCW 2D rounded rectangle outline in (x, y) centered at (cx, cy).
std::vector<std::pair<float, float>> makeRoundedRectOutline(
    float cx, float cy, float w, float h, float r, int segs = 12) {
    std::vector<std::pair<float, float>> pts;
    r = std::clamp(r, 0.0f, 0.5f * std::min(w, h) - 1e-4f);
    const float ax[4] = { cx + w * 0.5f - r, cx - w * 0.5f + r, cx - w * 0.5f + r, cx + w * 0.5f - r };
    const float ay[4] = { cy + h * 0.5f - r, cy + h * 0.5f - r, cy - h * 0.5f + r, cy - h * 0.5f + r };
    for (int c = 0; c < 4; ++c) {
        for (int i = 0; i <= segs; ++i) {
            float a = (c * 90.0f + i * 90.0f / segs) * (kPi / 180.0f);
            pts.push_back({ ax[c] + r * std::cos(a), ay[c] + r * std::sin(a) });
        }
    }
    return pts;
}

// Compute outward 2D vertex normals for a CCW 2D outline.
std::vector<std::pair<float, float>> computeOutwardNormals2D(
    const std::vector<std::pair<float, float>>& outline) {
    const size_t n = outline.size();
    std::vector<std::pair<float, float>> norms(n, { 0.0f, 0.0f });
    for (size_t i = 0; i < n; ++i) {
        auto p_prev = outline[(i + n - 1) % n];
        auto p_curr = outline[i];
        auto p_next = outline[(i + 1) % n];
        float dx0 = p_curr.first - p_prev.first, dy0 = p_curr.second - p_prev.second;
        float l0 = std::hypot(dx0, dy0);
        float nx0 = l0 > 1e-6f ? (dy0 / l0) : 0.0f, ny0 = l0 > 1e-6f ? (-dx0 / l0) : 0.0f;

        float dx1 = p_next.first - p_curr.first, dy1 = p_next.second - p_curr.second;
        float l1 = std::hypot(dx1, dy1);
        float nx1 = l1 > 1e-6f ? (dy1 / l1) : 0.0f, ny1 = l1 > 1e-6f ? (-dx1 / l1) : 0.0f;

        float nx = nx0 + nx1, ny = ny0 + ny1;
        float nl = std::hypot(nx, ny);
        if (nl > 1e-6f) { nx /= nl; ny /= nl; }
        else { nx = nx1; ny = ny1; }
        norms[i] = { nx, ny };
    }
    return norms;
}

} // namespace

TwinView::TwinView() {
    loadReference();
    if (have_reference_) recenter_requested_ = false;
    loadStyleEnv();
    noteShown();
}

TwinView::~TwinView() {
    if (test_grid_tex_) {
        SDL_DestroyTexture(test_grid_tex_);
        test_grid_tex_ = nullptr;
    }
}

void TwinView::loadStyleEnv() {
    if (style_loaded_) return;
    style_loaded_ = true;
    const char* raw = std::getenv("RPLAYHUB_FOLD_STYLE");
    if (!raw || !*raw) return;
    parseJsonFloat(raw, "lock",  lock_amount_);
    parseJsonFloat(raw, "blur",  blur_px_);
    parseJsonFloat(raw, "dark",  dark_gain_);
    parseJsonFloat(raw, "start", dark_start_);
    parseJsonFloat(raw, "gamma", gamma_k_);
    parseJsonFloat(raw, "glass", stylized_glass_);
}

// ---- quaternion helpers (x, y, z, w) ----
TwinView::Vec3 TwinView::rotate(const Quat& q, Vec3 v) {
    float tx = 2.0f * (q.y * v.z - q.z * v.y);
    float ty = 2.0f * (q.z * v.x - q.x * v.z);
    float tz = 2.0f * (q.x * v.y - q.y * v.x);
    return { v.x + q.w * tx + (q.y * tz - q.z * ty),
             v.y + q.w * ty + (q.z * tx - q.x * tz),
             v.z + q.w * tz + (q.x * ty - q.y * tx) };
}

Quat TwinView::mul(const Quat& a, const Quat& b) {
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
             a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
             a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}

Quat TwinView::inverse(const Quat& q) { return { -q.x, -q.y, -q.z, q.w }; }

Quat TwinView::normalize(const Quat& q) {
    float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (l < 1e-6f) return { 0, 0, 0, 1 };
    return { q.x / l, q.y / l, q.z / l, q.w / l };
}

Quat TwinView::slerp(const Quat& a, const Quat& bin, float t) {
    Quat b = bin;
    float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (dot < 0.0f) { b = { -b.x, -b.y, -b.z, -b.w }; dot = -dot; }
    if (dot > 0.9995f) {
        return normalize({ a.x + t * (b.x - a.x), a.y + t * (b.y - a.y),
                           a.z + t * (b.z - a.z), a.w + t * (b.w - a.w) });
    }
    float theta = std::acos(dot);
    float s = std::sin(theta);
    float wa = std::sin((1.0f - t) * theta) / s, wb = std::sin(t * theta) / s;
    return { wa * a.x + wb * b.x, wa * a.y + wb * b.y,
             wa * a.z + wb * b.z, wa * a.w + wb * b.w };
}

void TwinView::setOrientation(const Quat& q, bool have) {
    latest_ = q;
    have_latest_ = have;
}

void TwinView::setFold(bool foldable, float hinge_deg, bool have_hinge) {
    foldable_ = foldable;
    float clamped = std::clamp(hinge_deg, 0.0f, 180.0f);
    if (clamped < 2.0f) clamped = 0.0f;
    else if (clamped > 178.0f) clamped = 180.0f;
    hinge_deg_ = clamped;
    have_hinge_ = have_hinge;
    if (!hinge_seeded_ && have_hinge) {
        hinge_shown_ = hinge_deg_;
        hinge_vel_ = 0.0f;
        hinge_seeded_ = true;
    }
}

void TwinView::setFoldOnly(bool fold_only) {
    if (fold_only_ != fold_only) {
        fold_only_ = fold_only;
        if (fold_only_) {
            user_yaw_ = 0.0f;
            user_pitch_ = 0.0f;
        }
        noteShown();
    }
}

void TwinView::setRenderMode(int mode) {
    int next = (mode % 3 + 3) % 3;
    if (render_mode_ != next) {
        render_mode_ = next;
        static const char* const kNames[3] = { "1 Hard cut", "2 Locked", "3 Stylized" };
        std::cerr << "fold: mode " << kNames[render_mode_]
                  << " (hinge " << static_cast<int>( std::lround(hinge_shown_)) << "°)\n";
    }
    noteShown();
}

void TwinView::noteShown() {
    readout_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(4);
}

bool TwinView::readoutVisible() const {
    return std::chrono::steady_clock::now() < readout_until_;
}

void TwinView::recenter() {
    recenter_requested_ = true;
    reference_samples_.clear();
    user_yaw_ = 0.0f;
    user_pitch_ = 0.0f;
}

void TwinView::addOrbit(float dyaw, float dpitch) {
    if (fold_only_) return;
    user_yaw_ += dyaw;
    user_pitch_ = std::clamp(user_pitch_ + dpitch, -1.35f, 1.35f);
}

std::string TwinView::referencePath() {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
    if (!home) home = std::getenv("APPDATA");
    std::string dir = std::string(home ? home : ".") + "/.config/rplayhub-android";
    _mkdir((std::string(home ? home : ".") + "/.config").c_str());
    _mkdir(dir.c_str());
#else
    const char* home = std::getenv("HOME");
    std::string dir = std::string(home ? home : ".") + "/.config/rplayhub-android";
    ::mkdir((std::string(home ? home : ".") + "/.config").c_str(), 0755);
    ::mkdir(dir.c_str(), 0755);
#endif
    return dir + "/twin-facing-me";
}

void TwinView::loadReference() {
    std::ifstream in(referencePath());
    Quat q;
    if (in >> q.x >> q.y >> q.z >> q.w) {
        reference_ = normalize(q);
        have_reference_ = true;
    }
}

void TwinView::saveReference() const {
    std::ofstream out(referencePath());
    out << reference_.x << " " << reference_.y << " " << reference_.z << " " << reference_.w << "\n";
}

// Built-in 512x512 procedural test grid matching Mac TwinView.testGrid() so Fold View
// works even without a connected phone or live video stream.
ImTextureID TwinView::ensureTestGrid(SDL_Renderer* renderer) {
    if (test_grid_tex_ || !renderer) return (ImTextureID)test_grid_tex_;
    const int W = 512, H = 512;
    std::vector<uint32_t> px(W * H, 0xFF2A201Cu);   // ABGR for RGBA32: (28, 32, 42, 255)
    auto set_px = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if (x >= 0 && x < W && y >= 0 && y < H) {
            px[y * W + x] = (255u << 24) | (static_cast<uint32_t>(b) << 16) |
                            (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(r);
        }
    };
    auto fill_rect = [&](int x0, int y0, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
        for (int y = y0; y < y0 + h; ++y)
            for (int x = x0; x < x0 + w; ++x)
                set_px(x, y, r, g, b);
    };
    // Grid lines every 32 px, brighter every 128 px
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            bool major = (x % 128 == 0) || (y % 128 == 0);
            bool minor = (x % 32 == 0) || (y % 32 == 0);
            if (major) set_px(x, y, 105, 120, 150);
            else if (minor) set_px(x, y, 56, 66, 86);
        }
    }
    // Diagonal coral line across both halves (top-left to bottom-right)
    for (int i = 0; i < W; ++i) {
        for (int t = -2; t <= 2; ++t) set_px(i, i + t, 255, 105, 97);
    }
    // Circle centered on the crease (256, 256), radius 180
    const float cx = W * 0.5f, cy = H * 0.5f, rad = 180.0f;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float d = std::hypot(x - cx, y - cy);
            if (std::fabs(d - rad) <= 2.5f) set_px(x, y, 90, 200, 250);
        }
    }
    // Yellow crease line down x = 256
    fill_rect(W / 2 - 1, 0, 3, H, 255, 210, 60);

    // Block letters: "L" on left half (x=110, y=236), "R" on right half (x=376, y=236), "TOP" at top
    // "L"
    fill_rect(104, 224, 8, 56, 235, 240, 250);
    fill_rect(104, 272, 36, 8, 235, 240, 250);
    // "R"
    fill_rect(368, 224, 8, 56, 235, 240, 250);
    fill_rect(368, 224, 32, 8, 235, 240, 250);
    fill_rect(392, 224, 8, 28, 235, 240, 250);
    fill_rect(368, 248, 32, 8, 235, 240, 250);
    for (int i = 0; i < 24; ++i) fill_rect(378 + i, 256 + i, 8, 4, 235, 240, 250);
    // "TOP" bar at top center
    fill_rect(224, 36, 20, 6, 255, 210, 60);
    fill_rect(231, 36, 6, 26, 255, 210, 60);
    fill_rect(250, 36, 20, 6, 255, 210, 60);
    fill_rect(250, 56, 20, 6, 255, 210, 60);
    fill_rect(250, 36, 6, 26, 255, 210, 60);
    fill_rect(264, 36, 6, 26, 255, 210, 60);
    fill_rect(276, 36, 6, 26, 255, 210, 60);
    fill_rect(276, 36, 18, 6, 255, 210, 60);
    fill_rect(276, 46, 18, 6, 255, 210, 60);
    fill_rect(288, 36, 6, 16, 255, 210, 60);

    test_grid_tex_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, W, H);
    if (test_grid_tex_) {
        SDL_UpdateTexture(test_grid_tex_, nullptr, px.data(), W * 4);
        SDL_SetTextureScaleMode(test_grid_tex_, SDL_ScaleModeLinear);
        SDL_SetTextureBlendMode(test_grid_tex_, SDL_BLENDMODE_BLEND);
    }
    return (ImTextureID)test_grid_tex_;
}

void TwinView::updatePose(float dt) {
    // Critically-damped spring on hinge_shown_ -> hinge_deg_ (omega = 28 rad/s)
    if (foldable_) {
        const float omega = 28.0f;
        const float err = hinge_deg_ - hinge_shown_;
        if (std::fabs(err) < 0.05f && std::fabs(hinge_vel_) < 0.2f) {
            hinge_shown_ = hinge_deg_;
            hinge_vel_ = 0.0f;
        } else {
            const float e = std::exp(-omega * dt);
            const float x0 = hinge_shown_ - hinge_deg_;
            const float v0 = hinge_vel_;
            hinge_shown_ = hinge_deg_ + (x0 + (v0 + omega * x0) * dt) * e;
            hinge_vel_   = (v0 - (v0 + omega * x0) * omega * dt) * e;
            hinge_shown_ = std::clamp(hinge_shown_, 0.0f, 180.0f);
        }
        if (have_hinge_ && std::fabs(hinge_deg_ - last_logged_hinge_) >= 5.0f) {
            last_logged_hinge_ = hinge_deg_;
            static const char* const kNames[3] = { "1 Hard cut", "2 Locked", "3 Stylized" };
            std::cerr << "fold: hinge " << static_cast<int>(std::lround(hinge_deg_))
                      << "° (shown " << static_cast<int>(std::lround(hinge_shown_))
                      << "°) " << kNames[render_mode_ % 3] << "\n";
        }
    }

    // In Fold View (fold_only_), the hinge is the sole subject: the phone sits face-on
    // (identity pose) and the gyroscope / orbit is not consulted so the picture never tilts away.
    if (fold_only_) {
        pose_ = { 0, 0, 0, 1 };
        return;
    }

    Quat base = { 0, 0, 0, 1 };
    if (have_latest_) {
        const Quat q = latest_;
        if (recenter_requested_) {
            if (reference_samples_.empty() ||
                (reference_samples_[0].x * q.x + reference_samples_[0].y * q.y +
                 reference_samples_[0].z * q.z + reference_samples_[0].w * q.w) >= 0.0f) {
                reference_samples_.push_back(q);
            } else {
                reference_samples_.push_back({ -q.x, -q.y, -q.z, -q.w });
            }
            if (reference_samples_.size() >= 12) {
                Quat acc{ 0, 0, 0, 0 };
                for (const auto& s : reference_samples_) {
                    acc.x += s.x; acc.y += s.y; acc.z += s.z; acc.w += s.w;
                }
                reference_ = normalize(acc);
                have_reference_ = true;
                saveReference();
                reference_samples_.clear();
                recenter_requested_ = false;
                have_smoothed_ = false;
            }
            base = have_smoothed_ ? smoothed_ : Quat{ 0, 0, 0, 1 };
        } else {
            const Quat ref = have_reference_ ? reference_ : q;
            Quat target = normalize(mul(inverse(ref), q));
            if (have_smoothed_) {
                float dot = std::min(1.0f, std::fabs(smoothed_.x * target.x + smoothed_.y * target.y +
                                                     smoothed_.z * target.z + smoothed_.w * target.w));
                float step_deg = 2.0f * std::acos(dot) * 180.0f / kPi;
                float alpha = step_deg < 0.35f ? 0.02f : std::clamp(0.10f + step_deg * 0.30f, 0.10f, 0.9f);
                smoothed_ = slerp(smoothed_, target, alpha);
            } else {
                smoothed_ = target;
                have_smoothed_ = true;
            }
            base = smoothed_;
        }
    }

    // Optional user drag orbit on top of sensor pose when in 3D view
    Quat q_yaw{ 0.0f, std::sin(user_yaw_ * 0.5f), 0.0f, std::cos(user_yaw_ * 0.5f) };
    Quat q_pitch{ std::sin(user_pitch_ * 0.5f), 0.0f, 0.0f, std::cos(user_pitch_ * 0.5f) };
    pose_ = normalize(mul(mul(q_yaw, q_pitch), base));
}

ImVec2 TwinView::project(Vec3 v) const {
    float z = cam_z_ - v.z;
    if (z < 0.05f) z = 0.05f;
    float sx = origin_.x + size_.x * 0.5f + v.x * focal_ / z;
    float sy = origin_.y + size_.y * 0.5f - v.y * focal_ / z;
    return ImVec2(sx, sy);
}

// Counter-rotate texture UV for a picture arriving `quadrants` quarter-turns from the
// physical portrait panel's orientation (TwinView.panelTransform in TwinView.swift).
static ImVec2 applyPanelTransform(float u, float v, int quadrants) {
    switch (((quadrants % 4) + 4) % 4) {
        case 1:  return ImVec2(v, 1.0f - u);
        case 2:  return ImVec2(1.0f - u, 1.0f - v);
        case 3:  return ImVec2(1.0f - v, u);
        default: return ImVec2(u, v);
    }
}

void TwinView::render(ImDrawList* dl, ImVec2 origin, ImVec2 size, ImTextureID screen_tex, ImTextureID back_tex,
                      int display_w, int display_h, int rotation, float scale,
                      ImTextureID outer_tex, SDL_Renderer* renderer, int outer_rotation) {
    const auto now = std::chrono::steady_clock::now();
    const float dt = last_tick_.time_since_epoch().count() == 0
                   ? (1.0f / 60.0f)
                   : std::clamp(std::chrono::duration<float>(now - last_tick_).count(), 0.001f, 0.1f);
    last_tick_ = now;

    // Built-in test grid when Fold View is opened with no phone connected or before frames arrive
    if (!screen_tex || display_w <= 0 || display_h <= 0) {
        screen_tex = ensureTestGrid(renderer);
        if (display_w <= 0 || display_h <= 0) {
            display_w = 2076;
            display_h = 2152;
        }
    }

    updatePose(dt);
    origin_ = origin;
    size_ = size;
    display_w_ = display_w;
    display_h_ = display_h;
    rotation_ = rotation;

    // The physical phone hardware does not change shape when turned to landscape in 3D:
    // its canonical portrait chassis has width <= height, while `pose_` turns the 3D body
    // and `applyPanelTransform(u, v, rotation_)` counter-rotates the texture on the glass.
    const int phys_w = std::min(display_w, display_h);
    const int phys_h = std::max(display_w, display_h);
    float raw_aspect = (phys_w > 0 && phys_h > 0)
                     ? static_cast<float>(phys_w) / phys_h
                     : (foldable_ ? 2076.0f / 2152.0f : 9.0f / 19.5f);

    if (foldable_) {
        // When a session starts with a foldable closed, only the narrow cover panel (~0.46 aspect)
        // is known; unfolded, the inner panel is twice as wide (~0.96 aspect). Match Mac c8cf607.
        float open_aspect = (raw_aspect < 0.7f) ? (raw_aspect * 2.0f) : raw_aspect;
        if (raw_aspect < 0.7f && !outer_tex && screen_tex) {
            outer_tex = screen_tex;
            outer_rotation = rotation;
            screen_tex = ensureTestGrid(renderer);
            rotation_ = 0;
        }
        panel_h_ = 1.65f;
        panel_w_ = panel_h_ * open_aspect;
        half_w_  = panel_w_ * 0.5f;
        const float bez = panel_w_ * 0.020f;
        body_w_  = (half_w_ + bez) * 2.0f;
        body_h_  = panel_h_ + bez * 2.0f;
        body_d_  = body_w_ * 0.025f;   // ultra-slim foldable leaf thickness (~4.2 mm / 155 mm)
        cam_z_   = 4.6f;
        const float hd = body_d_ * 0.5f;
        const float half_body = half_w_ + bez;
        // Frame camera so the open foldable fits horizontally AND the 90° swung leaf (z = hd + half_body)
        // fits vertically without clipping the top or bottom of the stage (matching Mac 97314f1).
        const float fit_x = size.x * (cam_z_ - hd) / (body_w_ * 1.12f);
        const float fit_y = size.y * (cam_z_ - (hd + half_body * 0.82f)) / (body_h_ * 1.10f);
        focal_ = std::min(fit_x, fit_y);

        renderFold(dl, size, screen_tex, back_tex, outer_tex, scale, outer_rotation);
        return;
    }

    // Bar phone proportions from HeroComposer.makeHeroPhone:
    // Black glass covers the bezel out to a razor-thin hairline chamfer at the outer rail.
    panel_h_ = 1.50f;
    panel_w_ = panel_h_ * raw_aspect;
    const float bez = panel_w_ * 0.022f;
    body_w_ = panel_w_ + 2.0f * bez;
    body_h_ = panel_h_ + 2.0f * bez;
    body_d_ = body_w_ * 0.074f;
    cam_z_  = 3.4f;
    float diag = std::sqrt(body_w_ * body_w_ + body_h_ * body_h_);
    focal_ = std::min(size.x, size.y) * 0.82f * cam_z_ / diag;

    const Quat R = pose_;
    auto tf = [&](float x, float y, float z) { return rotate(R, { x, y, z }); };
    auto tf_n = [&](float nx, float ny, float nz) { return rotate(R, { nx, ny, nz }); };
    auto depth = [&](const std::vector<Vec3>& pts) {
        float d = 0.0f;
        for (const auto& p : pts) d += p.z;
        return d / static_cast<float>(pts.size());
    };
    auto facing = [&](const std::vector<Vec3>& pts) {
        if (pts.size() < 3) return false;
        Vec3 a = pts[0], b = pts[1], c = pts[2];
        float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
        float vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
        float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        return nx * (0.0f - a.x) + ny * (0.0f - a.y) + nz * (cam_z_ - a.z) > 0.0f;
    };

    struct Face {
        std::vector<Vec3> pts;
        ImU32 col;
        float depth;
        int kind; // 0 = rail/chamfer/button, 1 = front glass, 2 = back face, 3 = camera island cap
    };
    std::vector<Face> faces;
    const float hw = body_w_ * 0.5f, hh = body_h_ * 0.5f, hd = body_d_ * 0.5f;
    const float corner = body_w_ * 0.105f;
    // Razor-thin radial chamfer (ch_xy) so the black glass covers the bezel and only a hairline
    // metallic rim is visible around the perimeter ("the razor-thin bright line the reference has").
    const float ch_xy = body_w_ * 0.0055f;
    const float ch_z  = body_d_ * 0.20f;
    auto outline = makeRoundedRectOutline(0.0f, 0.0f, body_w_, body_h_, corner, 14);
    auto norms   = computeOutwardNormals2D(outline);

    // Emit 3-ring extruded body (front chamfer + polished rail with antenna breaks + back chamfer)
    {
        std::vector<Vec3> front, back;
        for (size_t i = 0; i < outline.size(); ++i) {
            float ix = outline[i].first  - ch_xy * norms[i].first;
            float iy = outline[i].second - ch_xy * norms[i].second;
            front.push_back(tf(ix, iy,  hd));
            back.push_back(tf(ix, iy, -hd));
        }
        std::reverse(back.begin(), back.end());
        Vec3 nf = tf_n(0.0f, 0.0f,  1.0f);
        Vec3 nb = tf_n(0.0f, 0.0f, -1.0f);
        Vec3 pf = tf(0.0f, 0.0f,  hd);
        Vec3 pb = tf(0.0f, 0.0f, -hd);
        faces.push_back({ front, shadePBR(pf.x, pf.y, pf.z, nf.x, nf.y, nf.z, cam_z_, kMatGlass), depth(front), 1 });
        faces.push_back({ back,  shadePBR(pb.x, pb.y, pb.z, nb.x, nb.y, nb.z, cam_z_, kMatFace),  depth(back),  2 });

        const size_t n = outline.size();
        for (size_t i = 0; i < n; ++i) {
            size_t j = (i + 1) % n;
            float ax_o = outline[i].first, ay_o = outline[i].second;
            float bx_o = outline[j].first, by_o = outline[j].second;
            float ax_i = ax_o - ch_xy * norms[i].first, ay_i = ay_o - ch_xy * norms[i].second;
            float bx_i = bx_o - ch_xy * norms[j].first, by_i = by_o - ch_xy * norms[j].second;

            float mx = 0.5f * (ax_o + bx_o), my = 0.5f * (ay_o + by_o);
            float nx = 0.5f * (norms[i].first + norms[j].first);
            float ny = 0.5f * (norms[i].second + norms[j].second);
            float nl = std::hypot(nx, ny);
            if (nl > 1e-6f) { nx /= nl; ny /= nl; }

            // Antenna breaks near the corners (HeroComposer.swift L518-534)
            bool is_antenna = (std::fabs(std::fabs(my) - body_h_ * 0.40f) < body_w_ * 0.008f && std::fabs(mx) > hw * 0.92f) ||
                              (std::fabs(std::fabs(mx) - body_w_ * 0.32f) < body_w_ * 0.008f && std::fabs(my) > hh * 0.92f);
            const PbrMaterial& rail_mat = is_antenna ? kMatMatte : kMatRail;
            const PbrMaterial& edge_mat = is_antenna ? kMatMatte : kMatEdge;

            // 1. Front hairline chamfer strip (inset at +hd -> outer at +hd - ch_z)
            {
                std::vector<Vec3> q = {
                    tf(ax_i, ay_i, hd), tf(ax_o, ay_o, hd - ch_z),
                    tf(bx_o, by_o, hd - ch_z), tf(bx_i, by_i, hd)
                };
                Vec3 wn = tf_n(nx * 0.72f, ny * 0.72f, 0.69f);
                Vec3 wp = tf(mx, my, hd - ch_z * 0.5f);
                faces.push_back({ q, shadePBR(wp.x, wp.y, wp.z, wn.x, wn.y, wn.z, cam_z_, edge_mat), depth(q), 0 });
            }
            // 2. Center polished rail strip (outer at +hd - ch_z -> outer at -hd + ch_z)
            {
                std::vector<Vec3> q = {
                    tf(ax_o, ay_o,  hd - ch_z), tf(ax_o, ay_o, -hd + ch_z),
                    tf(bx_o, by_o, -hd + ch_z), tf(bx_o, by_o,  hd - ch_z)
                };
                Vec3 wn = tf_n(nx, ny, 0.0f);
                Vec3 wp = tf(mx, my, 0.0f);
                faces.push_back({ q, shadePBR(wp.x, wp.y, wp.z, wn.x, wn.y, wn.z, cam_z_, rail_mat), depth(q), 0 });
            }
            // 3. Back hairline chamfer strip (outer at -hd + ch_z -> inset at -hd)
            {
                std::vector<Vec3> q = {
                    tf(ax_o, ay_o, -hd + ch_z), tf(ax_i, ay_i, -hd),
                    tf(bx_i, by_i, -hd),        tf(bx_o, by_o, -hd + ch_z)
                };
                Vec3 wn = tf_n(nx * 0.72f, ny * 0.72f, -0.69f);
                Vec3 wp = tf(mx, my, -hd + ch_z * 0.5f);
                faces.push_back({ q, shadePBR(wp.x, wp.y, wp.z, wn.x, wn.y, wn.z, cam_z_, edge_mat), depth(q), 0 });
            }
        }
    }

    // Side buttons on the RIGHT rail: power key above, volume rocker below (HeroComposer.swift L536-550)
    auto add_button = [&](float by_center, float b_len) {
        const float bx0 = hw - body_d_ * 0.01f;
        const float bx1 = hw + body_d_ * 0.11f;
        const float by0 = by_center - b_len * 0.5f, by1 = by_center + b_len * 0.5f;
        const float bz0 = -body_d_ * 0.22f, bz1 = body_d_ * 0.22f;
        auto push_quad = [&](Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, float nx, float ny, float nz, const PbrMaterial& m) {
            std::vector<Vec3> q = { p0, p1, p2, p3 };
            Vec3 wn = tf_n(nx, ny, nz);
            Vec3 wp = { 0.25f * (p0.x + p1.x + p2.x + p3.x),
                        0.25f * (p0.y + p1.y + p2.y + p3.y),
                        0.25f * (p0.z + p1.z + p2.z + p3.z) };
            faces.push_back({ q, shadePBR(wp.x, wp.y, wp.z, wn.x, wn.y, wn.z, cam_z_, m), depth(q), 0 });
        };
        push_quad(tf(bx1, by0, bz1), tf(bx1, by0, bz0), tf(bx1, by1, bz0), tf(bx1, by1, bz1),  1.0f,  0.0f,  0.0f, kMatEdge);
        push_quad(tf(bx0, by0, bz1), tf(bx1, by0, bz1), tf(bx1, by1, bz1), tf(bx0, by1, bz1),  0.3f,  0.0f,  0.95f, kMatButton);
        push_quad(tf(bx1, by0, bz0), tf(bx0, by0, bz0), tf(bx0, by1, bz0), tf(bx1, by1, bz0),  0.3f,  0.0f, -0.95f, kMatButton);
        push_quad(tf(bx0, by1, bz1), tf(bx1, by1, bz1), tf(bx1, by1, bz0), tf(bx0, by1, bz0),  0.2f,  0.98f, 0.0f, kMatButton);
        push_quad(tf(bx0, by0, bz0), tf(bx1, by0, bz0), tf(bx1, by0, bz1), tf(bx0, by0, bz1),  0.2f, -0.98f, 0.0f, kMatButton);
    };
    add_button(body_h_ * 0.26f, body_h_ * 0.06f);
    add_button(body_h_ * 0.12f, body_h_ * 0.11f);

    // 3D Extruded Camera Island on the back (HeroComposer.makeCameraIsland, L905-956) when no custom back_tex
    const float islandW = body_w_ * 0.74f;
    const float islandH = body_h_ * 0.145f;
    const float islandD = body_d_ * 0.28f;
    const float centreY = body_h_ * 0.295f;
    const float ch_isl_xy = body_w_ * 0.0045f;
    const float ch_isl_z  = islandD * 0.25f;
    if (!back_tex) {
        auto isl_out = makeRoundedRectOutline(0.0f, centreY, islandW, islandH, islandH * 0.5f, 12);
        auto isl_nrm = computeOutwardNormals2D(isl_out);
        std::vector<Vec3> cap;
        const size_t ni = isl_out.size();
        for (size_t i = 0; i < ni; ++i) {
            size_t j = (i + 1) % ni;
            float ax_o = isl_out[i].first, ay_o = isl_out[i].second;
            float bx_o = isl_out[j].first, by_o = isl_out[j].second;
            float ax_i = ax_o - ch_isl_xy * isl_nrm[i].first, ay_i = ay_o - ch_isl_xy * isl_nrm[i].second;
            float bx_i = bx_o - ch_isl_xy * isl_nrm[j].first, by_i = by_o - ch_isl_xy * isl_nrm[j].second;
            cap.push_back(tf(ax_i, ay_i, -hd - islandD));

            float mx = 0.5f * (ax_o + bx_o), my = 0.5f * (ay_o + by_o);
            float nx = 0.5f * (isl_nrm[i].first + isl_nrm[j].first);
            float ny = 0.5f * (isl_nrm[i].second + isl_nrm[j].second);
            float nl = std::hypot(nx, ny);
            if (nl > 1e-6f) { nx /= nl; ny /= nl; }

            // Side wall (-hd -> -hd - islandD + ch_isl_z) in island shell metal
            {
                std::vector<Vec3> q = {
                    tf(ax_o, ay_o, -hd), tf(ax_o, ay_o, -hd - islandD + ch_isl_z),
                    tf(bx_o, by_o, -hd - islandD + ch_isl_z), tf(bx_o, by_o, -hd)
                };
                Vec3 wn = tf_n(nx, ny, 0.0f);
                Vec3 wp = tf(mx, my, -hd - islandD * 0.35f);
                faces.push_back({ q, shadePBR(wp.x, wp.y, wp.z, wn.x, wn.y, wn.z, cam_z_, kMatIslandShell), depth(q), 0 });
            }
            // Bright hairline chamfer rim (-hd - islandD + ch_isl_z -> -hd - islandD) in finish.chamfer
            {
                std::vector<Vec3> q = {
                    tf(ax_o, ay_o, -hd - islandD + ch_isl_z), tf(ax_i, ay_i, -hd - islandD),
                    tf(bx_i, by_i, -hd - islandD),            tf(bx_o, by_o, -hd - islandD + ch_isl_z)
                };
                Vec3 wn = tf_n(nx * 0.70f, ny * 0.70f, -0.71f);
                Vec3 wp = tf(mx, my, -hd - islandD + ch_isl_z * 0.5f);
                faces.push_back({ q, shadePBR(wp.x, wp.y, wp.z, wn.x, wn.y, wn.z, cam_z_, kMatEdge), depth(q), 0 });
            }
        }
        std::reverse(cap.begin(), cap.end());
        Vec3 wn = tf_n(0.0f, 0.0f, -1.0f);
        Vec3 wp = tf(0.0f, centreY, -hd - islandD);
        faces.push_back({ cap, shadePBR(wp.x, wp.y, wp.z, wn.x, wn.y, wn.z, cam_z_, kMatIslandGlass), depth(cap), 3 });
    }

    std::sort(faces.begin(), faces.end(), [](const Face& a, const Face& b) { return a.depth < b.depth; });

    {
        ImVec2 c = project({ 0.0f, -hh - 0.05f, 0.0f });
        dl->AddEllipseFilled(ImVec2(c.x, c.y + 10.0f * scale), ImVec2(size.x * 0.28f, size.y * 0.04f), IM_COL32(0, 0, 0, 60));
    }

    // Helper to draw the sculpted Google "G" emblem on a back face (-z) (TwinView.googleGImage)
    auto draw_google_g = [&](float gx, float gy, float gz, float g_size) {
        const float outer = g_size * 0.42f;
        const float inner = g_size * 0.27f;
        const float stroke = outer - inner;
        Vec3 wn = tf_n(0.0f, 0.0f, -1.0f);
        Vec3 wp = tf(gx, gy, gz);
        ImU32 col = shadePBR(wp.x, wp.y, wp.z, wn.x, wn.y, wn.z, cam_z_, kMatGoogleG);
        // When viewed from the back (-z), viewer-right is -x and viewer-up is +y.
        // Arc spans 62° CCW to 370° (leaving 10°..62° open in the upper-right).
        const int segs = 28;
        for (int i = 0; i < segs; ++i) {
            float a0 = (62.0f + (370.0f - 62.0f) * i / segs) * (kPi / 180.0f);
            float a1 = (62.0f + (370.0f - 62.0f) * (i + 1) / segs) * (kPi / 180.0f);
            ImVec2 p0 = project(tf(gx - outer * std::cos(a0), gy + outer * std::sin(a0), gz));
            ImVec2 p1 = project(tf(gx - outer * std::cos(a1), gy + outer * std::sin(a1), gz));
            ImVec2 p2 = project(tf(gx - inner * std::cos(a1), gy + inner * std::sin(a1), gz));
            ImVec2 p3 = project(tf(gx - inner * std::cos(a0), gy + inner * std::sin(a0), gz));
            dl->AddQuadFilled(p0, p1, p2, p3, col);
        }
        // Horizontal crossbar from center (dx=0) to right ring (dx = inner + stroke * 0.30)
        float x0 = gx, x1 = gx - (inner + stroke * 0.30f);
        float y0 = gy - stroke * 0.5f, y1 = gy + stroke * 0.5f;
        dl->AddQuadFilled(project(tf(x0, y1, gz)), project(tf(x1, y1, gz)),
                          project(tf(x1, y0, gz)), project(tf(x0, y0, gz)), col);
    };

    // Helper to draw a 3D protruding lens barrel on the camera island (HeroComposer.swift L966-988)
    auto draw_lens_barrel = [&](float lx, float ly, float lr, float z_base) {
        const float z_tip = z_base - islandD * 0.38f;
        const int segs = 20;
        // 3D cylindrical ring wall
        for (int i = 0; i < segs; ++i) {
            float a0 = (2.0f * kPi * i) / segs;
            float a1 = (2.0f * kPi * (i + 1)) / segs;
            float c0 = std::cos(a0), s0 = std::sin(a0);
            float c1 = std::cos(a1), s1 = std::sin(a1);
            std::vector<Vec3> wall = {
                tf(lx + lr * c0, ly + lr * s0, z_base),
                tf(lx + lr * c0, ly + lr * s0, z_tip),
                tf(lx + lr * c1, ly + lr * s1, z_tip),
                tf(lx + lr * c1, ly + lr * s1, z_base)
            };
            if (facing(wall)) {
                Vec3 wn = tf_n(0.5f * (c0 + c1), 0.5f * (s0 + s1), -0.2f);
                Vec3 wp = tf(lx + lr * c0, ly + lr * s0, 0.5f * (z_base + z_tip));
                ImU32 col = shadePBR(wp.x, wp.y, wp.z, wn.x, wn.y, wn.z, cam_z_, kMatRingMetal);
                dl->AddQuadFilled(project(wall[0]), project(wall[1]), project(wall[2]), project(wall[3]), col);
            }
        }
        // Metallic ring face + recessed sapphire glass dome + pupil + indigo coating glint
        const float r_in = lr * 0.84f;
        for (int i = 0; i < segs; ++i) {
            float a0 = (2.0f * kPi * i) / segs;
            float a1 = (2.0f * kPi * (i + 1)) / segs;
            float c0 = std::cos(a0), s0 = std::sin(a0);
            float c1 = std::cos(a1), s1 = std::sin(a1);
            Vec3 wn = tf_n(0.25f * (c0 + c1), 0.25f * (s0 + s1), -0.96f);
            Vec3 wp = tf(lx + lr * c0, ly + lr * s0, z_tip);
            ImU32 ring_col = shadePBR(wp.x, wp.y, wp.z, wn.x, wn.y, wn.z, cam_z_, kMatEdge);
            dl->AddQuadFilled(project(tf(lx + lr * c0,   ly + lr * s0,   z_tip)),
                              project(tf(lx + lr * c1,   ly + lr * s1,   z_tip)),
                              project(tf(lx + r_in * c1, ly + r_in * s1, z_tip)),
                              project(tf(lx + r_in * c0, ly + r_in * s0, z_tip)), ring_col);
        }
        std::vector<ImVec2> dome_pts, pupil_pts;
        Vec3 wn_d = tf_n(0.0f, 0.0f, -1.0f);
        Vec3 wp_d = tf(lx, ly, z_tip);
        ImU32 dome_col = shadePBR(wp_d.x, wp_d.y, wp_d.z, wn_d.x, wn_d.y, wn_d.z, cam_z_, kMatLensGlass);
        for (int i = 0; i < segs; ++i) {
            float a = (2.0f * kPi * i) / segs;
            dome_pts.push_back(project(tf(lx + r_in * std::cos(a), ly + r_in * std::sin(a), z_tip - 0.0005f)));
            pupil_pts.push_back(project(tf(lx + lr * 0.42f * std::cos(a), ly + lr * 0.42f * std::sin(a), z_tip - 0.001f)));
        }
        dl->AddConvexPolyFilled(dome_pts.data(), static_cast<int>(dome_pts.size()), dome_col);
        dl->AddConvexPolyFilled(pupil_pts.data(), static_cast<int>(pupil_pts.size()), IM_COL32(3, 5, 10, 255));
        // Coated indigo highlight glint (HeroComposer.swift L981-987)
        ImVec2 gc = project(tf(lx - lr * 0.28f, ly + lr * 0.28f, z_tip - 0.0015f));
        ImVec2 ge = project(tf(lx - lr * 0.08f, ly + lr * 0.28f, z_tip - 0.0015f));
        float gr = std::max(1.5f, std::hypot(ge.x - gc.x, ge.y - gc.y));
        dl->AddCircleFilled(gc, gr, IM_COL32(82, 122, 228, 235), 14);
    };

    std::vector<ImVec2> poly;
    for (const Face& f : faces) {
        if (!facing(f.pts)) continue;
        poly.clear();
        for (const auto& p : f.pts) poly.push_back(project(p));
        dl->AddConvexPolyFilled(poly.data(), static_cast<int>(poly.size()), f.col);

        if (f.kind == 2) {
            if (back_tex) {
                const int gx = 6, gy = 12;
                for (int j = 0; j < gy; ++j) for (int i = 0; i < gx; ++i) {
                    float x0 = -hw + body_w_ * i / gx, x1 = -hw + body_w_ * (i + 1) / gx;
                    float y0 = hh - body_h_ * j / gy, y1 = hh - body_h_ * (j + 1) / gy;
                    ImVec2 a = project(tf(x0, y0, -hd - 0.002f)), b = project(tf(x1, y0, -hd - 0.002f));
                    ImVec2 c = project(tf(x1, y1, -hd - 0.002f)), d = project(tf(x0, y1, -hd - 0.002f));
                    float u0 = 1.0f - static_cast<float>(i) / gx, u1 = 1.0f - static_cast<float>(i + 1) / gx;
                    float v0 = static_cast<float>(j) / gy, v1 = static_cast<float>(j + 1) / gy;
                    dl->AddImageQuad(back_tex, a, b, c, d, ImVec2(u0, v0), ImVec2(u1, v0), ImVec2(u1, v1), ImVec2(u0, v1));
                }
            } else {
                // Sculpted Google "G" emblem low on the back (HeroComposer.swift L1007-1017)
                draw_google_g(0.0f, -body_h_ * 0.05f, -hd - 0.0015f, body_w_ * 0.24f);
            }
        } else if (f.kind == 3 && !back_tex) {
            // Three lenses + flash + sensor on the camera island cap (HeroComposer.swift L965-1004)
            const float z_cap = -hd - islandD - 0.001f;
            const float lensR = islandH * 0.24f;
            const float lx_arr[3] = { islandW * 0.32f, islandW * 0.10f, -islandW * 0.12f };
            for (float lx : lx_arr) {
                draw_lens_barrel(lx, centreY, lensR, z_cap);
            }
            // Flash & sensor dot at the far end of the island
            auto draw_disc = [&](float dx, float dy, float dr, ImU32 col) {
                std::vector<ImVec2> pts;
                for (int i = 0; i < 18; ++i) {
                    float a = (2.0f * kPi * i) / 18.0f;
                    pts.push_back(project(tf(dx + dr * std::cos(a), dy + dr * std::sin(a), z_cap - 0.001f)));
                }
                dl->AddConvexPolyFilled(pts.data(), static_cast<int>(pts.size()), col);
            };
            draw_disc(-islandW * 0.34f, centreY, lensR * 0.46f, IM_COL32(252, 249, 238, 255));
            draw_disc(-islandW * 0.23f, centreY, lensR * 0.12f, IM_COL32(16, 16, 20, 255));
        } else if (f.kind == 1) {
            const int gx = 8, gy = 16;
            const float pw = panel_w_ * 0.5f, ph = panel_h_ * 0.5f, z = hd + 0.0025f;
            for (int j = 0; j < gy; ++j) for (int i = 0; i < gx; ++i) {
                float x0 = -pw + panel_w_ * i / gx, x1 = -pw + panel_w_ * (i + 1) / gx;
                float y0 = ph - panel_h_ * j / gy, y1 = ph - panel_h_ * (j + 1) / gy;
                ImVec2 a = project(tf(x0, y0, z)), b = project(tf(x1, y0, z));
                ImVec2 c = project(tf(x1, y1, z)), d = project(tf(x0, y1, z));
                float u0 = static_cast<float>(i) / gx, u1 = static_cast<float>(i + 1) / gx;
                float v0 = static_cast<float>(j) / gy, v1 = static_cast<float>(j + 1) / gy;
                if (screen_tex) {
                    dl->AddImageQuad(screen_tex, a, b, c, d,
                                     applyPanelTransform(u0, v0, rotation_),
                                     applyPanelTransform(u1, v0, rotation_),
                                     applyPanelTransform(u1, v1, rotation_),
                                     applyPanelTransform(u0, v1, rotation_));
                } else {
                    dl->AddQuadFilled(a, b, c, d, IM_COL32(0, 0, 0, 255));
                }
            }
            // Rounded screen corner spandrel mask (radius = corner - bezel, HeroComposer.swift L470-471)
            const float r_scr = std::max(0.01f, corner - bez);
            Vec3 wn = tf_n(0.0f, 0.0f, 1.0f);
            Vec3 wp = tf(0.0f, 0.0f, z + 0.0005f);
            ImU32 bezel_col = shadePBR(wp.x, wp.y, wp.z, wn.x, wn.y, wn.z, cam_z_, kMatGlass);
            const float cx_c[4] = { pw - r_scr, -pw + r_scr, -pw + r_scr,  pw - r_scr };
            const float cy_c[4] = { ph - r_scr,  ph - r_scr, -ph + r_scr, -ph + r_scr };
            const float sx_c[4] = { pw + 0.002f, -pw - 0.002f, -pw - 0.002f,  pw + 0.002f };
            const float sy_c[4] = { ph + 0.002f,  ph + 0.002f, -ph - 0.002f, -ph - 0.002f };
            for (int c = 0; c < 4; ++c) {
                ImVec2 corner_pt = project(tf(sx_c[c], sy_c[c], z + 0.0005f));
                for (int s = 0; s < 8; ++s) {
                    float a0 = (c * 90.0f + s * 90.0f / 8.0f) * (kPi / 180.0f);
                    float a1 = (c * 90.0f + (s + 1) * 90.0f / 8.0f) * (kPi / 180.0f);
                    ImVec2 p0 = project(tf(cx_c[c] + r_scr * std::cos(a0), cy_c[c] + r_scr * std::sin(a0), z + 0.0005f));
                    ImVec2 p1 = project(tf(cx_c[c] + r_scr * std::cos(a1), cy_c[c] + r_scr * std::sin(a1), z + 0.0005f));
                    dl->AddTriangleFilled(corner_pt, p0, p1, bezel_col);
                }
            }
            // Front punch-hole camera with deep indigo coating glint + speaker slit (HeroComposer.swift L482-516)
            const float hole_r = body_w_ * 0.016f;
            const float hole_y = ph - panel_w_ * 0.045f;
            ImVec2 hc = project(tf(0.0f, hole_y, z + 0.001f));
            ImVec2 he = project(tf(hole_r, hole_y, z + 0.001f));
            float hr_px = std::max(2.0f, std::hypot(he.x - hc.x, he.y - hc.y));
            dl->AddCircleFilled(hc, hr_px, IM_COL32(4, 4, 6, 255), 16);
            dl->AddCircleFilled(ImVec2(hc.x - hr_px * 0.22f, hc.y - hr_px * 0.22f),
                                std::max(1.0f, hr_px * 0.42f), IM_COL32(36, 48, 96, 230), 12);
        }
    }
}

// Mac Fold Model (TwinView.swift + HeroComposer.swift):
// - Hinge is on the LEFT:
//   * Half A (half == 0) is the RIGHT half (x_A in [0, +half_body]), held still in the phone frame.
//     Carries the right half of the inner picture (u in [0.5, 1.0]) and the Pixel 11 Pro Fold
//     dual-pill camera island + Google G on its back (-z).
//   * Half B (half == 1) is the LEFT half (x_B in [-half_body, 0]), hinged along the crease
//     at (x = 0, z = +hd). Swings toward the viewer (+z) by +phi = (180 - hinge_shown) deg
//     so its outer edge lifts toward the viewer and folds over onto Half A like a book cover
//     whose spine is on the left.
// - In-place sideways shift:
//   * shift_x = -half_body * (1 - max(cos(phi), 0)) / 2 keeps the phone centered whether
//     unfolded (shift = 0) or folded shut (shift = -half_body / 2).
void TwinView::renderFold(ImDrawList* dl, ImVec2 size, ImTextureID screen_tex, ImTextureID back_tex,
                          ImTextureID outer_tex, float scale, int outer_rotation) {
    (void)back_tex;
    const Quat R = pose_;
    const float phi = (180.0f - hinge_shown_) * (kPi / 180.0f);
    const float cphi = std::cos(phi), sphi = std::sin(phi);
    const float hw = half_w_, hh = panel_h_ * 0.5f;
    const float bez = panel_w_ * 0.020f;
    const float half_body = hw + bez;
    const float full_width = panel_w_ + 2.0f * bez;
    const float half_depth = body_d_;
    const float hd = half_depth * 0.5f;
    const float corner = full_width * 0.056f;
    // Razor-thin radial chamfer (ch_xy) so the black glass covers the bezel and only a hairline
    // metallic rim is visible around the perimeter ("the razor-thin bright line the reference has").
    const float ch_xy = half_body * 0.0065f;
    const float ch_z  = half_depth * 0.20f;

    // Slide both Half A and the hinge pivot left as the phone closes so it folds in place.
    shift_x_ = -half_body * (1.0f - std::max(cphi, 0.0f)) * 0.5f;

    // Transform a point in Half A (half == 0, lx in [0, +half_body]) or Half B (half == 1, lx in [-half_body, 0])
    // into Half A's unshifted coordinate frame (where the crease is at x = 0, z = +hd).
    auto to_half_a = [&](int half, float lx, float ly, float lz) -> Vec3 {
        if (half == 1) {
            const float rel_x = lx;          // <= 0
            const float rel_z = lz - hd;     // <= 0
            const float xa =  rel_x * cphi + rel_z * sphi;
            const float za = -rel_x * sphi + rel_z * cphi + hd;
            return { xa, ly, za };
        }
        return { lx, ly, lz };
    };
    auto place = [&](int half, float lx, float ly, float lz) -> Vec3 {
        Vec3 pA = to_half_a(half, lx, ly, lz);
        return rotate(R, { shift_x_ + pA.x, pA.y, pA.z });
    };
    auto place_n = [&](int half, float nx, float ny, float nz) -> Vec3 {
        if (half == 1) {
            float nxa =  nx * cphi + nz * sphi;
            float nza = -nx * sphi + nz * cphi;
            return rotate(R, { nxa, ny, nza });
        }
        return rotate(R, { nx, ny, nz });
    };
    auto depth = [&](const std::vector<Vec3>& pts) {
        float d = 0.0f;
        for (const auto& p : pts) d += p.z;
        return d / static_cast<float>(pts.size());
    };
    auto facing = [&](const std::vector<Vec3>& pts) {
        if (pts.size() < 3) return false;
        Vec3 a = pts[0], b = pts[1], c = pts[2];
        float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
        float vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
        float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        return nx * (0.0f - a.x) + ny * (0.0f - a.y) + nz * (cam_z_ - a.z) > 0.0f;
    };

    // Build half-outline with outer corners rounded (14 segments) and crease corners (x = 0) square
    // matching HeroComposer.halfPath (HeroComposer.swift L736-759)
    auto half_outline = [&](int half) {
        std::vector<std::pair<float, float>> pts;
        const float r = std::min(corner, std::min(half_body, hh + bez) - 1e-4f);
        const float y_top = hh + bez, y_bot = -hh - bez;
        const int segs = 14;
        auto arc = [&](float cx, float cy, float a0_deg, float a1_deg) {
            for (int i = 0; i <= segs; ++i) {
                float a = (a0_deg + (a1_deg - a0_deg) * i / segs) * (kPi / 180.0f);
                pts.push_back({ cx + r * std::cos(a), cy + r * std::sin(a) });
            }
        };
        if (half == 0) {
            // Right half A: x in [0, +half_body], CCW loop
            pts.push_back({ 0.0f, y_bot });
            arc(half_body - r, y_bot + r, -90.0f, 0.0f);
            arc(half_body - r, y_top - r, 0.0f, 90.0f);
            pts.push_back({ 0.0f, y_top });
        } else {
            // Left half B: x in [-half_body, 0], CCW loop
            pts.push_back({ 0.0f, y_top });
            arc(-half_body + r, y_top - r, 90.0f, 180.0f);
            arc(-half_body + r, y_bot + r, 180.0f, 270.0f);
            pts.push_back({ 0.0f, y_bot });
        }
        return pts;
    };

    // In Stylized mode (mode 2), the moving Half B turns slightly to glass mid-fold:
    // glass = stylized_glass_ * sin(phi), so Half A's bright right half shows faintly through
    // the frosted moving pane and returns to 100% opaque at 180° (flat) and 0° (shut).
    const float glass_b = (render_mode_ == 2) ? (stylized_glass_ * sphi) : 0.0f;
    const float alpha_b = std::clamp(1.0f - glass_b, 0.15f, 1.0f);
    const uint8_t alpha_b_u8 = static_cast<uint8_t>(std::lround(alpha_b * 255.0f));

    struct Face {
        std::vector<Vec3> pts;
        ImU32 col;
        float depth;
        int half;   // 0 = Right Half A (held), 1 = Left Half B (moving)
        int kind;   // 0 = rail/chamfer/button, 1 = inner glass face, 2 = outer back/cover face, 3 = camera pill cap
    };
    std::vector<Face> faces;

    for (int half = 0; half < 2; ++half) {
        const uint8_t a_u8 = (half == 1) ? alpha_b_u8 : 255;
        auto outline = half_outline(half);
        auto norms   = computeOutwardNormals2D(outline);
        // Keep the crease edge (x = 0) flush with zero x-inset so the two halves meet seamlessly at x = 0
        auto inset_pt = [&](size_t idx) -> std::pair<float, float> {
            float ox = outline[idx].first, oy = outline[idx].second;
            float dx = (std::fabs(ox) < 1e-4f) ? 0.0f : (ch_xy * norms[idx].first);
            float dy = ch_xy * norms[idx].second;
            return { ox - dx, oy - dy };
        };

        std::vector<Vec3> front, back;
        for (size_t i = 0; i < outline.size(); ++i) {
            auto ip = inset_pt(i);
            front.push_back(place(half, ip.first, ip.second,  hd));
            back.push_back(place(half, ip.first, ip.second, -hd));
        }
        std::reverse(back.begin(), back.end());
        Vec3 nf = place_n(half, 0.0f, 0.0f,  1.0f);
        Vec3 nb = place_n(half, 0.0f, 0.0f, -1.0f);
        Vec3 pf = place(half, (half == 0 ? 0.5f : -0.5f) * half_body, 0.0f,  hd);
        Vec3 pb = place(half, (half == 0 ? 0.5f : -0.5f) * half_body, 0.0f, -hd);
        const PbrMaterial& back_mat = (half == 1) ? kMatGlass : kMatFace;
        faces.push_back({ front, shadePBR(pf.x, pf.y, pf.z, nf.x, nf.y, nf.z, cam_z_, kMatGlass, a_u8), depth(front), half, 1 });
        faces.push_back({ back,  shadePBR(pb.x, pb.y, pb.z, nb.x, nb.y, nb.z, cam_z_, back_mat,  a_u8), depth(back),  half, 2 });

        const size_t n = outline.size();
        for (size_t i = 0; i < n; ++i) {
            size_t j = (i + 1) % n;
            float ax_o = outline[i].first, ay_o = outline[i].second;
            float bx_o = outline[j].first, by_o = outline[j].second;
            auto ai = inset_pt(i), bi = inset_pt(j);

            float mx = 0.5f * (ax_o + bx_o), my = 0.5f * (ay_o + by_o);
            float nx = 0.5f * (norms[i].first + norms[j].first);
            float ny = 0.5f * (norms[i].second + norms[j].second);
            float nl = std::hypot(nx, ny);
            if (nl > 1e-6f) { nx /= nl; ny /= nl; }

            // Antenna breaks near outer corners of each half
            bool is_antenna = (std::fabs(std::fabs(my) - body_h_ * 0.40f) < half_body * 0.014f && std::fabs(mx) > half_body * 0.90f) ||
                              (std::fabs(std::fabs(mx) - half_body * 0.58f) < half_body * 0.014f && std::fabs(my) > (hh + bez) * 0.92f);
            const PbrMaterial& rail_mat = is_antenna ? kMatMatte : kMatRail;
            const PbrMaterial& edge_mat = is_antenna ? kMatMatte : kMatEdge;

            // 1. Front hairline chamfer strip
            {
                std::vector<Vec3> q = {
                    place(half, ai.first, ai.second, hd), place(half, ax_o, ay_o, hd - ch_z),
                    place(half, bx_o, by_o, hd - ch_z),   place(half, bi.first, bi.second, hd)
                };
                Vec3 wn = place_n(half, nx * 0.72f, ny * 0.72f, 0.69f);
                Vec3 wp = place(half, mx, my, hd - ch_z * 0.5f);
                faces.push_back({ q, shadePBR(wp.x, wp.y, wp.z, wn.x, wn.y, wn.z, cam_z_, edge_mat, a_u8), depth(q), half, 0 });
            }
            // 2. Center polished rail strip
            {
                std::vector<Vec3> q = {
                    place(half, ax_o, ay_o,  hd - ch_z), place(half, ax_o, ay_o, -hd + ch_z),
                    place(half, bx_o, by_o, -hd + ch_z), place(half, bx_o, by_o,  hd - ch_z)
                };
                Vec3 wn = place_n(half, nx, ny, 0.0f);
                Vec3 wp = place(half, mx, my, 0.0f);
                faces.push_back({ q, shadePBR(wp.x, wp.y, wp.z, wn.x, wn.y, wn.z, cam_z_, rail_mat, a_u8), depth(q), half, 0 });
            }
            // 3. Back hairline chamfer strip
            {
                std::vector<Vec3> q = {
                    place(half, ax_o, ay_o, -hd + ch_z), place(half, ai.first, ai.second, -hd),
                    place(half, bi.first, bi.second, -hd), place(half, bx_o, by_o, -hd + ch_z)
                };
                Vec3 wn = place_n(half, nx * 0.72f, ny * 0.72f, -0.69f);
                Vec3 wp = place(half, mx, my, -hd + ch_z * 0.5f);
                faces.push_back({ q, shadePBR(wp.x, wp.y, wp.z, wn.x, wn.y, wn.z, cam_z_, edge_mat, a_u8), depth(q), half, 0 });
            }
        }
    }

    // Polished metallic hinge spine bridging Half A's crease edge and Half B's crease edge
    // around the pivot (x = 0, z = +hd) so the spine reads as a gleaming rounded hinge barrel when folding
    if (phi > 0.04f) {
        const int spine_segs = 10;
        const float y_top = hh + bez - ch_xy, y_bot = -hh - bez + ch_xy;
        for (int s = 0; s < spine_segs; ++s) {
            float t0 = static_cast<float>(s) / spine_segs;
            float t1 = static_cast<float>(s + 1) / spine_segs;
            float a0 = t0 * phi, a1 = t1 * phi;
            float x0 = -half_depth * std::sin(a0), z0 = hd - half_depth * std::cos(a0);
            float x1 = -half_depth * std::sin(a1), z1 = hd - half_depth * std::cos(a1);
            std::vector<Vec3> q = {
                place(0, x0, y_bot, z0), place(0, x1, y_bot, z1),
                place(0, x1, y_top, z1), place(0, x0, y_top, z0)
            };
            float am = 0.5f * (a0 + a1);
            Vec3 wn = place_n(0, -std::sin(am), 0.0f, -std::cos(am));
            Vec3 wp = place(0, -half_depth * std::sin(am), 0.0f, hd - half_depth * std::cos(am));
            faces.push_back({ q, shadePBR(wp.x, wp.y, wp.z, wn.x, wn.y, wn.z, cam_z_, kMatRail), depth(q), 0, 0 });
        }
    }

    // Side buttons on Half A's outer right rail (power key + volume rocker)
    auto add_fold_button = [&](float by_center, float b_len) {
        const float bx0 = half_body - half_depth * 0.01f;
        const float bx1 = half_body + half_depth * 0.14f;
        const float by0 = by_center - b_len * 0.5f, by1 = by_center + b_len * 0.5f;
        const float bz0 = -half_depth * 0.24f, bz1 = half_depth * 0.24f;
        auto push_quad = [&](Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, float nx, float ny, float nz, const PbrMaterial& m) {
            std::vector<Vec3> q = { p0, p1, p2, p3 };
            Vec3 wn = place_n(0, nx, ny, nz);
            Vec3 wp = { 0.25f * (p0.x + p1.x + p2.x + p3.x),
                        0.25f * (p0.y + p1.y + p2.y + p3.y),
                        0.25f * (p0.z + p1.z + p2.z + p3.z) };
            faces.push_back({ q, shadePBR(wp.x, wp.y, wp.z, wn.x, wn.y, wn.z, cam_z_, m), depth(q), 0, 0 });
        };
        push_quad(place(0, bx1, by0, bz1), place(0, bx1, by0, bz0), place(0, bx1, by1, bz0), place(0, bx1, by1, bz1), 1.0f, 0.0f, 0.0f, kMatEdge);
        push_quad(place(0, bx0, by0, bz1), place(0, bx1, by0, bz1), place(0, bx1, by1, bz1), place(0, bx0, by1, bz1), 0.3f, 0.0f, 0.95f, kMatButton);
        push_quad(place(0, bx1, by0, bz0), place(0, bx0, by0, bz0), place(0, bx0, by1, bz0), place(0, bx1, by1, bz0), 0.3f, 0.0f, -0.95f, kMatButton);
        push_quad(place(0, bx0, by1, bz1), place(0, bx1, by1, bz1), place(0, bx1, by1, bz0), place(0, bx0, by1, bz0), 0.2f, 0.98f, 0.0f, kMatButton);
        push_quad(place(0, bx0, by0, bz0), place(0, bx1, by0, bz0), place(0, bx1, by0, bz1), place(0, bx0, by0, bz1), 0.2f, -0.98f, 0.0f, kMatButton);
    };
    add_fold_button(body_h_ * 0.26f, body_h_ * 0.06f);
    add_fold_button(body_h_ * 0.12f, body_h_ * 0.11f);

    // 3D Extruded Dual-Pill Camera Island on Half A's back (-z) (HeroComposer.makeFoldCameraIsland, L797-903)
    const float pw_isl  = half_body * 0.56f;
    const float ph_isl  = body_h_ * 0.12f;
    const float isl_d   = half_depth * 0.36f;
    const float cx_isl  = half_body * 0.67f;
    const float upper_y = body_h_ * 0.5f - body_h_ * 0.03f - ph_isl * 0.5f;
    const float lower_y = upper_y - ph_isl;
    const float ch_pill_xy = pw_isl * 0.015f;
    const float ch_pill_z  = isl_d * 0.25f;

    const float pill_ys[2] = { upper_y, lower_y };
    for (int p_idx = 0; p_idx < 2; ++p_idx) {
        float py = pill_ys[p_idx];
        auto pill_out = makeRoundedRectOutline(cx_isl, py, pw_isl, ph_isl, ph_isl * 0.42f, 10);
        auto pill_nrm = computeOutwardNormals2D(pill_out);
        std::vector<Vec3> cap;
        const size_t np = pill_out.size();
        for (size_t i = 0; i < np; ++i) {
            size_t j = (i + 1) % np;
            float ax_o = pill_out[i].first, ay_o = pill_out[i].second;
            float bx_o = pill_out[j].first, by_o = pill_out[j].second;
            float ax_i = ax_o - ch_pill_xy * pill_nrm[i].first, ay_i = ay_o - ch_pill_xy * pill_nrm[i].second;
            float bx_i = bx_o - ch_pill_xy * pill_nrm[j].first, by_i = by_o - ch_pill_xy * pill_nrm[j].second;
            cap.push_back(place(0, ax_i, ay_i, -hd - isl_d));

            float mx = 0.5f * (ax_o + bx_o), my = 0.5f * (ay_o + by_o);
            float nx = 0.5f * (pill_nrm[i].first + pill_nrm[j].first);
            float ny = 0.5f * (pill_nrm[i].second + pill_nrm[j].second);
            float nl = std::hypot(nx, ny);
            if (nl > 1e-6f) { nx /= nl; ny /= nl; }

            // Pill side wall (-hd -> -hd - isl_d + ch_pill_z) in shell material
            {
                std::vector<Vec3> q = {
                    place(0, ax_o, ay_o, -hd), place(0, ax_o, ay_o, -hd - isl_d + ch_pill_z),
                    place(0, bx_o, by_o, -hd - isl_d + ch_pill_z), place(0, bx_o, by_o, -hd)
                };
                Vec3 wn = place_n(0, nx, ny, 0.0f);
                Vec3 wp = place(0, mx, my, -hd - isl_d * 0.35f);
                faces.push_back({ q, shadePBR(wp.x, wp.y, wp.z, wn.x, wn.y, wn.z, cam_z_, kMatIslandShell), depth(q), 0, 0 });
            }
            // Pill bright hairline chamfer rim (-hd - isl_d + ch_pill_z -> -hd - isl_d) in finish.chamfer
            {
                std::vector<Vec3> q = {
                    place(0, ax_o, ay_o, -hd - isl_d + ch_pill_z), place(0, ax_i, ay_i, -hd - isl_d),
                    place(0, bx_i, by_i, -hd - isl_d),             place(0, bx_o, by_o, -hd - isl_d + ch_pill_z)
                };
                Vec3 wn = place_n(0, nx * 0.70f, ny * 0.70f, -0.71f);
                Vec3 wp = place(0, mx, my, -hd - isl_d + ch_pill_z * 0.5f);
                faces.push_back({ q, shadePBR(wp.x, wp.y, wp.z, wn.x, wn.y, wn.z, cam_z_, kMatEdge), depth(q), 0, 0 });
            }
        }
        std::reverse(cap.begin(), cap.end());
        Vec3 wn = place_n(0, 0.0f, 0.0f, -1.0f);
        Vec3 wp = place(0, cx_isl, py, -hd - isl_d);
        // Tag kind = 3 + p_idx (3 = upper pill cap, 4 = lower pill cap)
        faces.push_back({ cap, shadePBR(wp.x, wp.y, wp.z, wn.x, wn.y, wn.z, cam_z_, kMatIslandGlass), depth(cap), 0, 3 + p_idx });
    }

    // Determine which of the two convex halves (Half A vs Half B) is further from the camera eye
    // (0, 0, cam_z_) in 3D space. The hinge crease (x_A = 0, z_A = +hd) defines a separating plane
    // between Half A and Half B, so drawing the further half completely before the nearer half
    // prevents any inter-half rail/face bleed-through at ANY 3D rotation or fold angle.
    Vec3 cA = place(0,  half_body * 0.5f, 0.0f, 0.0f);
    Vec3 cB = place(1, -half_body * 0.5f, 0.0f, 0.0f);
    auto cam_dist_sq = [&](Vec3 p) {
        return p.x * p.x + p.y * p.y + (p.z - cam_z_) * (p.z - cam_z_);
    };
    const float dist_sq_half[2] = { cam_dist_sq(cA), cam_dist_sq(cB) };

    std::sort(faces.begin(), faces.end(), [&](const Face& a, const Face& b) {
        if (a.half != b.half) return dist_sq_half[a.half] > dist_sq_half[b.half];
        return a.depth < b.depth;
    });

    // Soft stage shadow beneath the phone
    {
        ImVec2 c = project({ shift_x_ * 0.5f, -hh - bez - 0.05f, 0.0f });
        float sw = size.x * (0.16f + 0.14f * std::max(cphi, 0.0f));
        dl->AddEllipseFilled(ImVec2(c.x, c.y + 10.0f * scale), ImVec2(sw, size.y * 0.035f), IM_COL32(0, 0, 0, 65));
    }

    // Camera eye in Half A's unshifted coordinate frame (for space-locked ray-plane intersection)
    Vec3 eye_rot = rotate(inverse(R), { 0.0f, 0.0f, cam_z_ });
    Vec3 eye_A = { eye_rot.x - shift_x_, eye_rot.y, eye_rot.z };

    // Ray-plane intersection in Half A's frame against plane z_A = plane_z:
    auto project_onto_plane_a = [&](Vec3 pA, float plane_z, float& hit_x, float& hit_y) -> bool {
        float dz = pA.z - eye_A.z;
        if (std::fabs(dz) < 1e-6f) return false;
        float t = (plane_z - eye_A.z) / dz;
        if (t <= 0.0f) return false;
        hit_x = eye_A.x + t * (pA.x - eye_A.x);
        hit_y = eye_A.y + t * (pA.y - eye_A.y);
        return std::isfinite(hit_x) && std::isfinite(hit_y);
    };

    // Helper to emit a textured quad with per-vertex RGBA colors (hardware Gouraud shading)
    auto add_shaded_image_quad = [&](ImTextureID tex,
                                     ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3,
                                     ImVec2 uv0, ImVec2 uv1, ImVec2 uv2, ImVec2 uv3,
                                     ImU32 c0, ImU32 c1, ImU32 c2, ImU32 c3) {
        dl->PushTextureID(tex);
        dl->PrimReserve(6, 4);
        ImDrawIdx idx = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
        dl->_VtxWritePtr[0].pos = p0; dl->_VtxWritePtr[0].uv = uv0; dl->_VtxWritePtr[0].col = c0;
        dl->_VtxWritePtr[1].pos = p1; dl->_VtxWritePtr[1].uv = uv1; dl->_VtxWritePtr[1].col = c1;
        dl->_VtxWritePtr[2].pos = p2; dl->_VtxWritePtr[2].uv = uv2; dl->_VtxWritePtr[2].col = c2;
        dl->_VtxWritePtr[3].pos = p3; dl->_VtxWritePtr[3].uv = uv3; dl->_VtxWritePtr[3].col = c3;
        dl->_VtxWritePtr += 4;
        dl->_VtxCurrentIdx += 4;
        dl->_IdxWritePtr[0] = idx + 0; dl->_IdxWritePtr[1] = idx + 1; dl->_IdxWritePtr[2] = idx + 2;
        dl->_IdxWritePtr[3] = idx + 0; dl->_IdxWritePtr[4] = idx + 2; dl->_IdxWritePtr[5] = idx + 3;
        dl->_IdxWritePtr += 6;
        dl->PopTextureID();
    };

    // Helper to mask the two outer corners of a foldable half's screen (HeroComposer.halfMask, L763-781)
    auto draw_half_outer_corner_masks = [&](int half, float x_outer, float y_top_s, float y_bot_s, float z_s, uint8_t a_u8) {
        const float r_scr = std::max(0.01f, corner - bez);
        Vec3 wn = place_n(half, 0.0f, 0.0f, (z_s >= 0.0f) ? 1.0f : -1.0f);
        Vec3 wp = place(half, x_outer * 0.5f, 0.0f, z_s);
        ImU32 bezel_col = shadePBR(wp.x, wp.y, wp.z, wn.x, wn.y, wn.z, cam_z_, kMatGlass, a_u8);
        const float sign_x = (x_outer >= 0.0f) ? 1.0f : -1.0f;
        const float cx = x_outer - sign_x * r_scr;
        const float cy_arr[2] = { y_top_s - r_scr, y_bot_s + r_scr };
        const float sy_arr[2] = { y_top_s + 0.002f, y_bot_s - 0.002f };
        const float a0_arr[2] = { (sign_x > 0.0f) ? 0.0f : 90.0f, (sign_x > 0.0f) ? -90.0f : 180.0f };
        for (int c = 0; c < 2; ++c) {
            ImVec2 corner_pt = project(place(half, x_outer + sign_x * 0.002f, sy_arr[c], z_s));
            for (int s = 0; s < 8; ++s) {
                float a0 = (a0_arr[c] + s * 90.0f / 8.0f) * (kPi / 180.0f);
                float a1 = (a0_arr[c] + (s + 1) * 90.0f / 8.0f) * (kPi / 180.0f);
                ImVec2 p0 = project(place(half, cx + r_scr * std::cos(a0), cy_arr[c] + r_scr * std::sin(a0), z_s));
                ImVec2 p1 = project(place(half, cx + r_scr * std::cos(a1), cy_arr[c] + r_scr * std::sin(a1), z_s));
                dl->AddTriangleFilled(corner_pt, p0, p1, bezel_col);
            }
        }
    };

    // Helper to draw the Stylized crease seam gradient band (TwinView.makeSeams) on Half A or Half B
    auto draw_seam_band = [&](int half) {
        if (render_mode_ != 2) return;
        const float stylized_w = 0.28f;
        const float band = stylized_w * hw * std::min(1.0f, phi);
        if (band <= 0.0002f) return;
        const float z_seam = hd + 0.004f;
        const float x_crease = 0.0f;
        const float x_outer  = (half == 0) ? band : -band;
        const int gy_s = 12;
        const uint8_t dark_a = static_cast<uint8_t>(std::lround(140.0f * (half == 1 ? alpha_b : 1.0f)));
        const ImU32 col_crease = IM_COL32(0, 0, 0, dark_a);
        const ImU32 col_clear  = IM_COL32(0, 0, 0, 0);
        ImVec2 white_uv = ImGui::GetFontTexUvWhitePixel();
        ImTextureID font_tex = ImGui::GetIO().Fonts->TexID;
        for (int j = 0; j < gy_s; ++j) {
            float y0 = hh - panel_h_ * j / gy_s;
            float y1 = hh - panel_h_ * (j + 1) / gy_s;
            ImVec2 p_c0 = project(place(half, x_crease, y0, z_seam));
            ImVec2 p_o0 = project(place(half, x_outer,  y0, z_seam));
            ImVec2 p_o1 = project(place(half, x_outer,  y1, z_seam));
            ImVec2 p_c1 = project(place(half, x_crease, y1, z_seam));
            add_shaded_image_quad(font_tex, p_c0, p_o0, p_o1, p_c1,
                                  white_uv, white_uv, white_uv, white_uv,
                                  col_crease, col_clear, col_clear, col_crease);
        }
    };

    // Helper to draw a textured moving screen (either Half B's inner screen or Half B's outer cover screen)
    // supporting all 3 modes: 0 Hard cut, 1 Locked, 2 Stylized (iPhone Duo look: space-locked + blur + darkening gradient).
    auto draw_half_b_screen = [&](ImTextureID tex, bool is_cover, int tex_rot) {
        if (!tex) return;
        const int gx = 24, gy = 18;
        const float z_local = is_cover ? (-hd - 0.0025f) : (hd + 0.0025f);
        const float x_min = is_cover ? -(half_body - bez) : -hw;
        const float x_max = is_cover ? -bez               : -0.001f;
        const float y_max =  hh;
        const float y_min = -hh;
        const float cover_w = x_max - x_min;

        const float plane_z = is_cover ? (hd + half_depth) : hd;
        float cover_anchor_hx = -x_max;
        if (is_cover && (render_mode_ == 1 || render_mode_ == 2)) {
            float hx_edge = 0.0f, hy_edge = 0.0f;
            if (project_onto_plane_a(to_half_a(1, x_max, 0.0f, z_local), plane_z, hx_edge, hy_edge)) {
                cover_anchor_hx = hx_edge;
            }
        }

        const float m_gate = (render_mode_ == 2)
                           ? (is_cover ? smoothstep01((kPi - phi) / (kPi * 0.5f))
                                       : smoothstep01(phi / (kPi * 0.5f)))
                           : 0.0f;

        struct Vtx {
            ImVec2 pos;
            float u = 0.0f, v = 0.0f;
            float edge = 0.0f;
            float bright = 1.0f;
            bool inside = true;
        };
        std::vector<Vtx> grid((gx + 1) * (gy + 1));

        for (int j = 0; j <= gy; ++j) {
            const float fy = static_cast<float>(j) / gy;
            const float ly = y_max - (y_max - y_min) * fy;
            const float v_glued = fy;
            for (int i = 0; i <= gx; ++i) {
                const float fx = static_cast<float>(i) / gx;
                const float lx = x_min + (x_max - x_min) * fx;
                const float u_glued = is_cover ? (1.0f - fx) : (0.5f * fx);

                Vec3 pA = to_half_a(1, lx, ly, z_local);
                Vec3 world = rotate(R, { shift_x_ + pA.x, pA.y, pA.z });
                Vtx vtx;
                vtx.pos = project(world);
                vtx.u = u_glued;
                vtx.v = v_glued;
                vtx.inside = true;

                if (render_mode_ == 1 || render_mode_ == 2) {
                    float hx = 0.0f, hy = 0.0f;
                    if (project_onto_plane_a(pA, plane_z, hx, hy)) {
                        const float u_proj = is_cover
                                           ? ((hx - cover_anchor_hx) / std::max(cover_w, 1e-4f))
                                           : (0.5f + hx / panel_w_);
                        const float v_proj = 0.5f - hy / panel_h_;
                        vtx.u = u_glued + lock_amount_ * (u_proj - u_glued);
                        vtx.v = v_glued + lock_amount_ * (v_proj - v_glued);
                        vtx.inside = (vtx.u >= 0.0f && vtx.u <= 1.0f && vtx.v >= 0.0f && vtx.v <= 1.0f);
                    } else {
                        vtx.inside = false;
                    }
                }

                const float edge_u = is_cover ? std::clamp(vtx.u, 0.0f, 1.0f)
                                              : std::clamp((0.5f - vtx.u) / 0.5f, 0.0f, 1.0f);
                vtx.edge = edge_u;

                if (render_mode_ == 2) {
                    float d_norm = std::clamp((edge_u - dark_start_) / std::max(1.0f - dark_start_, 1e-4f), 0.0f, 1.0f);
                    float dark = std::min(1.0f, dark_gain_ * m_gate * std::pow(d_norm, gamma_k_));
                    vtx.bright = 1.0f - dark;
                }
                grid[j * (gx + 1) + i] = vtx;
            }
        }

        auto make_col = [&](float bright, float alpha_mul) -> ImU32 {
            uint8_t c = static_cast<uint8_t>(std::lround(std::clamp(bright, 0.0f, 1.0f) * 255.0f));
            uint8_t a = static_cast<uint8_t>(std::lround(std::clamp(alpha_b * alpha_mul, 0.0f, 1.0f) * 255.0f));
            return IM_COL32(c, c, c, a);
        };

        dl->AddQuadFilled(grid[0].pos,
                          grid[gx].pos,
                          grid[gy * (gx + 1) + gx].pos,
                          grid[gy * (gx + 1)].pos,
                          IM_COL32(0, 0, 0, alpha_b_u8));

        for (int j = 0; j < gy; ++j) {
            for (int i = 0; i < gx; ++i) {
                const Vtx& v00 = grid[j * (gx + 1) + i];
                const Vtx& v10 = grid[j * (gx + 1) + (i + 1)];
                const Vtx& v11 = grid[(j + 1) * (gx + 1) + (i + 1)];
                const Vtx& v01 = grid[(j + 1) * (gx + 1) + i];

                if (!v00.inside && !v10.inside && !v11.inside && !v01.inside) {
                    continue;
                }

                ImVec2 uv00 = applyPanelTransform(std::clamp(v00.u, 0.0f, 1.0f), std::clamp(v00.v, 0.0f, 1.0f), tex_rot);
                ImVec2 uv10 = applyPanelTransform(std::clamp(v10.u, 0.0f, 1.0f), std::clamp(v10.v, 0.0f, 1.0f), tex_rot);
                ImVec2 uv11 = applyPanelTransform(std::clamp(v11.u, 0.0f, 1.0f), std::clamp(v11.v, 0.0f, 1.0f), tex_rot);
                ImVec2 uv01 = applyPanelTransform(std::clamp(v01.u, 0.0f, 1.0f), std::clamp(v01.v, 0.0f, 1.0f), tex_rot);

                float b00 = v00.inside ? v00.bright : 0.0f;
                float b10 = v10.inside ? v10.bright : 0.0f;
                float b11 = v11.inside ? v11.bright : 0.0f;
                float b01 = v01.inside ? v01.bright : 0.0f;

                float avg_edge = 0.25f * (v00.edge + v10.edge + v11.edge + v01.edge);
                float blur_rad = (render_mode_ == 2) ? (blur_px_ * m_gate * std::pow(avg_edge, gamma_k_)) : 0.0f;

                if (blur_rad > 1.5f) {
                    const float du = std::min(0.035f, blur_rad / std::max(static_cast<float>(display_w_), 1024.0f));
                    const float dv = std::min(0.035f, blur_rad / std::max(static_cast<float>(display_h_), 1024.0f));
                    static const float kTapX[5] = { 0.0f, -0.85f,  0.85f, -0.45f,  0.45f };
                    static const float kTapY[5] = { 0.0f, -0.45f,  0.45f,  0.85f, -0.85f };
                    static const float kTapW[5] = { 0.36f, 0.22f,  0.22f,  0.22f,  0.22f };
                    for (int tap = 0; tap < 5; ++tap) {
                        ImVec2 o(kTapX[tap] * du, kTapY[tap] * dv);
                        auto shift_uv = [&](ImVec2 u) {
                            return ImVec2(std::clamp(u.x + o.x, 0.001f, 0.999f),
                                          std::clamp(u.y + o.y, 0.001f, 0.999f));
                        };
                        add_shaded_image_quad(
                            tex, v00.pos, v10.pos, v11.pos, v01.pos,
                            shift_uv(uv00), shift_uv(uv10), shift_uv(uv11), shift_uv(uv01),
                            make_col(b00, kTapW[tap]), make_col(b10, kTapW[tap]),
                            make_col(b11, kTapW[tap]), make_col(b01, kTapW[tap]));
                    }
                } else {
                    add_shaded_image_quad(
                        tex, v00.pos, v10.pos, v11.pos, v01.pos,
                        uv00, uv10, uv11, uv01,
                        make_col(b00, 1.0f), make_col(b10, 1.0f),
                        make_col(b11, 1.0f), make_col(b01, 1.0f));
                }
            }
        }
        // Round the outer corners of Half B's screen (HeroComposer.halfMask)
        draw_half_outer_corner_masks(1, x_min, y_max, y_min, z_local + (is_cover ? -0.0005f : 0.0005f), alpha_b_u8);
        if (is_cover) {
            // Cover display top punch-hole camera
            const float hole_r = half_body * 0.022f;
            const float hole_x = 0.5f * (x_min + x_max);
            const float hole_y = hh - half_body * 0.055f;
            ImVec2 hc = project(place(1, hole_x, hole_y, z_local - 0.001f));
            ImVec2 he = project(place(1, hole_x + hole_r, hole_y, z_local - 0.001f));
            float hr_px = std::max(1.8f, std::hypot(he.x - hc.x, he.y - hc.y));
            dl->AddCircleFilled(hc, hr_px, IM_COL32(4, 4, 6, alpha_b_u8), 14);
        }
    };

    // Helper to draw a 3D protruding lens barrel on Half A's camera island (HeroComposer.swift L846-865)
    auto draw_fold_lens = [&](float lx, float ly, float lr, float z_base) {
        const float z_tip = z_base - isl_d * 0.42f;
        const int segs = 20;
        for (int i = 0; i < segs; ++i) {
            float a0 = (2.0f * kPi * i) / segs;
            float a1 = (2.0f * kPi * (i + 1)) / segs;
            float c0 = std::cos(a0), s0 = std::sin(a0);
            float c1 = std::cos(a1), s1 = std::sin(a1);
            std::vector<Vec3> wall = {
                place(0, lx + lr * c0, ly + lr * s0, z_base),
                place(0, lx + lr * c0, ly + lr * s0, z_tip),
                place(0, lx + lr * c1, ly + lr * s1, z_tip),
                place(0, lx + lr * c1, ly + lr * s1, z_base)
            };
            if (facing(wall)) {
                Vec3 wn = place_n(0, 0.5f * (c0 + c1), 0.5f * (s0 + s1), -0.2f);
                Vec3 wp = place(0, lx + lr * c0, ly + lr * s0, 0.5f * (z_base + z_tip));
                ImU32 col = shadePBR(wp.x, wp.y, wp.z, wn.x, wn.y, wn.z, cam_z_, kMatRingMetal);
                dl->AddQuadFilled(project(wall[0]), project(wall[1]), project(wall[2]), project(wall[3]), col);
            }
        }
        const float r_in = lr * 0.86f;
        for (int i = 0; i < segs; ++i) {
            float a0 = (2.0f * kPi * i) / segs;
            float a1 = (2.0f * kPi * (i + 1)) / segs;
            float c0 = std::cos(a0), s0 = std::sin(a0);
            float c1 = std::cos(a1), s1 = std::sin(a1);
            Vec3 wn = place_n(0, 0.25f * (c0 + c1), 0.25f * (s0 + s1), -0.96f);
            Vec3 wp = place(0, lx + lr * c0, ly + lr * s0, z_tip);
            ImU32 ring_col = shadePBR(wp.x, wp.y, wp.z, wn.x, wn.y, wn.z, cam_z_, kMatEdge);
            dl->AddQuadFilled(project(place(0, lx + lr * c0,   ly + lr * s0,   z_tip)),
                              project(place(0, lx + lr * c1,   ly + lr * s1,   z_tip)),
                              project(place(0, lx + r_in * c1, ly + r_in * s1, z_tip)),
                              project(place(0, lx + r_in * c0, ly + r_in * s0, z_tip)), ring_col);
        }
        std::vector<ImVec2> dome_pts, pupil_pts;
        Vec3 wn_d = place_n(0, 0.0f, 0.0f, -1.0f);
        Vec3 wp_d = place(0, lx, ly, z_tip);
        ImU32 dome_col = shadePBR(wp_d.x, wp_d.y, wp_d.z, wn_d.x, wn_d.y, wn_d.z, cam_z_, kMatLensGlass);
        for (int i = 0; i < segs; ++i) {
            float a = (2.0f * kPi * i) / segs;
            dome_pts.push_back(project(place(0, lx + r_in * std::cos(a), ly + r_in * std::sin(a), z_tip - 0.0005f)));
            pupil_pts.push_back(project(place(0, lx + lr * 0.42f * std::cos(a), ly + lr * 0.42f * std::sin(a), z_tip - 0.001f)));
        }
        dl->AddConvexPolyFilled(dome_pts.data(), static_cast<int>(dome_pts.size()), dome_col);
        dl->AddConvexPolyFilled(pupil_pts.data(), static_cast<int>(pupil_pts.size()), IM_COL32(3, 5, 10, 255));
        // Coated indigo highlight glint (HeroComposer.swift L859-864)
        ImVec2 gc = project(place(0, lx - lr * 0.28f, ly + lr * 0.28f, z_tip - 0.0015f));
        ImVec2 ge = project(place(0, lx - lr * 0.08f, ly + lr * 0.28f, z_tip - 0.0015f));
        float gr = std::max(1.5f, std::hypot(ge.x - gc.x, ge.y - gc.y));
        dl->AddCircleFilled(gc, gr, IM_COL32(82, 122, 228, 235), 14);
    };

    // Helper to draw the sculpted Google "G" emblem on Half A's back face (-z) (HeroComposer.swift L890-901)
    auto draw_fold_google_g = [&](float gx, float gy, float gz, float g_size) {
        const float outer = g_size * 0.42f;
        const float inner = g_size * 0.27f;
        const float stroke = outer - inner;
        Vec3 wn = place_n(0, 0.0f, 0.0f, -1.0f);
        Vec3 wp = place(0, gx, gy, gz);
        ImU32 col = shadePBR(wp.x, wp.y, wp.z, wn.x, wn.y, wn.z, cam_z_, kMatGoogleG);
        const int segs = 28;
        for (int i = 0; i < segs; ++i) {
            float a0 = (62.0f + (370.0f - 62.0f) * i / segs) * (kPi / 180.0f);
            float a1 = (62.0f + (370.0f - 62.0f) * (i + 1) / segs) * (kPi / 180.0f);
            ImVec2 p0 = project(place(0, gx - outer * std::cos(a0), gy + outer * std::sin(a0), gz));
            ImVec2 p1 = project(place(0, gx - outer * std::cos(a1), gy + outer * std::sin(a1), gz));
            ImVec2 p2 = project(place(0, gx - inner * std::cos(a1), gy + inner * std::sin(a1), gz));
            ImVec2 p3 = project(place(0, gx - inner * std::cos(a0), gy + inner * std::sin(a0), gz));
            dl->AddQuadFilled(p0, p1, p2, p3, col);
        }
        float x0 = gx, x1 = gx - (inner + stroke * 0.30f);
        float y0 = gy - stroke * 0.5f, y1 = gy + stroke * 0.5f;
        dl->AddQuadFilled(project(place(0, x0, y1, gz)), project(place(0, x1, y1, gz)),
                          project(place(0, x1, y0, gz)), project(place(0, x0, y0, gz)), col);
    };

    std::vector<ImVec2> poly;
    for (const Face& f : faces) {
        if (!facing(f.pts)) continue;
        poly.clear();
        for (const auto& p : f.pts) poly.push_back(project(p));
        dl->AddConvexPolyFilled(poly.data(), static_cast<int>(poly.size()), f.col);

        if (f.kind == 1) {
            if (f.half == 0) {
                // Right Half A (held): carries the right half of the inner picture (u in [0.5, 1.0])
                const int gx = 10, gy = 16;
                const float z = hd + 0.0025f;
                for (int j = 0; j < gy; ++j) {
                    for (int i = 0; i < gx; ++i) {
                        float x0 = hw * i / gx, x1 = hw * (i + 1) / gx;
                        float y0 = hh - panel_h_ * j / gy, y1 = hh - panel_h_ * (j + 1) / gy;
                        ImVec2 a = project(place(0, x0, y0, z)), b = project(place(0, x1, y0, z));
                        ImVec2 c = project(place(0, x1, y1, z)), d = project(place(0, x0, y1, z));
                        float u0 = 0.5f + 0.5f * i / gx, u1 = 0.5f + 0.5f * (i + 1) / gx;
                        float v0 = static_cast<float>(j) / gy, v1 = static_cast<float>(j + 1) / gy;
                        if (screen_tex) {
                            dl->AddImageQuad(screen_tex, a, b, c, d,
                                             applyPanelTransform(u0, v0, rotation_),
                                             applyPanelTransform(u1, v0, rotation_),
                                             applyPanelTransform(u1, v1, rotation_),
                                             applyPanelTransform(u0, v1, rotation_));
                        }
                    }
                }
                // Round Half A's two outer right corners (HeroComposer.halfMask sign: +1)
                draw_half_outer_corner_masks(0, hw, hh, -hh, z + 0.0005f, 255);
                // Inner selfie punch-hole camera in upper-right corner of Half A
                const float hole_r = half_body * 0.020f;
                const float hole_x = hw * 0.76f;
                const float hole_y = hh - half_body * 0.055f;
                ImVec2 hc = project(place(0, hole_x, hole_y, z + 0.001f));
                ImVec2 he = project(place(0, hole_x + hole_r, hole_y, z + 0.001f));
                float hr_px = std::max(1.8f, std::hypot(he.x - hc.x, he.y - hc.y));
                dl->AddCircleFilled(hc, hr_px, IM_COL32(4, 4, 6, 255), 14);
                draw_seam_band(0);
            } else {
                // Left Half B (moving): inner screen with Hard cut / Locked / Stylized rendering
                draw_half_b_screen(screen_tex, /*is_cover=*/false, rotation_);
                draw_seam_band(1);
            }
        } else if (f.kind == 2) {
            if (f.half == 1) {
                // Left Half B's outer face: the cover screen!
                if (outer_tex) {
                    draw_half_b_screen(outer_tex, /*is_cover=*/true, outer_rotation);
                }
            } else {
                // Right Half A's outer back face (-z): sculpted Google "G" in the middle of the back
                // (HeroComposer.swift L890-901: gSize = halfWidth * 0.19, pos = (halfWidth / 2, bodyHeight * 0.016))
                draw_fold_google_g(half_body * 0.5f, body_h_ * 0.016f, -hd - 0.0015f, half_body * 0.19f);
            }
        } else if (f.kind == 3) {
            // Upper pill cap on Half A's back (HeroComposer.swift L874-884):
            // - Flash disc on the outer side (cx + pw * 0.26, upperY, r = pw * 0.08)
            // - Telephoto periscope rounded-rect window on the hinge side (cx - pw * 0.26, upperY, teleW = pw * 0.114, teleH = pw * 0.143)
            const float z_cap = -hd - isl_d - 0.001f;
            const float flash_x = cx_isl + pw_isl * 0.26f;
            const float flash_r = pw_isl * 0.08f;
            std::vector<ImVec2> f_pts;
            for (int i = 0; i < 18; ++i) {
                float a = (2.0f * kPi * i) / 18.0f;
                f_pts.push_back(project(place(0, flash_x + flash_r * std::cos(a), upper_y + flash_r * std::sin(a), z_cap - 0.001f)));
            }
            dl->AddConvexPolyFilled(f_pts.data(), static_cast<int>(f_pts.size()), IM_COL32(252, 249, 238, 255));

            const float tele_w = pw_isl * 0.114f, tele_h = pw_isl * 0.143f;
            const float tele_x = cx_isl - pw_isl * 0.26f;
            auto tele_out = makeRoundedRectOutline(tele_x, upper_y, tele_w, tele_h, tele_w * 0.22f, 6);
            std::vector<ImVec2> t_pts;
            for (const auto& tp : tele_out) {
                t_pts.push_back(project(place(0, tp.first, tp.second, z_cap - 0.001f)));
            }
            dl->AddConvexPolyFilled(t_pts.data(), static_cast<int>(t_pts.size()), IM_COL32(26, 30, 48, 255));
            dl->AddPolyline(t_pts.data(), static_cast<int>(t_pts.size()), IM_COL32(110, 115, 128, 220), ImDrawFlags_Closed, std::max(1.0f, 1.1f * scale));
            ImVec2 tg = project(place(0, tele_x - tele_w * 0.18f, upper_y + tele_h * 0.18f, z_cap - 0.0015f));
            dl->AddCircleFilled(tg, std::max(1.5f, 2.2f * scale), IM_COL32(82, 122, 228, 225), 10);
        } else if (f.kind == 4) {
            // Lower pill cap on Half A's back (HeroComposer.swift L885-889):
            // - Two 3D lens barrels (cx + pw * 0.26, r = pw * 0.10) and (cx - pw * 0.07, r = pw * 0.086)
            // - Sensor dot on the hinge side (cx - pw * 0.27, r = pw * 0.023)
            const float z_cap = -hd - isl_d - 0.001f;
            draw_fold_lens(cx_isl + pw_isl * 0.26f, lower_y, pw_isl * 0.10f,  z_cap);
            draw_fold_lens(cx_isl - pw_isl * 0.07f, lower_y, pw_isl * 0.086f, z_cap);
            const float sens_x = cx_isl - pw_isl * 0.27f;
            const float sens_r = pw_isl * 0.023f;
            std::vector<ImVec2> s_pts;
            for (int i = 0; i < 14; ++i) {
                float a = (2.0f * kPi * i) / 14.0f;
                s_pts.push_back(project(place(0, sens_x + sens_r * std::cos(a), lower_y + sens_r * std::sin(a), z_cap - 0.001f)));
            }
            dl->AddConvexPolyFilled(s_pts.data(), static_cast<int>(s_pts.size()), IM_COL32(18, 18, 24, 255));
        }
    }

    // Crease hairline along x_A = 0 when inner glass is visible
    if (hinge_shown_ > 15.0f) {
        ImVec2 ct = project(place(0, 0.0f,  hh, hd + 0.003f));
        ImVec2 cb = project(place(0, 0.0f, -hh, hd + 0.003f));
        uint8_t ca = static_cast<uint8_t>(std::clamp((180.0f - hinge_shown_) * 1.8f, 18.0f, 120.0f));
        dl->AddLine(ct, cb, IM_COL32(0, 0, 0, ca), std::max(1.0f, 1.5f * scale));
    }
}

bool TwinView::hitTest(ImVec2 mouse, int& out_x, int& out_y) const {
    if (focal_ <= 0.0f || display_w_ <= 0 || display_h_ <= 0) return false;
    float dx = (mouse.x - (origin_.x + size_.x * 0.5f)) / focal_;
    float dy = -(mouse.y - (origin_.y + size_.y * 0.5f)) / focal_;
    Vec3 o = { 0.0f, 0.0f, cam_z_ }, d = { dx, dy, -1.0f };
    float best_t = 1e30f;
    float best_u = -1.0f, best_v = -1.0f;
    const float hd = body_d_ * 0.5f;

    if (!foldable_) {
        Vec3 p0 = rotate(pose_, { 0.0f, 0.0f, hd });
        Vec3 n  = rotate(pose_, { 0.0f, 0.0f, 1.0f });
        float denom = n.x * d.x + n.y * d.y + n.z * d.z;
        // Only hit-test when the screen faces the camera (denom < 0); clicking the back orbits.
        if (denom < -1e-6f) {
            float t = (n.x * (p0.x - o.x) + n.y * (p0.y - o.y) + n.z * (p0.z - o.z)) / denom;
            if (t > 0.0f) {
                Vec3 hit = { o.x + d.x * t, o.y + d.y * t, o.z + d.z * t };
                Vec3 local = rotate(inverse(pose_), { hit.x - p0.x, hit.y - p0.y, hit.z - p0.z });
                float uu = local.x / panel_w_ + 0.5f;
                float vv = 0.5f - local.y / panel_h_;
                if (uu >= 0.0f && uu <= 1.0f && vv >= 0.0f && vv <= 1.0f) {
                    best_u = uu;
                    best_v = vv;
                }
            }
        }
    } else {
        const float phi = (180.0f - hinge_shown_) * (kPi / 180.0f);
        const bool cover_stream_active = (std::min(display_w_, display_h_) < std::max(display_w_, display_h_) * 0.7f);
        // 1. Intersect ray with both inner screen halves: Half A (right) and Half B (left)
        if (!cover_stream_active) {
            for (int half = 0; half < 2; ++half) {
                Quat turn = (half == 1)
                          ? Quat{ 0.0f, std::sin(phi * 0.5f), 0.0f, std::cos(phi * 0.5f) }
                          : Quat{ 0.0f, 0.0f, 0.0f, 1.0f };
                Quat Rh = normalize(mul(pose_, turn));
                Vec3 pivot_world = rotate(pose_, { shift_x_, 0.0f, hd });
                Vec3 n = rotate(Rh, { 0.0f, 0.0f, 1.0f });
                float denom = n.x * d.x + n.y * d.y + n.z * d.z;
                if (denom >= -1e-6f) continue;   // back-facing inner half
                float t = (n.x * (pivot_world.x - o.x) + n.y * (pivot_world.y - o.y) + n.z * (pivot_world.z - o.z)) / denom;
                if (t <= 0.0f || t >= best_t) continue;
                Vec3 hit = { o.x + d.x * t, o.y + d.y * t, o.z + d.z * t };
                Vec3 local = rotate(inverse(Rh), { hit.x - pivot_world.x, hit.y - pivot_world.y, hit.z - pivot_world.z });
                if (half == 0 && (local.x < 0.0f || local.x > half_w_)) continue;
                if (half == 1 && (local.x < -half_w_ || local.x > 0.0f)) continue;
                float uu = 0.5f + local.x / panel_w_;
                float vv = 0.5f - local.y / panel_h_;
                if (uu >= 0.0f && uu <= 1.0f && vv >= 0.0f && vv <= 1.0f) {
                    best_t = t;
                    best_u = uu;
                    best_v = vv;
                }
            }
        } else {
            // 2. Cover stream is active: hit-test Half B's outer cover face (-z normal in Half B's frame)
            Quat turn{ 0.0f, std::sin(phi * 0.5f), 0.0f, std::cos(phi * 0.5f) };
            Quat Rh = normalize(mul(pose_, turn));
            Vec3 pivot_world = rotate(pose_, { shift_x_, 0.0f, hd });
            Vec3 cover_origin = {
                pivot_world.x + rotate(Rh, { 0.0f, 0.0f, -body_d_ }).x,
                pivot_world.y + rotate(Rh, { 0.0f, 0.0f, -body_d_ }).y,
                pivot_world.z + rotate(Rh, { 0.0f, 0.0f, -body_d_ }).z
            };
            Vec3 n = rotate(Rh, { 0.0f, 0.0f, -1.0f });
            float denom = n.x * d.x + n.y * d.y + n.z * d.z;
            if (denom < -1e-6f) {
                float t = (n.x * (cover_origin.x - o.x) + n.y * (cover_origin.y - o.y) + n.z * (cover_origin.z - o.z)) / denom;
                if (t > 0.0f) {
                    Vec3 hit = { o.x + d.x * t, o.y + d.y * t, o.z + d.z * t };
                    Vec3 local = rotate(inverse(Rh), { hit.x - cover_origin.x, hit.y - cover_origin.y, hit.z - cover_origin.z });
                    // Cover screen spans local.x in [-half_w_, 0], with u=0 at the hinge edge (0) and u=1 at the outer edge (-half_w_)
                    float uu = -local.x / std::max(half_w_, 1e-4f);
                    float vv = 0.5f - local.y / panel_h_;
                    if (uu >= 0.0f && uu <= 1.0f && vv >= 0.0f && vv <= 1.0f) {
                        best_u = uu;
                        best_v = vv;
                    }
                }
            }
        }
    }

    if (best_u < 0.0f) return false;
    ImVec2 uv_t = applyPanelTransform(best_u, best_v, rotation_);
    out_x = static_cast<int>(std::clamp(uv_t.x, 0.0f, 1.0f) * display_w_);
    out_y = static_cast<int>(std::clamp(uv_t.y, 0.0f, 1.0f) * display_h_);
    return true;
}

} // namespace rplayhub
