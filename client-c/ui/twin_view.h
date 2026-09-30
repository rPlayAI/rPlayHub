#pragma once

// The 3D device twin: a phone-shaped slab that turns as the real one turns,
// driven by the rotation vector sensor, with the mirror texture-mapped onto
// its face and the back artwork on its back. For foldables (or in Fold View),
// builds a two-leaf hinged phone matching macOS TwinView.swift + HeroComposer.swift:
// Half A (right, x in [0, +hw]) is held still; Half B (left, x in [-hw, 0])
// swings toward the viewer (+z) around the crease on the left and folds over
// onto Half A, with three render modes:
//   1 Hard cut  — glued to glass
//   2 Locked    — content plane fixed in Half A's frame (ray-plane homography)
//   3 Stylized  — iPhone Duo look (space-locked + blur & darkening ramp + translucent glass)

#include "imgui.h"
#include <SDL2/SDL.h>
#include <chrono>
#include <string>
#include <vector>

namespace rplayhub {

struct Quat { float x = 0, y = 0, z = 0, w = 1; };

class TwinView {
public:
    using Quat = rplayhub::Quat;
    struct Vec3 { float x = 0, y = 0, z = 0; };

    TwinView();
    ~TwinView();

    void setOrientation(const Quat& q, bool have);
    void setFold(bool foldable, float hinge_deg, bool have_hinge);
    void setFoldOnly(bool fold_only);
    bool foldOnly() const { return fold_only_; }
    void setRenderMode(int mode);
    int  renderMode() const { return render_mode_; }
    void noteShown();
    bool readoutVisible() const;
    float shownHinge() const { return hinge_shown_; }

    void recenter();
    bool recentering() const { return recenter_requested_; }
    bool hasReference() const { return have_reference_; }
    void addOrbit(float dyaw, float dpitch);

    void render(ImDrawList* dl, ImVec2 origin, ImVec2 size, ImTextureID screen_tex, ImTextureID back_tex,
                int display_w, int display_h, int rotation, float scale,
                ImTextureID outer_tex = 0, SDL_Renderer* renderer = nullptr,
                int outer_rotation = 0);

    bool hitTest(ImVec2 mouse, int& out_x, int& out_y) const;

private:
    static Vec3 rotate(const Quat& q, Vec3 v);
    static Quat mul(const Quat& a, const Quat& b);
    static Quat inverse(const Quat& q);
    static Quat normalize(const Quat& q);
    static Quat slerp(const Quat& a, const Quat& b, float t);
    ImVec2 project(Vec3 v) const;
    void updatePose(float dt);
    void loadReference();
    void saveReference() const;
    static std::string referencePath();
    void loadStyleEnv();
    ImTextureID ensureTestGrid(SDL_Renderer* renderer);
    void renderFold(ImDrawList* dl, ImVec2 size, ImTextureID screen_tex, ImTextureID back_tex,
                    ImTextureID outer_tex, float scale, int outer_rotation);

    Quat latest_{};
    bool have_latest_ = false;
    Quat reference_{};
    bool have_reference_ = false;
    Quat smoothed_{};
    bool have_smoothed_ = false;
    bool recenter_requested_ = true;
    std::vector<Quat> reference_samples_;
    Quat pose_{};

    // Mouse-drag orbit when not in foldOnly mode
    float user_yaw_ = 0.0f;
    float user_pitch_ = 0.0f;

    // Foldable / Fold View state
    bool foldable_ = false;
    bool fold_only_ = false;
    float hinge_deg_ = 180.0f;
    float hinge_shown_ = 180.0f;
    float hinge_vel_ = 0.0f;
    bool have_hinge_ = false;
    bool hinge_seeded_ = false;
    float last_logged_hinge_ = -999.0f;
    int render_mode_ = 2;   // 0 Hard cut, 1 Locked, 2 Stylized (iPhone Duo look; default)

    std::chrono::steady_clock::time_point last_tick_{};
    std::chrono::steady_clock::time_point readout_until_{};

    // FoldStyle parameters (matching Mac TwinView.swift FoldStyle)
    bool style_loaded_ = false;
    float lock_amount_ = 1.0f;
    float blur_px_ = 72.0f;
    float dark_gain_ = 2.0f;
    float dark_start_ = 0.2f;
    float gamma_k_ = 1.35f;
    float stylized_glass_ = 0.10f;

    SDL_Texture* test_grid_tex_ = nullptr;

    // Cached from last frame for hit testing
    ImVec2 origin_{}, size_{};
    float focal_ = 0.0f, cam_z_ = 3.2f;
    float body_w_ = 0.0f, body_h_ = 0.0f, body_d_ = 0.0f;
    float panel_w_ = 0.0f, panel_h_ = 0.0f, half_w_ = 0.0f, shift_x_ = 0.0f;
    int display_w_ = 0, display_h_ = 0, rotation_ = 0;
};

} // namespace rplayhub
