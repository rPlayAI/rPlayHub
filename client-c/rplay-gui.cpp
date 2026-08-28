/* rplay-gui — the full Linux front-end: mirror pane + inspector tabs in one window.
 *
 * A SEPARATE app from rplay-view (which stays the untouched minimal reference viewer). Video
 * comes through stream-core.c — a verbatim copy of rplay-view's verified engine — and everything
 * else is a thin client over the engine's JSON API on 127.0.0.1:9876 (app/api/PROTOCOL.md), the
 * same API the macOS app's panels use. Dear ImGui over the SDL2 renderer: no new system deps
 * beyond a C++ compiler; deps/imgui + deps/json come from scripts/fetch-gui-deps.sh.
 *
 *   ┌────────────────────────────────────────────┐
 *   │ Device▾                    [status]        │
 *   ├──────────────┬─────────────────────────────┤
 *   │              │ Info Apps Console Files Diag│
 *   │   mirror     │ ─────────────────────────── │
 *   │ (live video  │        tab content          │
 *   │  + input)    │                             │
 *   └──────────────┴─────────────────────────────┘
 */
#include <SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include "nlohmann/json.hpp"

extern "C" {
#include "stream-core.h"
}

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

using json = nlohmann::json;

static const char *g_host = "127.0.0.1";

/* ------------------------------------------------------------------ JSON API worker
 *
 * One background thread owns a lazy connection to 9876 and runs requests in order; completions
 * are queued and their callbacks run on the UI thread (drained once per frame), so tab code
 * never blocks the interface and never touches ImGui from the wrong thread. */

struct ApiDone {
    std::function<void(bool ok, json reply)> cb;
    bool ok;
    json reply;
};

class Api {
public:
    void start() { worker_ = std::thread([this] { run(); }); }

    void request(const std::string &method, const json &params,
                 std::function<void(bool ok, json reply)> cb) {
        std::lock_guard<std::mutex> lk(mu_);
        reqs_.push_back({method, params, std::move(cb)});
        cv_.notify_one();
    }

    /* UI thread, once per frame. */
    void drain() {
        std::deque<ApiDone> done;
        {
            std::lock_guard<std::mutex> lk(mu_);
            done.swap(done_);
        }
        for (auto &d : done)
            if (d.cb) d.cb(d.ok, std::move(d.reply));
    }

private:
    struct Req { std::string method; json params; std::function<void(bool, json)> cb; };

    void run() {
        for (;;) {
            Req r;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return !reqs_.empty(); });
                r = std::move(reqs_.front());
                reqs_.pop_front();
            }
            json reply;
            bool ok = call(r.method, r.params, reply);
            std::lock_guard<std::mutex> lk(mu_);
            done_.push_back({std::move(r.cb), ok, std::move(reply)});
        }
    }

    bool call(const std::string &method, const json &params, json &reply) {
        for (int attempt = 0; attempt < 2; attempt++) {
            if (fd_ < 0) {
                fd_ = tcp_connect(g_host, 9876);
                if (fd_ < 0) { reply = "engine not reachable on 9876"; return false; }
            }
            json req = {{"id", ++id_}, {"method", method}};
            if (!params.is_null()) req["params"] = params;
            std::string line = req.dump() + "\n";
            if (send(fd_, line.data(), line.size(), MSG_NOSIGNAL) != (ssize_t)line.size()) {
                close(fd_); fd_ = -1; continue;   /* engine restarted: one re-dial */
            }
            std::string buf;
            char chunk[65536];
            for (;;) {
                ssize_t n = recv(fd_, chunk, sizeof chunk, 0);
                if (n <= 0) { close(fd_); fd_ = -1; break; }
                buf.append(chunk, (size_t)n);
                if (memchr(chunk, '\n', (size_t)n)) break;
            }
            if (fd_ < 0) continue;
            try {
                json r = json::parse(buf);
                if (r.value("ok", false)) { reply = r.value("result", json::object()); return true; }
                reply = r.contains("error") ? r["error"].value("message", "error") : "error";
                return false;
            } catch (...) { reply = "unparseable reply"; return false; }
        }
        reply = "engine not reachable on 9876";
        return false;
    }

    std::thread worker_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Req> reqs_;
    std::deque<ApiDone> done_;
    int fd_ = -1;
    int id_ = 0;
};

static Api g_api;

/* ------------------------------------------------------------------ syslog stream
 *
 * The one streaming method: a dedicated connection carries {"event":"syslog","line":...} objects
 * until the client closes. A ring of recent lines under a mutex; the Console tab renders it. */

static std::mutex g_slog_mu;
static std::deque<std::string> g_slog;
static std::atomic<bool> g_slog_on{false};
static std::atomic<int> g_slog_fd{-1};

static void syslog_thread()
{
    int fd = tcp_connect(g_host, 9876);
    if (fd < 0) { g_slog_on = false; return; }
    g_slog_fd = fd;
    const char req[] = "{\"id\":1,\"method\":\"syslog\"}\n";
    if (send(fd, req, sizeof req - 1, MSG_NOSIGNAL) != (ssize_t)(sizeof req - 1)) {
        close(fd); g_slog_fd = -1; g_slog_on = false; return;
    }
    std::string buf;
    char chunk[65536];
    while (g_slog_on) {
        ssize_t n = recv(fd, chunk, sizeof chunk, 0);
        if (n <= 0) break;
        buf.append(chunk, (size_t)n);
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            try {
                json j = json::parse(line);
                if (j.value("event", "") == "syslog") {
                    std::lock_guard<std::mutex> lk(g_slog_mu);
                    g_slog.push_back(j.value("line", ""));
                    while (g_slog.size() > 5000) g_slog.pop_front();
                }
            } catch (...) {}
        }
    }
    close(fd);
    g_slog_fd = -1;
    g_slog_on = false;
}

/* ------------------------------------------------------------------ base64 (for screenshots) */

static std::vector<uint8_t> b64_decode(const std::string &in)
{
    static int8_t map[256];
    static bool init = false;
    if (!init) {
        memset(map, -1, sizeof map);
        const char *al = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; i++) map[(uint8_t)al[i]] = (int8_t)i;
        init = true;
    }
    std::vector<uint8_t> out;
    out.reserve(in.size() * 3 / 4);
    int acc = 0, bits = 0;
    for (unsigned char c : in) {
        if (map[c] < 0) continue;
        acc = (acc << 6) | map[c];
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((uint8_t)(acc >> bits)); }
    }
    return out;
}

static std::string save_blob(const char *stem, const char *ext, const std::vector<uint8_t> &data)
{
    char path[512];
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    const char *home = getenv("HOME");
    snprintf(path, sizeof path, "%s/%s-%04d%02d%02d-%02d%02d%02d.%s", home ? home : ".",
             stem, tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ext);
    FILE *f = fopen(path, "wb");
    if (!f) return "";
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    return path;
}

/* ------------------------------------------------------------------ video (stream-core) */

static SDL_Renderer *g_ren;
static SDL_Texture *g_vtex;
static int g_vtex_w, g_vtex_h;          /* coded size */
static int g_active_w, g_active_h;
static int g_video_fd = -1;
static uint64_t g_video_retry_at;
static stream_state g_ss;
static annexb_parser g_parser;

static void on_video_frame(AVFrame *f, int active_w, int active_h)
{
    if (!g_vtex || g_vtex_w != f->width || g_vtex_h != f->height) {
        if (g_vtex) SDL_DestroyTexture(g_vtex);
        g_vtex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING,
                                   f->width, f->height);
        g_vtex_w = f->width;
        g_vtex_h = f->height;
    }
    SDL_UpdateYUVTexture(g_vtex, NULL, f->data[0], f->linesize[0],
                         f->data[1], f->linesize[1], f->data[2], f->linesize[2]);
    g_active_w = active_w;
    g_active_h = active_h;
}

static void video_reset_decoder()
{
    if (g_ss.dec) avcodec_free_context(&g_ss.dec);
    memset(&g_ss, 0, sizeof g_ss);
    g_ss.awaiting_keyframe = 1;
    g_ss.on_frame = on_video_frame;
    const AVCodec *codec = avcodec_find_decoder(g_h264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC);
    g_ss.dec = avcodec_alloc_context3(codec);
    g_ss.dec->thread_count = 1;   /* latency over throughput, same as rplay-view */
    avcodec_open2(g_ss.dec, codec, NULL);
    g_parser.len = 0;
}

/* Once per UI frame: drain whatever the socket has, feed the parser, apply the 20 ms idle flush.
 * Auto-(re)connects, so starting the GUI before the engine just works. */
static void video_pump()
{
    if (g_video_fd < 0) {
        uint64_t now = now_ms();
        if (now < g_video_retry_at) return;
        g_video_retry_at = now + 2000;
        g_h264 = stream_says_h264(g_host, 9876);
        int fd = tcp_connect(g_host, 9877);
        if (fd < 0) return;
        video_reset_decoder();
        g_video_fd = fd;
    }
    uint8_t chunk[65536];
    for (int i = 0; i < 64; i++) {   /* bounded per frame so the UI never stalls */
        ssize_t n = recv(g_video_fd, chunk, sizeof chunk, MSG_DONTWAIT);
        if (n > 0) {
            annexb_feed(&g_parser, chunk, (size_t)n, handle_nal, &g_ss);
        } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            close(g_video_fd);
            g_video_fd = -1;
            return;
        } else {
            break;
        }
    }
    if (g_ss.au_len && now_ms() - g_ss.last_nal_ms >= 20) flush_au(&g_ss);
}

/* ------------------------------------------------------------------ UI state */

static json g_devices = json::array();   /* list_devices result, refreshed periodically */
static uint64_t g_devices_at;
static int g_screen_w, g_screen_h;       /* TRUE device screen size (the coded frame has padding) */
static bool g_screen_size_probed;        /* one silent screenshot teaches the engine the size */
static std::string g_dev_name, g_dev_os; /* canvas header */

static json g_info;              /* device_info result */
static std::string g_info_err;
static bool g_info_pending;

static json g_apps = json::array();
static std::string g_apps_err;
static bool g_apps_pending;
static char g_apps_filter[128];

static json g_profiles;
static bool g_profiles_pending;

static json g_files;             /* list_dir result */
static std::string g_files_path = "/";
static int g_files_service = 0;  /* 0 media, 1 crash */
static std::string g_files_err;
static bool g_files_pending;

static json g_diag;              /* stream_info result */
static uint64_t g_diag_at;
static float g_diag_mbps[120];
static int g_diag_mbps_n;

static bool g_recording;
static std::string g_status;     /* one-line status bar message */
static uint64_t g_status_at;

static void set_status(const std::string &s)
{
    g_status = s;
    g_status_at = now_ms();
}

static void refresh_info()
{
    g_info_pending = true;
    g_api.request("device_info", nullptr, [](bool ok, json r) {
        g_info_pending = false;
        if (ok) { g_info = std::move(r); g_info_err.clear(); }
        else g_info_err = r.is_string() ? r.get<std::string>() : "failed";
    });
}

static void refresh_apps()
{
    g_apps_pending = true;
    g_api.request("list_apps", nullptr, [](bool ok, json r) {
        g_apps_pending = false;
        if (ok && r.is_array()) { g_apps = std::move(r); g_apps_err.clear(); }
        else g_apps_err = r.is_string() ? r.get<std::string>() : "failed";
    });
}

static void refresh_files()
{
    g_files_pending = true;
    json params = {{"service", g_files_service ? "crash" : "media"}, {"path", g_files_path}};
    g_api.request("list_dir", params, [](bool ok, json r) {
        g_files_pending = false;
        if (ok) { g_files = std::move(r); g_files_err.clear(); }
        else g_files_err = r.is_string() ? r.get<std::string>() : "failed";
    });
}

/* ------------------------------------------------------------------ tabs */

static void kv_row(const char *k, const std::string &v)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn(); ImGui::TextUnformatted(k);
    ImGui::TableNextColumn(); ImGui::TextUnformatted(v.c_str());
}

static std::string jstr(const json &j)
{
    if (j.is_string()) return j.get<std::string>();
    if (j.is_null()) return "";
    return j.dump();
}

static void tab_info()
{
    if (ImGui::Button("Refresh") || (!g_info_pending && g_info.is_null() && g_info_err.empty()))
        refresh_info();
    if (g_info_pending) { ImGui::SameLine(); ImGui::TextDisabled("loading..."); }
    if (!g_info_err.empty()) ImGui::TextColored(ImVec4(1, .4f, .4f, 1), "%s", g_info_err.c_str());
    if (!g_info.is_object()) return;

    if (ImGui::BeginTable("info", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        const json &d = g_info.value("default", json::object());
        for (auto &kv : d.items())
            kv_row(kv.key().c_str(), jstr(kv.value()));
        const json &bat = g_info.value("com.apple.mobile.battery", json::object());
        for (auto &kv : bat.items())
            kv_row(kv.key().c_str(), jstr(kv.value()));
        const json &disk = g_info.value("com.apple.disk_usage", json::object());
        for (auto &kv : disk.items()) {
            if (kv.value().is_number() && kv.value().get<double>() > 1e6) {
                char v[64];
                snprintf(v, sizeof v, "%.2f GB", kv.value().get<double>() / 1e9);
                kv_row(kv.key().c_str(), v);
            } else {
                kv_row(kv.key().c_str(), jstr(kv.value()));
            }
        }
        ImGui::EndTable();
    }
}

/* Device Hub's Apps `+`: a local .ipa path (this machine, not the device) staged into
 * /PublicStaging over AFC and installed via installation_proxy. ImGui has no native file picker
 * in this app -- every local path elsewhere (recordings, crash export) is a typed field too. */
static char g_install_app_path[512];

/* 0 All, 1 App Clips, 2 Default, 3 Developer -- read by the table loop below, written by the
 * Filter row at the bottom of the tab (Device Hub pins Filter + the category dropdown at the
 * very bottom of Apps, below the list and the +/- row, not above the list). */
static int g_apps_cat;
static const char *g_apps_cat_names[] = { "All Apps", "App Clips", "Default", "Developer" };

static void tab_apps()
{
    if (ImGui::Button("Refresh") || (!g_apps_pending && g_apps.empty() && g_apps_err.empty()))
        refresh_apps();
    if (g_apps_pending) { ImGui::SameLine(); ImGui::TextDisabled("loading..."); }
    ImGui::SameLine();
    ImGui::TextDisabled("%d apps", (int)g_apps.size());
    if (!g_apps_err.empty()) ImGui::TextColored(ImVec4(1, .4f, .4f, 1), "%s", g_apps_err.c_str());

    ImGui::SetNextItemWidth(-4 * ImGui::GetFontSize());
    ImGui::InputTextWithHint("##install_app_path", "Path to a local .ipa to install",
                             g_install_app_path, sizeof g_install_app_path);
    ImGui::SameLine();
    if (ImGui::Button("+##install_app") && g_install_app_path[0]) {
        std::string path = g_install_app_path;
        set_status("installing " + path + "...");
        g_api.request("install_app", {{"path", path}}, [path](bool ok, json r) {
            set_status(ok ? "installed " + path
                          : "install failed: " + (r.is_string() ? r.get<std::string>() : ""));
            if (ok) refresh_apps();
        });
    }

    int cat = g_apps_cat;
    /* List fills the tab except for one line reserved at the bottom for Filter + the category
     * dropdown, same trick tab_console uses for its own bottom filter bar. */
    float footer = ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("appslist", ImVec2(0, -footer), ImGuiChildFlags_None);
    int shown = 0;
    if (ImGui::BeginTable("apps", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp |
                          ImGuiTableFlags_BordersInnerH)) {  /* the hairline Device Hub draws between rows */
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthFixed, 5 * ImGui::GetFontSize());
        ImGui::TableSetupColumn("Bundle");
        ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, 8 * ImGui::GetFontSize());
        ImGui::TableHeadersRow();
        for (auto &a : g_apps) {
            std::string name = a.value("name", "");
            std::string bundle = a.value("bundleIdentifier", "");
            if (g_apps_filter[0] &&
                name.find(g_apps_filter) == std::string::npos &&
                bundle.find(g_apps_filter) == std::string::npos)
                continue;
            bool fp = a.value("isFirstParty", false);
            /* isDeveloper is get-task-allow out of Entitlements: true for a development-signed
             * build (what Xcode installs), false for an App Store or ad-hoc one. Device Hub's
             * "Developer" filter is this, not merely "not an Apple app" -- isFirstParty alone
             * matched every third-party app, App Store installs included. */
            bool dev = a.value("isDeveloper", false);
            bool clip = a.value("isAppClip", false);
            if (cat == 1 && !clip) continue; /* App Clips */
            if (cat == 2 && !fp) continue;   /* Default    = first-party */
            if (cat == 3 && !dev) continue;  /* Developer  = get-task-allow */
            shown++;
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(name.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(a.value("version", "").c_str());
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", bundle.c_str());
            ImGui::TableNextColumn();
            ImGui::PushID(bundle.c_str());
            if (ImGui::SmallButton("Launch")) {
                g_api.request("launch_app", {{"bundle_id", bundle}}, [name](bool ok, json r) {
                    set_status(ok ? "launched " + name
                                  : "launch failed: " + (r.is_string() ? r.get<std::string>() : ""));
                });
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("-")) ImGui::OpenPopup("uninstall?");
            if (ImGui::BeginPopupModal("uninstall?", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("Uninstall \"%s\"?", name.c_str());
                ImGui::TextDisabled("This removes the app and its data from the device.");
                if (ImGui::Button("Uninstall")) {
                    g_api.request("uninstall_app", {{"bundle_id", bundle}}, [name](bool ok, json r) {
                        set_status(ok ? "uninstalled " + name
                                      : "uninstall failed: " + (r.is_string() ? r.get<std::string>() : ""));
                        if (ok) refresh_apps();
                    });
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (shown == 0 && !g_apps.empty()) {
        /* Device Hub's own empty state ("No App Clips" etc.), confirmed against the live app --
         * rather than a silently blank list when a category filter matches nothing. */
        static const char *cat_label[] = { "Apps", "App Clips", "Apps", "Apps" };
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + avail.y * 0.4f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        const char *label = cat_label[g_apps_cat];
        char text[32]; snprintf(text, sizeof text, "No %s", label);
        float w = ImGui::CalcTextSize(text).x;
        ImGui::SetCursorPosX((avail.x - w) * 0.5f);
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    /* Bottom filter row: Filter field + category dropdown, full width, where Device Hub places
     * them. */
    ImGui::SetNextItemWidth(-13 * ImGui::GetFontSize());
    ImGui::InputTextWithHint("##filter", "Filter", g_apps_filter, sizeof g_apps_filter);
    ImGui::SameLine();
    /* Category dropdown, laid out like Device Hub: All Apps, a separator, then the kinds. */
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##cat", g_apps_cat_names[g_apps_cat])) {
        if (ImGui::Selectable(g_apps_cat_names[0], g_apps_cat == 0)) g_apps_cat = 0;
        ImGui::Separator();
        for (int i = 1; i < 4; i++)
            if (ImGui::Selectable(g_apps_cat_names[i], g_apps_cat == i)) g_apps_cat = i;
        ImGui::EndCombo();
    }
}

static void tab_console()
{
    static char filter[128];
    static bool autoscroll = true;

    /* Top row: the actions only (Device Hub keeps the log filter at the bottom, not here). */
    bool on = g_slog_on;
    if (ImGui::Button(on ? "Stop" : "Start")) {
        if (on) {
            g_slog_on = false;
            int fd = g_slog_fd.exchange(-1);
            if (fd >= 0) shutdown(fd, SHUT_RDWR);   /* unblocks the reader; it closes the fd */
        } else {
            g_slog_on = true;
            std::thread(syslog_thread).detach();
        }
    }
    ImGui::SameLine();
    ImGui::Checkbox("Follow", &autoscroll);
    ImGui::SameLine();
    if (ImGui::Button("Clear")) { std::lock_guard<std::mutex> lk(g_slog_mu); g_slog.clear(); }

    /* Log fills the tab except for one line reserved at the bottom for the filter bar. */
    float footer = ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("log", ImVec2(0, -footer), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::lock_guard<std::mutex> lk(g_slog_mu);
        for (auto &l : g_slog) {
            if (filter[0] && l.find(filter) == std::string::npos) continue;
            ImGui::TextUnformatted(l.c_str());
        }
    }
    if (autoscroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40)
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    /* Bottom filter input — full width, where Device Hub places it. */
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##logfilter", "Filter", filter, sizeof filter);
}

static void tab_files()
{
    const char *services[] = { "media", "crash" };
    ImGui::SetNextItemWidth(90);
    if (ImGui::Combo("##svc", &g_files_service, services, 2)) { g_files_path = "/"; refresh_files(); }
    ImGui::SameLine();
    if (ImGui::Button("Refresh") || (!g_files_pending && g_files.is_null() && g_files_err.empty()))
        refresh_files();
    ImGui::SameLine();
    ImGui::TextUnformatted(g_files_path.c_str());
    if (g_files_pending) { ImGui::SameLine(); ImGui::TextDisabled("loading..."); }
    if (!g_files_err.empty()) ImGui::TextColored(ImVec4(1, .4f, .4f, 1), "%s", g_files_err.c_str());

    if (ImGui::BeginTable("files", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                          ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 6 * ImGui::GetFontSize());
        ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, 4 * ImGui::GetFontSize());
        ImGui::TableHeadersRow();

        if (g_files_path != "/") {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable("..")) {
                size_t cut = g_files_path.find_last_of('/');
                g_files_path = cut == 0 ? "/" : g_files_path.substr(0, cut);
                refresh_files();
            }
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
        }
        if (g_files.is_object()) {
            for (auto &e : g_files.value("entries", json::array())) {
                std::string name = e.value("name", "");
                bool dir = e.value("is_dir", false);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(name.c_str());
                if (dir) {
                    if (ImGui::Selectable((name + "/").c_str())) {
                        g_files_path += (g_files_path == "/" ? "" : "/") + name;
                        refresh_files();
                    }
                } else {
                    ImGui::TextUnformatted(name.c_str());
                }
                ImGui::TableNextColumn();
                if (!dir) ImGui::Text("%lld", (long long)e.value("size", 0LL));
                ImGui::TableNextColumn();
                if (!dir && ImGui::SmallButton("Save")) {
                    std::string path = g_files_path + (g_files_path == "/" ? "" : "/") + name;
                    json params = {{"service", g_files_service ? "crash" : "media"}, {"path", path}};
                    g_api.request("read_file", params, [name](bool ok, json r) {
                        if (!ok) { set_status("read failed"); return; }
                        auto data = b64_decode(r.value("data_b64", ""));
                        size_t dot = name.find_last_of('.');
                        std::string saved = save_blob(
                            ("rplay-" + (dot == std::string::npos ? name : name.substr(0, dot))).c_str(),
                            dot == std::string::npos ? "bin" : name.c_str() + dot + 1, data);
                        set_status(saved.empty() ? "save failed" : "saved " + saved);
                    });
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}

/* Device Hub's Profiles `+`: a local .mobileprovision/.mobileconfig path, dispatched by
 * extension on the engine side (misagent vs. MCInstall). */
static char g_install_profile_path[512];

static void refresh_profiles()
{
    g_profiles_pending = true;
    g_api.request("list_profiles", nullptr, [](bool ok, json r) {
        g_profiles_pending = false;
        if (ok) g_profiles = std::move(r);
    });
}

/* Confirms, then removes a provisioning (by uuid) or configuration (by identifier) profile. */
static void remove_profile_button(const char *label, const char *name, const char *type, const char *id)
{
    ImGui::PushID(id);
    if (ImGui::SmallButton(label)) ImGui::OpenPopup("remove profile?");
    if (ImGui::BeginPopupModal("remove profile?", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Remove \"%s\"?", name);
        if (ImGui::Button("Remove")) {
            json params = {{"type", type}};
            params[!strcmp(type, "provisioning") ? "uuid" : "identifier"] = id;
            std::string nm = name;
            g_api.request("remove_profile", params, [nm](bool ok, json r) {
                set_status(ok ? "removed " + nm
                              : "remove failed: " + (r.is_string() ? r.get<std::string>() : ""));
                if (ok) refresh_profiles();
            });
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

static void tab_profiles()
{
    if (ImGui::Button("Refresh") || (!g_profiles_pending && g_profiles.is_null()))
        refresh_profiles();
    if (g_profiles_pending) { ImGui::SameLine(); ImGui::TextDisabled("loading..."); }

    ImGui::SetNextItemWidth(-4 * ImGui::GetFontSize());
    ImGui::InputTextWithHint("##install_profile_path", "Path to a local .mobileprovision or .mobileconfig",
                             g_install_profile_path, sizeof g_install_profile_path);
    ImGui::SameLine();
    if (ImGui::Button("+##install_profile") && g_install_profile_path[0]) {
        std::string path = g_install_profile_path;
        set_status("installing " + path + "...");
        g_api.request("install_profile", {{"path", path}}, [path](bool ok, json r) {
            set_status(ok ? "installed " + path
                          : "install failed: " + (r.is_string() ? r.get<std::string>() : ""));
            if (ok) refresh_profiles();
        });
    }

    if (!g_profiles.is_object()) return;

    ImGui::SeparatorText("Provisioning");
    if (ImGui::BeginTable("prov", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Team");
        ImGui::TableSetupColumn("Expires");
        ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, 3 * ImGui::GetFontSize());
        ImGui::TableHeadersRow();
        for (auto &p : g_profiles.value("provisioning", json::array())) {
            std::string name = p.value("name", "");
            std::string uuid = p.value("uuid", "");
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(name.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.value("team", "").c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(jstr(p.value("expires", json())).c_str());
            ImGui::TableNextColumn();
            if (!uuid.empty()) remove_profile_button("-", name.c_str(), "provisioning", uuid.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Configuration");
    if (ImGui::BeginTable("conf", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Organization");
        ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, 3 * ImGui::GetFontSize());
        ImGui::TableHeadersRow();
        for (auto &p : g_profiles.value("configuration", json::array())) {
            std::string name = p.value("name", "");
            std::string identifier = p.value("identifier", "");
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(name.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.value("organization", "").c_str());
            ImGui::TableNextColumn();
            if (!identifier.empty()) remove_profile_button("-", name.c_str(), "configuration", identifier.c_str());
        }
        ImGui::EndTable();
    }
}

static void tab_diagnostics()
{
    if (now_ms() - g_diag_at >= 1000) {
        g_diag_at = now_ms();
        g_api.request("stream_info", nullptr, [](bool ok, json r) {
            if (!ok) return;
            g_diag = std::move(r);
            float mbps = g_diag.value("mbps", 0.0f);
            if (g_diag_mbps_n < (int)(sizeof g_diag_mbps / sizeof *g_diag_mbps)) {
                g_diag_mbps[g_diag_mbps_n++] = mbps;
            } else {
                memmove(g_diag_mbps, g_diag_mbps + 1, sizeof g_diag_mbps - sizeof *g_diag_mbps);
                g_diag_mbps[g_diag_mbps_n - 1] = mbps;
            }
        });
    }
    if (!g_diag.is_object()) { ImGui::TextDisabled("no stream yet"); return; }

    ImGui::PlotLines("Mbps", g_diag_mbps, g_diag_mbps_n, 0, NULL, 0.0f, 8.0f, ImVec2(-1, 60));
    if (ImGui::BeginTable("diag", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        for (auto &kv : g_diag.items())
            kv_row(kv.key().c_str(), jstr(kv.value()));
        ImGui::EndTable();
    }
    ImGui::TextDisabled("active rect %dx%d, coded %dx%d", g_active_w, g_active_h, g_vtex_w, g_vtex_h);
}

/* ------------------------------------------------------------------ device actions */

static const char *g_confirm_action;   /* "sleep" / "restart" / "shutdown", NULL when closed */

static void do_screenshot()
{
    g_api.request("take_screenshot", nullptr, [](bool ok, json r) {
        if (!ok) { set_status("screenshot failed"); return; }
        auto png = b64_decode(r.value("image_b64", ""));
        std::string saved = save_blob("rplay-screenshot", "png", png);
        set_status(saved.empty() ? "save failed" : "saved " + saved);
    });
}

static void menu_device()
{
    if (ImGui::MenuItem("Take Screenshot")) do_screenshot();
    if (ImGui::MenuItem(g_recording ? "Stop Recording" : "Start Recording")) {
        const char *m = g_recording ? "stop_recording" : "start_recording";
        bool starting = !g_recording;
        g_api.request(m, nullptr, [starting](bool ok, json r) {
            if (!ok) { set_status("recording call failed"); return; }
            g_recording = starting;
            set_status(starting ? "recording (engine-side, recordings/)"
                                : "recording stopped: " + jstr(r.value("path", json())));
        });
    }
    if (ImGui::MenuItem("Press Home"))
        g_api.request("press_button", {{"button", "home"}},
                      [](bool ok, json) { if (!ok) set_status("home failed"); });
    ImGui::Separator();
    if (ImGui::MenuItem("Sleep (Lock)...")) g_confirm_action = "sleep";
    if (ImGui::MenuItem("Restart...")) g_confirm_action = "restart";
    if (ImGui::MenuItem("Shut Down...")) g_confirm_action = "shutdown";
}

static void confirm_modal()
{
    if (g_confirm_action && !ImGui::IsPopupOpen("Confirm"))
        ImGui::OpenPopup("Confirm");
    if (ImGui::BeginPopupModal("Confirm", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Really %s the device?", g_confirm_action ? g_confirm_action : "?");
        if (!strcmp(g_confirm_action, "restart"))
            ImGui::TextDisabled("The tunnel drops; the engine reconnects in about 45 s.");
        if (!strcmp(g_confirm_action, "shutdown"))
            ImGui::TextDisabled("The phone stays off until its side button is pressed.");
        ImGui::Separator();
        if (ImGui::Button("Yes", ImVec2(120, 0))) {
            std::string action = g_confirm_action;
            g_api.request("device_action", {{"action", action}}, [action](bool ok, json) {
                set_status(ok ? action + ": ok" : action + " failed");
            });
            g_confirm_action = NULL;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            g_confirm_action = NULL;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

/* ------------------------------------------------------------------ device sidebar */

static void device_sidebar()
{
    if (now_ms() - g_devices_at >= 5000) {
        g_devices_at = now_ms();
        g_api.request("list_devices", nullptr, [](bool ok, json r) {
            if (!(ok && r.is_object() && r["devices"].is_array())) return;
            g_devices = r["devices"];
            for (auto &d : g_devices) {
                if (!d.value("bound", false)) continue;
                g_dev_name = d.value("name", "");
                g_dev_os = d.value("os_version", "");
                g_screen_w = d.value("screen_width", 0);
                g_screen_h = d.value("screen_height", 0);
                /* The engine learns the true screen size from its first screenshot; take one
                 * silently so click mapping and the crop are exact (the coded frame carries
                 * 16-pixel alignment padding the screen does not have). */
                if (!g_screen_w && !g_screen_size_probed) {
                    g_screen_size_probed = true;
                    g_api.request("take_screenshot", nullptr, [](bool, json) { g_devices_at = 0; });
                }
            }
        });
    }
    static char search[64];
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##search", "Search", search, sizeof search);
    ImGui::SeparatorText("Available");
    if (g_devices.empty()) {
        ImGui::TextDisabled("no devices");
        return;
    }
    for (auto &d : g_devices) {
        std::string name = d.value("name", d.value("udid", "?"));
        if (search[0] && name.find(search) == std::string::npos) continue;
        bool bound = d.value("bound", false);
        ImGui::PushID(d.value("udid", "").c_str());
        if (ImGui::Selectable(("##" + name).c_str(), bound,
                              ImGuiSelectableFlags_None,
                              ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 2))) {
            /* One bound device today; selection becomes a bind call when the API grows one. */
        }
        ImVec2 rmin = ImGui::GetItemRectMin();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddText(ImVec2(rmin.x + 6, rmin.y + 2), ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
        char sub[96];
        snprintf(sub, sizeof sub, "%s · %s", d.value("product_type", "").c_str(),
                 d.value("os_version", "").c_str());
        dl->AddText(ImVec2(rmin.x + 6, rmin.y + 2 + ImGui::GetTextLineHeight()),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), sub);
        ImGui::PopID();
    }
}

/* ------------------------------------------------------------------ mirror pane */

static void mirror_pane()
{
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (!g_vtex || avail.x < 16 || avail.y < 16) {
        ImGui::TextDisabled(g_video_fd < 0 ? "connecting to the engine..." : "waiting for video...");
        return;
    }
    /* The coded frame is NOT the screen: 16-pixel alignment pads a 1170x2532 screen to
     * 1184x2576, screen anchored top-left. The downshift tiers are anamorphic squeezes of that
     * whole padded frame, so the visible screen occupies active*(screen/coded) of the texture.
     * Cropping there removes the dead edges AND makes click fractions exact (mapping against the
     * full frame drifts every tap toward the top-left by up to 1.7% — MirrorView.swift's rule). */
    float visx = g_screen_w ? (float)g_screen_w / g_vtex_w : 1.0f;
    float visy = g_screen_h ? (float)g_screen_h / g_vtex_h : 1.0f;
    ImVec2 uv1(visx, visy);
    if (g_active_w && g_active_h) {
        uv1.x *= (float)g_active_w / g_vtex_w;
        uv1.y *= (float)g_active_h / g_vtex_h;
    }
    float aspect = g_screen_w && g_screen_h ? (float)g_screen_w / g_screen_h
                                            : (float)g_vtex_w / g_vtex_h;
    float scale = avail.x / aspect < avail.y ? avail.x / aspect : avail.y;
    ImVec2 size(aspect * scale, scale);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    pos.x += (avail.x - size.x) / 2;
    pos.y += (avail.y - size.y) / 2;
    ImGui::SetCursorScreenPos(pos);
    ImGui::Image((ImTextureID)(intptr_t)g_vtex, size, ImVec2(0, 0), uv1);

    /* Input: same gesture model as rplay-view / the macOS MirrorView. */
    static ImVec2 down_frac;
    static uint64_t down_at;
    static bool down;
    if (ImGui::IsItemHovered() || down) {
        ImVec2 m = ImGui::GetMousePos();
        double fx = (m.x - pos.x) / size.x, fy = (m.y - pos.y) / size.y;
        fx = fx < 0 ? 0 : fx > 1 ? 1 : fx;
        fy = fy < 0 ? 0 : fy > 1 ? 1 : fy;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            down_frac = ImVec2((float)fx, (float)fy);
            down_at = now_ms();
            down = true;
        }
        if (down && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            down = false;
            double dx = fx - down_frac.x, dy = fy - down_frac.y;
            uint64_t held = now_ms() - down_at;
            if (dx * dx + dy * dy < 0.02 * 0.02) {
                g_api.request("tap", {{"fx", down_frac.x}, {"fy", down_frac.y}}, nullptr);
            } else {
                unsigned dur = held < 60 ? 60 : held > 2000 ? 2000 : (unsigned)held;
                g_api.request("swipe", {{"fx0", down_frac.x}, {"fy0", down_frac.y},
                                        {"fx1", fx}, {"fy1", fy}, {"duration_ms", dur}}, nullptr);
            }
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
            ImGui::OpenPopup("device_actions");
    }
    /* Device actions live on the mirror's right-click, like the macOS MirrorView's context menu. */
    if (ImGui::BeginPopup("device_actions")) {
        menu_device();
        ImGui::EndPopup();
    }
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) g_host = argv[++i];
        else { fprintf(stderr, "usage: rplay-gui [-s host]\n"); return 2; }
    }
    setenv("RPLAY_RVRA", "1", 0);

    SDL_Init(SDL_INIT_VIDEO);
    SDL_DisplayMode desk;
    int win_w = 1360, win_h = 940;
    if (SDL_GetCurrentDisplayMode(0, &desk) == 0) {   /* ~70% of the desktop, like a real app */
        win_w = desk.w * 7 / 10;
        win_h = desk.h * 7 / 10;
    }
    SDL_Window *win = SDL_CreateWindow("rplay-hub", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       win_w, win_h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    g_ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
    if (!g_ren) g_ren = SDL_CreateRenderer(win, -1, 0);
    if (!g_ren) { fprintf(stderr, "no display: %s\n", SDL_GetError()); return 1; }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;   /* window layout is fixed; don't scatter imgui.ini files */
    ImGui::StyleColorsDark();
    /* Rough high-DPI accommodation: scale the UI with the desktop, and load a real TTF at the
     * target pixel size — the stock bitmap font upscaled is what makes tools look homemade. */
    SDL_DisplayMode dm;
    float ui_scale = SDL_GetCurrentDisplayMode(0, &dm) == 0 && dm.h >= 2000 ? 2.0f : 1.0f;
    ImGui::GetStyle().ScaleAllSizes(ui_scale);
    {
        const char *faces[] = {
            "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        };
        bool loaded = false;
        for (size_t i = 0; i < sizeof faces / sizeof *faces && !loaded; i++)
            loaded = io.Fonts->AddFontFromFileTTF(faces[i], 15.0f * ui_scale) != NULL;
        if (!loaded) io.FontGlobalScale = ui_scale;
    }
    ImGui_ImplSDL2_InitForSDLRenderer(win, g_ren);
    ImGui_ImplSDLRenderer2_Init(g_ren);

    g_api.start();

    bool quit = false;
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) quit = true;
        }
        video_pump();
        g_api.drain();

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##root", NULL,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
        confirm_modal();

        /* Device Hub's three columns: sidebar | canvas | inspector. Proportional with floors, so
         * the canvas keeps a usable mirror at any window size or UI scale. */
        float em = ImGui::GetFontSize();
        float total = ImGui::GetContentRegionAvail().x;
        float sidebar_w = total * 0.13f < 11 * em ? 11 * em : total * 0.13f;
        float inspector_w = total * 0.34f;   /* the canvas is the star: it gets the widest column */
        float canvas_w = total - sidebar_w - inspector_w - em;
        ImGui::BeginChild("sidebar", ImVec2(sidebar_w, 0), ImGuiChildFlags_Borders);
        device_sidebar();
        ImGui::EndChild();
        ImGui::SameLine();

        ImGui::BeginChild("canvas", ImVec2(canvas_w, 0), ImGuiChildFlags_Borders);
        {
            /* Header: device name + OS, like the canvas title; transient status on the right. */
            ImGui::TextUnformatted(g_dev_name.empty() ? "no device" : g_dev_name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("iOS %s", g_dev_os.c_str());
            if (!g_status.empty() && now_ms() - g_status_at < 6000) {
                float w = ImGui::CalcTextSize(g_status.c_str()).x;
                ImGui::SameLine(canvas_w - w - 2 * ImGui::GetFontSize());
                ImGui::TextDisabled("%s", g_status.c_str());
            }
            float strip_h = ImGui::GetFrameHeightWithSpacing() + em / 2;
            ImGui::BeginChild("screen", ImVec2(0, -strip_h));
            mirror_pane();
            ImGui::EndChild();
            /* Device controls strip, centered. */
            ImGuiStyle &st = ImGui::GetStyle();
            float bw = ImGui::CalcTextSize("Screenshot").x + ImGui::CalcTextSize("Record").x +
                       ImGui::CalcTextSize("Home").x + 6 * st.FramePadding.x + 2 * st.ItemSpacing.x;
            float off = (canvas_w - bw) / 2;
            if (off > 0) ImGui::SetCursorPosX(off);
            if (ImGui::Button("Screenshot")) do_screenshot();
            ImGui::SameLine();
            if (ImGui::Button(g_recording ? "Stop Rec" : "Record")) {
                const char *m = g_recording ? "stop_recording" : "start_recording";
                bool starting = !g_recording;
                g_api.request(m, nullptr, [starting](bool ok, json r) {
                    if (!ok) { set_status("recording call failed"); return; }
                    g_recording = starting;
                    set_status(starting ? "recording (engine-side, recordings/)"
                                        : "recording stopped: " + jstr(r.value("path", json())));
                });
            }
            ImGui::SameLine();
            if (ImGui::Button("Home"))
                g_api.request("press_button", {{"button", "home"}}, nullptr);
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("inspector", ImVec2(0, 0), ImGuiChildFlags_Borders);
        /* Device Hub's two levels: a top row of icon tabs (Settings/Report/Info) and, under
         * Info, a second row of text tabs (Info/Apps/Profiles plus our folded-in
         * Files/Console/Diagnostics, which Device Hub has no equivalent for). This app has no
         * icon font anywhere, so the top row is text too rather than faking icons. Settings and
         * Report are stubs pending the engine methods they'd need (device appearance/
         * accessibility controls, a diagnostics report) -- same as the macOS ComingSoonPanel. */
        if (ImGui::BeginTabBar("toplevel")) {
            if (ImGui::BeginTabItem("Settings")) {
                ImGui::TextWrapped("Device appearance and accessibility controls (Appearance, "
                                   "Text Size, Reduce Motion, ...) need new engine methods to "
                                   "read/set them.");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Report")) {
                ImGui::TextWrapped("A diagnostics report view, matching Device Hub's Report tab.");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Info")) {
                if (ImGui::BeginTabBar("sublevel")) {
                    if (ImGui::BeginTabItem("Info"))        { tab_info();        ImGui::EndTabItem(); }
                    if (ImGui::BeginTabItem("Apps"))        { tab_apps();        ImGui::EndTabItem(); }
                    if (ImGui::BeginTabItem("Profiles"))    { tab_profiles();    ImGui::EndTabItem(); }
                    if (ImGui::BeginTabItem("Files"))       { tab_files();       ImGui::EndTabItem(); }
                    if (ImGui::BeginTabItem("Console"))     { tab_console();     ImGui::EndTabItem(); }
                    if (ImGui::BeginTabItem("Diagnostics")) { tab_diagnostics(); ImGui::EndTabItem(); }
                    ImGui::EndTabBar();
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();
        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(g_ren, 18, 18, 18, 255);
        SDL_RenderClear(g_ren);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), g_ren);
        SDL_RenderPresent(g_ren);
    }

    g_slog_on = false;
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(g_ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
