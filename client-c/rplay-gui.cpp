/* rplay-gui.cpp — the full Linux GUI client for rPlayHub (iOS Device Hub).
 *
 * Shares the Dear ImGui + SDL2 macOS/Device Hub visual layer with rplay-hub-android
 * (client-c/ui/{theme.h, icons.h, window_effects.*, display_window.*, twin_view.*,
 * fold_view.*, png_decode.*}) and drives the iOS cdhost JSON control API on 127.0.0.1:9876,
 * Annex-B HEVC/H.264 video stream on 127.0.0.1:9877, and AAC-ELD audio stream on 127.0.0.1:9878.
 *
 * Layout (matching macOS AppDelegate.swift + InspectorPane.swift):
 *   - Borderless macOS light-theme window with X11 ARGB rounded corners and traffic lights
 *   - Unified 52pt toolbar: traffic lights, sidebar toggle, menus, device pill + stream status,
 *     stage action icons (Pin, 3D View, Pop-out Window, Screenshot, Disconnect), and the
 *     3 top-level Inspector Group icon buttons (Settings, Report, Info)
 *   - Left sidebar (260pt): connected iOS devices from list_devices
 *   - Center stage: gated Phone Mockup with "View Screen" pill button -> Live Mirrored Display
 *     (with DeviceModel crop/notch, rotation, touch tap/swipe, 3D TwinView, FoldView,
 *     pop-out DisplayWindow) + bottom ControlStrip
 *   - Right inspector pane (320pt, collapsible):
 *       1. Settings (Appearance, Liquid Glass, Color Filter, Text Size, Accessibility toggles,
 *          Simulated Location with 14 preset cities + Custom Coordinates modal)
 *       2. Report   (Crash & diagnostic logs from crashreportcopymobile with process name parser,
 *          relative dates, category filter, double-click to save & open, Export All)
 *       3. Info     (Segmented pill sub-tabs: Info | Apps | Profiles | Files | Console)
 */

extern "C" {
#include "stream-core.h"
#include "audio.h"
}

#include "ui/theme.h"
#include "ui/icons.h"
#include "ui/window_effects.h"
#include "ui/display_window.h"
#include "ui/twin_view.h"
#include "ui/fold_view.h"
#include "ui/png_decode.h"

#include <SDL2/SDL.h>
#ifdef _WIN32
#include <SDL2/SDL_syswm.h>
#include <timeapi.h>
#endif
#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <io.h>
#include <direct.h>
typedef intptr_t ssize_t;
#define usleep(x) Sleep((DWORD)((x)/1000))
#define sleep(x) Sleep((DWORD)((x)*1000))
#define close closesocket
#define popen _popen
#define pclose _pclose
#define mkdir(p, m) _mkdir(p)
static inline int setenv_win(const char *n, const char *v, int o) {
    (void)o;
    _putenv_s(n, v);
    SetEnvironmentVariableA(n, v);
    return 0;
}
#define setenv(n, v, o) setenv_win(n, v, o)
#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif
#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef F_OK
#define F_OK 0
#endif
#define access _access
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
static inline const char *strcasestr(const char *haystack, const char *needle) {
    if (!haystack || !needle) return nullptr;
    if (!*needle) return haystack;
    size_t nlen = strlen(needle);
    for (; *haystack; ++haystack) {
        if (_strnicmp(haystack, needle, nlen) == 0) return haystack;
    }
    return nullptr;
}
static inline char *strcasestr(char *haystack, const char *needle) {
    return const_cast<char *>(strcasestr(static_cast<const char *>(haystack), needle));
}
static inline void set_sock_timeout(int fd, int timeout_ms) {
    DWORD tv = (DWORD)timeout_ms;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof tv);
}
static inline bool is_sock_wouldblock() {
    int err = WSAGetLastError();
    return err == WSAEWOULDBLOCK || err == WSAETIMEDOUT || err == WSAEINTR;
}
#else
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <netinet/tcp.h>
#include <unistd.h>
static inline void set_sock_timeout(int fd, int timeout_ms) {
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}
static inline bool is_sock_wouldblock() {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}
#endif
#include <thread>
#include <vector>

using json = nlohmann::json;

/* =========================================================================
 * 1. DeviceModel table — ported from app/rPlayHub/DeviceModel.swift
 * =========================================================================
 * The H.265 SPS rounds 1170x2532 up to 1184x2576 (multiples of 16); cropping to the
 * true screen pixels removes the right/bottom encoder padding bar.
 */
struct DeviceModelSpec {
    const char *product_type;
    const char *marketing_name;
    int screen_w, screen_h;
    int cutout; /* 0 = none, 1 = notch, 2 = dynamic_island */
};

static const DeviceModelSpec kDeviceModels[] = {
    /* iPhone X / XS / 11 Pro */
    {"iPhone10,3", "iPhone X", 1125, 2436, 1},
    {"iPhone10,6", "iPhone X", 1125, 2436, 1},
    {"iPhone11,2", "iPhone XS", 1125, 2436, 1},
    {"iPhone11,4", "iPhone XS Max", 1242, 2688, 1},
    {"iPhone11,6", "iPhone XS Max", 1242, 2688, 1},
    {"iPhone11,8", "iPhone XR", 828, 1792, 1},
    {"iPhone12,1", "iPhone 11", 828, 1792, 1},
    {"iPhone12,3", "iPhone 11 Pro", 1125, 2436, 1},
    {"iPhone12,5", "iPhone 11 Pro Max", 1242, 2688, 1},
    /* iPhone 12 / 13 / 14 (Notch) */
    {"iPhone13,1", "iPhone 12 mini", 1080, 2340, 1},
    {"iPhone13,2", "iPhone 12", 1170, 2532, 1},
    {"iPhone13,3", "iPhone 12 Pro", 1170, 2532, 1},
    {"iPhone13,4", "iPhone 12 Pro Max", 1284, 2778, 1},
    {"iPhone14,4", "iPhone 13 mini", 1080, 2340, 1},
    {"iPhone14,5", "iPhone 13", 1170, 2532, 1},
    {"iPhone14,2", "iPhone 13 Pro", 1170, 2532, 1},
    {"iPhone14,3", "iPhone 13 Pro Max", 1284, 2778, 1},
    {"iPhone14,7", "iPhone 14", 1170, 2532, 1},
    {"iPhone14,8", "iPhone 14 Plus", 1284, 2778, 1},
    /* iPhone 14 Pro / 15 / 16 (Dynamic Island) */
    {"iPhone15,2", "iPhone 14 Pro", 1179, 2556, 2},
    {"iPhone15,3", "iPhone 14 Pro Max", 1290, 2796, 2},
    {"iPhone15,4", "iPhone 15", 1179, 2556, 2},
    {"iPhone15,5", "iPhone 15 Plus", 1290, 2796, 2},
    {"iPhone16,1", "iPhone 15 Pro", 1179, 2556, 2},
    {"iPhone16,2", "iPhone 15 Pro Max", 1290, 2796, 2},
    {"iPhone17,1", "iPhone 16 Pro", 1206, 2622, 2},
    {"iPhone17,2", "iPhone 16 Pro Max", 1320, 2868, 2},
    {"iPhone17,3", "iPhone 16", 1179, 2556, 2},
    {"iPhone17,4", "iPhone 16 Plus", 1290, 2796, 2},
    {"iPhone17,5", "iPhone 16e", 1170, 2532, 1},
    /* iPhone SE / 8 (Home button, no cutout) */
    {"iPhone12,8", "iPhone SE (2nd gen)", 750, 1334, 0},
    {"iPhone14,6", "iPhone SE (3rd gen)", 750, 1334, 0},
    {"iPhone10,1", "iPhone 8", 750, 1334, 0},
    {"iPhone10,4", "iPhone 8", 750, 1334, 0},
    {"iPhone10,2", "iPhone 8 Plus", 1080, 1920, 0},
    {"iPhone10,5", "iPhone 8 Plus", 1080, 1920, 0},
};

static const DeviceModelSpec *lookup_device_model(const std::string &product_type)
{
    for (const auto &m : kDeviceModels) {
        if (product_type == m.product_type) return &m;
    }
    return nullptr;
}

static int infer_cutout_type(const std::string &product_type)
{
    if (const auto *m = lookup_device_model(product_type)) return m->cutout;
    if (product_type.rfind("iPhone", 0) == 0) {
        int major = std::atoi(product_type.c_str() + 6);
        if (major >= 15) return 2;
        if (major >= 10) return 1;
    }
    return 0;
}

/* =========================================================================
 * 2. Video stream thread & shared frame buffer
 * ========================================================================= */
static const char *g_host = "127.0.0.1";
static int         g_video_port = 9877;
static int         g_api_port   = 9876;

static std::mutex           g_frame_mu;
static AVFrame             *g_latest_frame = nullptr;
static int                  g_frame_dirty = 0;
static int                  g_active_w = 0, g_active_h = 0;
static uint32_t             g_frame_seq = 0;
static std::atomic<bool>    g_quit{false};
static std::atomic<bool>    g_stream_connected{false};
static std::atomic<uint64_t> g_frames_decoded{0};
static std::atomic<uint64_t> g_decode_errors{0};
static std::atomic<uint64_t> g_nals_total{0};
static std::atomic<uint64_t> g_trailers_total{0};
static std::atomic<float>    g_live_fps{0.0f};

/* Also populate a DecodedFrame for DisplayWindow (pop-out window) when active. */
static rplayhub::DecodedFrame g_popout_frame;

static void on_decoded_frame(AVFrame *f, int active_w, int active_h)
{
    static bool logged_first = false;
    if (!logged_first) {
        printf("video: first frame decoded (%dx%d, active %dx%d)\n", f->width, f->height, active_w, active_h);
        fflush(stdout);
        logged_first = true;
    }
    static uint32_t s_fps_frames = 0;
    static uint64_t s_fps_last_ms = 0;
    s_fps_frames++;
    uint64_t now = now_ms();
    if (now - s_fps_last_ms >= 500) {
        if (s_fps_last_ms > 0) {
            g_live_fps = (float)s_fps_frames * 1000.0f / (float)(now - s_fps_last_ms);
        }
        s_fps_frames = 0;
        s_fps_last_ms = now;
    }
    std::lock_guard<std::mutex> lk(g_frame_mu);
    if (!g_latest_frame) g_latest_frame = av_frame_alloc();
    av_frame_unref(g_latest_frame);
    av_frame_ref(g_latest_frame, f);
    g_active_w = active_w;
    g_active_h = active_h;
    g_frame_dirty = 1;
    g_frame_seq++;
}

static void video_thread_main()
{
    while (!g_quit) {
        g_h264 = stream_says_h264(g_host, g_api_port);
        const AVCodec *codec = avcodec_find_decoder(g_h264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC);
        if (!codec) { fprintf(stderr, "video: decoder not in this libavcodec\n"); return; }

        int fd = tcp_connect(g_host, g_video_port);
        if (fd < 0) {
            for (int i = 0; i < 10 && !g_quit; i++) usleep(100 * 1000);
            continue;
        }
        /* Disable Nagle's algorithm for immediate transmission and set timeout */
        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof nodelay);
        set_sock_timeout(fd, 2000);

        stream_state st = {};
        st.awaiting_keyframe = 1;
        st.on_frame = on_decoded_frame;
        st.dec = avcodec_alloc_context3(codec);
        st.dec->thread_count = 1; /* RVRA resampling requires single-thread decode */
        st.dec->flags |= AV_CODEC_FLAG_LOW_DELAY;
        st.dec->flags2 |= AV_CODEC_FLAG2_FAST;
        if (avcodec_open2(st.dec, codec, nullptr) < 0) {
            avcodec_free_context(&st.dec);
            close(fd);
            continue;
        }

        g_stream_connected = true;
        annexb_parser parser = {};
        uint8_t buf[65536];
        while (!g_quit) {
            ssize_t r = recv(fd, (char *)buf, (int)sizeof buf, 0);
            if (r > 0) {
                annexb_feed(&parser, buf, (size_t)r, handle_nal, &st);
                /* Drain any remaining packets from the current burst */
                for (;;) {
                    u_long avail = 0;
#ifdef _WIN32
                    if (ioctlsocket(fd, FIONREAD, &avail) != 0 || avail == 0) break;
#else
                    if (ioctl(fd, FIONREAD, &avail) != 0 || avail == 0) break;
#endif
                    ssize_t more = recv(fd, (char *)buf, (int)std::min<size_t>(sizeof buf, (size_t)avail), 0);
                    if (more <= 0) break;
                    annexb_feed(&parser, buf, (size_t)more, handle_nal, &st);
                }
            } else if (r == 0) {
                break;
            } else if (!is_sock_wouldblock()) {
                break;
            }
            g_frames_decoded = st.frames_decoded;
            g_decode_errors  = st.decode_errors;
            g_nals_total     = st.nals;
            g_trailers_total = st.trailers;
        }
        g_stream_connected = false;
        free(parser.buf);
        free(st.au);
        avcodec_free_context(&st.dec);
        close(fd);
    }
}

/* =========================================================================
 * 3. Engine JSON API client (port 9876) & async worker pool
 * ========================================================================= */
static inline std::string json_get_str(const json &j, const std::string &key, const std::string &fallback = "") {
    if (!j.is_object() || !j.contains(key)) return fallback;
    const auto &val = j[key];
    if (val.is_string()) return val.get<std::string>();
    if (val.is_number()) return std::to_string(val.get<int64_t>());
    if (val.is_boolean()) return val.get<bool>() ? "true" : "false";
    return fallback;
}

static inline bool json_get_bool(const json &j, const std::string &key, bool fallback = false) {
    if (!j.is_object() || !j.contains(key)) return fallback;
    const auto &val = j[key];
    if (val.is_boolean()) return val.get<bool>();
    if (val.is_number()) return val.get<int64_t>() != 0;
    if (val.is_string()) return val.get<std::string>() == "true" || val.get<std::string>() == "1";
    return fallback;
}

static inline int64_t json_get_int(const json &j, const std::string &key, int64_t fallback = 0) {
    if (!j.is_object() || !j.contains(key)) return fallback;
    const auto &val = j[key];
    if (val.is_number()) return val.get<int64_t>();
    if (val.is_string()) {
        try { return std::stoll(val.get<std::string>()); } catch (...) { return fallback; }
    }
    return fallback;
}

static inline std::string json_err_str(const json &r, const std::string &fallback = "failed") {
    if (!r.is_object() || !r.contains("error")) return fallback;
    const auto &err = r["error"];
    if (err.is_string()) return err.get<std::string>();
    if (err.is_object()) {
        std::string msg = json_get_str(err, "message", "");
        if (msg.empty()) msg = json_get_str(err, "code", fallback);
        return msg.empty() ? fallback : msg;
    }
    return err.dump();
}

static json api_call(const std::string &method, const json &params = json::object(), int timeout_s = 15)
{
    int fd = tcp_connect(g_host, g_api_port);
    if (fd < 0) return {{"ok", false}, {"error", "cannot connect to cdhost:9876"}};

    set_sock_timeout(fd, timeout_s * 1000);

    json req = {{"id", 1}, {"method", method}};
    if (!params.empty()) req["params"] = params;
    std::string line = req.dump() + "\n";

    ssize_t sent = send(fd, line.data(), (int)line.size(), 0);
    if (sent != (ssize_t)line.size()) {
        close(fd);
        return {{"ok", false}, {"error", "send failed"}};
    }

    std::string reply;
    char ch;
    while (recv(fd, &ch, 1, 0) == 1) {
        if (ch == '\n') break;
        reply.push_back(ch);
    }
    close(fd);
    if (reply.empty()) return {{"ok", false}, {"error", "empty reply from engine"}};
    try {
        json r = json::parse(reply);
        /* The engine reports failures as {"error": {"code": ..., "message": ...}}; every caller
         * reads r["error"] as a string, so lift the message out here and keep the code beside
         * it. Leaves a string error (our own, above) as it is. */
        if (r.contains("error")) {
            if (r["error"].is_object()) {
                json err = r["error"];
                std::string code = json_get_str(err, "code", "");
                std::string msg  = json_get_str(err, "message", "");
                r["error_code"]  = code;
                if (msg.empty()) msg = code.empty() ? "error" : code;
                r["error"] = msg;
            } else if (!r["error"].is_string()) {
                r["error"] = r["error"].dump();
            }
        }
        return r;
    } catch (const std::exception &e) {
        return {{"ok", false}, {"error", std::string("bad json: ") + e.what()}};
    }
}

static void show_toast(const std::string &msg, uint64_t duration_ms);

static std::mutex                        g_ui_cb_mu;
static std::vector<std::function<void()>> g_ui_callbacks;

static void post_to_ui(std::function<void()> fn)
{
    std::lock_guard<std::mutex> lk(g_ui_cb_mu);
    g_ui_callbacks.push_back(std::move(fn));
}

static void drain_ui_callbacks()
{
    std::vector<std::function<void()>> batch;
    {
        std::lock_guard<std::mutex> lk(g_ui_cb_mu);
        batch.swap(g_ui_callbacks);
    }
    /* A reply whose shape the handler did not expect must not take the whole window down: report
     * it and carry on. */
    for (auto &fn : batch) {
        try {
            fn();
        } catch (const std::exception &e) {
            fprintf(stderr, "ui callback: %s\n", e.what());
            show_toast(std::string("Unexpected reply from the engine: ") + e.what(), 8000);
        }
    }
}

static void api_async(const std::string &method, const json &params,
                      std::function<void(const json &)> on_done = nullptr,
                      int timeout_s = 30)
{
    std::thread([method, params, on_done, timeout_s]() {
        json r = api_call(method, params, timeout_s);
        if (on_done) {
            post_to_ui([on_done, r]() { on_done(r); });
        }
    }).detach();
}

/* =========================================================================
 * 4. Base64 decode & file helpers
 * ========================================================================= */
static std::vector<uint8_t> b64_decode(const std::string &in)
{
    static const int8_t T[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63, 52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14, 15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40, 41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    };
    std::vector<uint8_t> out;
    out.reserve(in.size() * 3 / 4);
    uint32_t val = 0;
    int bits = -8;
    for (unsigned char c : in) {
        int8_t d = T[c];
        if (d < 0) continue;
        val = (val << 6) | (uint32_t)d;
        bits += 6;
        if (bits >= 0) {
            out.push_back((uint8_t)((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

static std::string ensure_media_dir(const char *subdir)
{
#ifdef _WIN32
    const char *home = getenv("USERPROFILE");
    if (!home) home = getenv("TEMP");
    std::string base = home ? home : ".";
#else
    const char *home = getenv("HOME");
    std::string base = home ? home : "/tmp";
#endif
    std::string dir = base + "/" + subdir;
    mkdir(dir.c_str(), 0755);
    std::string sub = dir + "/rPlayHub";
    if (mkdir(sub.c_str(), 0755) == 0 || access(sub.c_str(), W_OK) == 0) return sub;
    if (access(dir.c_str(), W_OK) == 0) return dir;
#ifdef _WIN32
    return base;
#else
    return "/tmp";
#endif
}

static std::string timestamp_filename(const char *prefix, const char *ext)
{
    time_t t = time(nullptr);
    struct tm tm = {};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64];
    strftime(buf, sizeof buf, "%Y-%m-%d_%H-%M-%S", &tm);
    return std::string(prefix) + "_" + buf + ext;
}

static std::string format_bytes(int64_t bytes)
{
    char buf[64];
    if (bytes >= (1LL << 30)) snprintf(buf, sizeof buf, "%.2f GB", (double)bytes / (double)(1LL << 30));
    else if (bytes >= (1LL << 20)) snprintf(buf, sizeof buf, "%.1f MB", (double)bytes / (double)(1LL << 20));
    else if (bytes >= 1024) snprintf(buf, sizeof buf, "%.1f KB", (double)bytes / 1024.0);
    else snprintf(buf, sizeof buf, "%lld B", (long long)bytes);
    return buf;
}

static std::string format_relative_time(int64_t mtime)
{
    if (mtime <= 0) return "";
    time_t now = time(nullptr);
    int64_t diff = (int64_t)now - mtime;
    struct tm tm_file = {}, tm_now = {};
    time_t mt = (time_t)mtime;
#ifdef _WIN32
    localtime_s(&tm_file, &mt);
    localtime_s(&tm_now, &now);
#else
    localtime_r(&mt, &tm_file);
    localtime_r(&now, &tm_now);
#endif
    char tbuf[32];
    if (diff >= 0 && diff < 86400 && tm_file.tm_yday == tm_now.tm_yday && tm_file.tm_year == tm_now.tm_year) {
        strftime(tbuf, sizeof tbuf, "Today, %H:%M", &tm_file);
        return tbuf;
    }
    if (diff >= 0 && diff < 172800) {
        strftime(tbuf, sizeof tbuf, "Yesterday, %H:%M", &tm_file);
        return tbuf;
    }
    strftime(tbuf, sizeof tbuf, "%b %d, %H:%M", &tm_file);
    return tbuf;
}

/* Native desktop file dialog helper (zenity on GNOME, kdialog on KDE, GetOpenFileName on Windows) run on a worker thread. */
static void pick_file_async(const char *title, const char *zenity_filter, const char *kdialog_filter,
                            std::function<void(const std::string &)> on_picked)
{
#ifdef _WIN32
    (void)zenity_filter; (void)kdialog_filter;
    std::string wtitle = title ? title : "Select File";
    std::thread([wtitle, on_picked]() {
        char filename[MAX_PATH] = { 0 };
        OPENFILENAMEA ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = NULL;
        ofn.lpstrFile = filename;
        ofn.nMaxFile = sizeof(filename);
        ofn.lpstrTitle = wtitle.c_str();
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameA(&ofn)) {
            std::string out = filename;
            post_to_ui([on_picked, out]() { on_picked(out); });
        } else {
            post_to_ui([on_picked]() { on_picked(""); });
        }
    }).detach();
#else
    std::string zcmd = std::string("zenity --file-selection --title='") + title +
                       "' --file-filter='" + zenity_filter + "' 2>/dev/null";
    std::string kcmd = std::string("kdialog --getopenfilename . '") + kdialog_filter + "' 2>/dev/null";
    std::thread([zcmd, kcmd, on_picked]() {
        const char *cmds[2] = { zcmd.c_str(), kcmd.c_str() };
        for (const char *cmd : cmds) {
            FILE *fp = popen(cmd, "r");
            if (!fp) continue;
            char buf[4096] = {};
            std::string out;
            while (fgets(buf, sizeof buf, fp)) out += buf;
            int rc = pclose(fp);
            if (rc == 0) {
                while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
                post_to_ui([on_picked, out]() { on_picked(out); });
                return;
            }
            if (WIFEXITED(rc) && WEXITSTATUS(rc) == 1) {
                post_to_ui([on_picked]() { on_picked(""); });
                return;
            }
        }
        post_to_ui([on_picked]() { on_picked("!nodialog"); });
    }).detach();
#endif
}

/* =========================================================================
 * 5. Application state & data models
 * ========================================================================= */
struct DeviceRow {
    std::string udid;
    std::string name;
    std::string product_type;
    std::string os_version;
    std::string connection;
    int         screen_w = 0;
    int         screen_h = 0;
    bool        bound = false;

    std::string display_name() const {
        if (!name.empty()) return name;
        if (const auto *m = lookup_device_model(product_type)) return m->marketing_name;
        return product_type.empty() ? udid : product_type;
    }
};

struct AppRow {
    std::string bundle_id;
    std::string name;
    std::string version;
    bool        first_party = false;
    bool        developer   = false;
    bool        app_clip    = false;
    SDL_Texture *icon_tex   = nullptr;
    bool        icon_requested = false;
};

struct ProvProfile {
    std::string uuid;
    std::string name;
    std::string app_id_name;
    std::string team;
    std::string expiration;
};

struct ConfigProfile {
    std::string identifier;
    std::string uuid;
    std::string display_name;
    std::string organization;
};

struct FileRow {
    std::string name;
    std::string full_path; /* used in Report tab when combining / and /DiagnosticLogs */
    std::string type;      /* "dir", "file", "symlink" */
    int64_t     size  = 0;
    int64_t     mtime = 0;
};

/* Preset simulated locations from app/rPlayHub/SettingsPanel.swift */
struct LocationPreset {
    const char *name;
    double      lat;
    double      lon;
};

static const LocationPreset kLocationPresets[] = {
    {"Cupertino",     37.3349, -122.0090},
    {"San Francisco", 37.7749, -122.4194},
    {"New York",      40.7128,  -74.0060},
    {"London",        51.5074,   -0.1278},
    {"Paris",         48.8566,    2.3522},
    {"Berlin",        52.5200,   13.4050},
    {"Tokyo",         35.6762,  139.6503},
    {"Seoul",         37.5665,  126.9780},
    {"Singapore",      1.3521,  103.8198},
    {"Sydney",       -33.8688,  151.2093},
    {"Mumbai",        19.0760,   72.8777},
    {"S\xc3\xa3o Paulo", -23.5505, -46.6333},
    {"Cairo",         30.0444,   31.2357},
    {"Reykjavik",     64.1466,  -21.9426},
};

static const char *const kTextSizes[7] = {
    "extraSmall",
    "small",
    "medium",
    "large",
    "extraLarge",
    "extraExtraLarge",
    "extraExtraExtraLarge",
};

static const char *const kTextSizeLabels[7] = {
    "XS", "S", "M", "L", "XL", "XXL", "XXXL"
};

/* Window & chrome state */
static SDL_Window   *g_win      = nullptr;
static SDL_Renderer *g_ren      = nullptr;
static float         g_scale    = 1.0f;
static bool          g_argb     = false;
#ifdef _WIN32
static bool          g_system_titlebar = true;
#else
static bool          g_system_titlebar = false;
#endif
static bool          g_pinned   = false;
static float         g_menu_h   = 52.0f;
static std::vector<ImVec4> g_no_drag_rects;

/* Cursors for borderless resize */
static SDL_Cursor *g_cursor_arrow       = nullptr;
static SDL_Cursor *g_cursor_resize_ew   = nullptr;
static SDL_Cursor *g_cursor_resize_ns   = nullptr;
static SDL_Cursor *g_cursor_resize_nwse = nullptr;
static SDL_Cursor *g_cursor_resize_nesw = nullptr;
static bool        g_cursor_overridden  = false;
static bool        g_is_resizing_border = false;
static SDL_Cursor *g_active_resize_cursor = nullptr;
static ImGuiMouseCursor g_active_resize_imgui_cursor = ImGuiMouseCursor_Arrow;

/* Fonts & dynamic glyph atlas */
static ImFont *g_font_regular  = nullptr;
static ImFont *g_font_medium   = nullptr;
static ImFont *g_font_semibold = nullptr;
static ImFont *g_font_bold     = nullptr;
static ImFont *g_font_caption  = nullptr;
static ImFont *g_font_mono     = nullptr;
static std::string g_regular_font_path;
static std::string g_medium_font_path;
static std::string g_semibold_font_path;
static std::string g_bold_font_path;
static std::string g_mono_font_path;
static std::string g_cjk_font_path;
static int         g_cjk_font_face = 0;
static std::set<ImWchar> g_extra_codepoints;
static bool        g_fonts_dirty = false;

/* Layout & focus panes */
enum class FocusPane { Sidebar, Stage, Inspector };
static FocusPane g_focused_pane      = FocusPane::Stage;
static bool      g_sidebar_hidden    = false;
static bool      g_inspector_visible = true;
static int       g_inspector_group   = 2; /* 0 = Settings, 1 = Report, 2 = Info */
static int       g_info_subtab       = 0; /* 0 = Info, 1 = Apps, 2 = Profiles, 3 = Files, 4 = Console */

/* Devices & session state */
static std::vector<DeviceRow> g_devices;
static int                    g_selected_device_idx = -1;
static std::string            g_bound_udid;
static std::string            g_bound_name;
static std::string            g_bound_product_type;
static std::string            g_bound_os_version;
static int                    g_bound_screen_w = 0;
static int                    g_bound_screen_h = 0;
static bool                   g_switching_device = false;
static uint64_t               g_last_dev_poll_ms = 0;

/* Center stage & mirror state */
#ifdef _WIN32
static bool          g_view_screen   = true;  /* Auto-start mirror on Windows */
#else
static bool          g_view_screen   = false; /* Gated by "View Screen" pill button on mockup */
#endif
static SDL_Texture  *g_vtex          = nullptr;
static int           g_vtex_w        = 0;
static int           g_vtex_h        = 0;
static int           g_live_w        = 0;
static int           g_live_h        = 0;
static ImVec2        g_vtex_uv0      = ImVec2(0.0f, 0.0f);
static ImVec2        g_vtex_uv1      = ImVec2(1.0f, 1.0f);
static int           g_rotation      = 0; /* 0..3 quadrants */
static bool          g_recording     = false;
static std::string   g_recording_path;
static bool          g_drag_active   = false;
static float         g_drag_fx0      = 0.0f, g_drag_fy0 = 0.0f;
static uint64_t      g_drag_t0_ms    = 0;

/* 3D TwinView, FoldView & Pop-out DisplayWindow */
static rplayhub::TwinView  g_twin;
static rplayhub::FoldView  g_fold;
static bool                g_twin_mode      = false;
static bool                g_fold_only_view = false;
static bool                g_twin_demo      = false;
static std::chrono::steady_clock::time_point g_twin_demo_start{};
static std::chrono::steady_clock::time_point g_fold_clock{};
static bool                g_orbit_dragging = false;
static SDL_Texture        *g_back_texture   = nullptr;
static std::unique_ptr<rplayhub::DisplayWindow> g_popout_win;
static rplayhub::AgentSession g_popout_session;

/* Center stage screen tabs (matching macOS Device Hub ScreenTabBar.swift) */
struct ScreenTab {
    std::string udid;
    std::string title;
};
static std::vector<ScreenTab> g_screen_tabs;
static int g_current_tab = 0;

/* Settings tab state (Group 0) */
static bool  g_settings_loaded        = false;
static bool  g_settings_loading       = false;
static int   g_set_appearance         = 0; /* 0 = Light, 1 = Dark */
static float g_set_liquid_glass       = 0.0f;
static int   g_set_color_filter       = 0; /* 0 = None, 1 = On */
static int   g_set_text_size_idx      = 3; /* 0..6, default 3 = "large" */
static bool  g_set_reduce_motion      = false;
static bool  g_set_increase_contrast  = false;
static bool  g_set_show_borders       = false;
static bool  g_set_reduce_transp      = false;
static bool  g_set_voiceover          = false;
static int   g_set_location_idx       = 0; /* 0 = None, 1..14 = preset cities, 15 = Custom */
static char  g_custom_lat_buf[64]     = "37.3349";
static char  g_custom_lon_buf[64]     = "-122.0090";
static bool  g_open_custom_loc_modal  = false;
static std::string g_custom_loc_label;

/* Report tab state (Group 1) */
static std::vector<FileRow> g_reports;
static bool                 g_reports_loaded  = false;
static bool                 g_reports_loading = false;
static char                 g_report_filter[128] = "";
static int                  g_report_category = 0; /* 0 = Crashes, 1 = Spins, 2 = Logs, 3 = Diagnostics */

/* Info sub-tab 0: Device Info & Stream Diagnostics */
static json     g_device_info;
static bool     g_device_info_loaded  = false;
static bool     g_device_info_loading = false;
static json     g_stream_info;
static uint64_t g_last_stream_poll_ms = 0;

/* Info sub-tab 1: Apps */
static std::vector<AppRow>             g_apps;
static std::map<std::string, int>      g_running_pids; /* process name or bundle_id -> pid */
static std::map<std::string, SDL_Texture*> g_app_icon_cache;
static int                             g_icons_in_flight = 0;
static bool                            g_apps_loaded  = false;
static bool                            g_apps_loading = false;
static char                            g_app_filter[128] = "";
static int                             g_app_category = 0; /* 0 = All Apps, 1 = App Clips, 2 = Default, 3 = Developer */
static char                            g_ipa_path_buf[512] = "";
static bool                            g_open_install_ipa_modal = false;
static std::string                     g_confirm_uninstall_bundle;
static std::string                     g_confirm_uninstall_name;

/* Info sub-tab 2: Profiles */
static std::vector<ProvProfile>   g_prov_profiles;
static std::vector<ConfigProfile> g_cfg_profiles;
static bool                       g_profiles_loaded  = false;
static bool                       g_profiles_loading = false;
static int                        g_selected_prov_idx = -1;
static int                        g_selected_cfg_idx  = -1;
static char                       g_profile_path_buf[512] = "";
static bool                       g_open_install_profile_modal = false;
static std::string                g_confirm_remove_profile_type;
static std::string                g_confirm_remove_profile_id;
static std::string                g_confirm_remove_profile_name;

/* Info sub-tab 3: Files */
static const char          *g_file_service = "media"; /* "media" or "crash" */
static std::string          g_file_path    = "/";
static std::vector<FileRow> g_files;
static bool                 g_files_loaded  = false;
static bool                 g_files_loading = false;

/* Info sub-tab 4: Console (syslog) */
static std::mutex              g_syslog_mu;
static std::deque<std::string> g_syslog_lines;
static std::atomic<bool>       g_syslog_running{false};
static std::atomic<int>        g_syslog_fd{-1};
static bool                    g_syslog_follow = true;
static char                    g_syslog_filter[128] = "";

/* Confirmation modals & Toast notifications */
static bool        g_open_shutdown_modal = false;
static std::string g_toast_msg;
static uint64_t    g_toast_until_ms = 0;

static void show_toast(const std::string &msg, uint64_t duration_ms = 4000)
{
    g_toast_msg = msg;
    g_toast_until_ms = now_ms() + duration_ms;
}

/* =========================================================================
 * 6. Font discovery & dynamic glyph range builder
 * ========================================================================= */
static std::string find_existing_file(std::initializer_list<std::string> candidates)
{
    for (const auto &p : candidates) {
        if (!p.empty() && access(p.c_str(), R_OK) == 0) return p;
    }
    return "";
}

static void ensure_glyphs(const std::string &utf8)
{
    if (!g_font_regular || utf8.empty()) return;
    const char *p = utf8.c_str();
    const char *end = p + utf8.size();
    while (p < end) {
        unsigned int c = 0;
        int n = ImTextCharFromUtf8(&c, p, end);
        if (n <= 0) break;
        p += n;
        if (c < 0x80 || c == 0xFFFD) continue;
        if (g_font_regular->FindGlyphNoFallback((ImWchar)c)) continue;
        if (g_extra_codepoints.insert((ImWchar)c).second) g_fonts_dirty = true;
    }
}

static void discover_fonts()
{
    char *base_raw = SDL_GetBasePath();
    std::string base = base_raw ? base_raw : "./";
    if (base_raw) SDL_free(base_raw);

    g_regular_font_path = find_existing_file({
        base + "fonts/Inter-Regular.ttf",
        base + "../fonts/Inter-Regular.ttf",
        "client-c/fonts/Inter-Regular.ttf",
        "fonts/Inter-Regular.ttf",
        "/usr/share/rplayhub/fonts/Inter-Regular.ttf",
        "/usr/share/fonts/truetype/inter/Inter-Regular.ttf",
        "/usr/share/fonts/opentype/inter/Inter-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
    });
    g_medium_font_path = find_existing_file({
        base + "fonts/Inter-Medium.ttf",
        base + "../fonts/Inter-Medium.ttf",
        "client-c/fonts/Inter-Medium.ttf",
        "fonts/Inter-Medium.ttf",
        "/usr/share/rplayhub/fonts/Inter-Medium.ttf",
        "/usr/share/fonts/truetype/inter/Inter-Medium.ttf",
        "/usr/share/fonts/opentype/inter/Inter-Medium.ttf",
        g_regular_font_path,
    });
    g_semibold_font_path = find_existing_file({
        base + "fonts/Inter-SemiBold.ttf",
        base + "../fonts/Inter-SemiBold.ttf",
        "client-c/fonts/Inter-SemiBold.ttf",
        "fonts/Inter-SemiBold.ttf",
        "/usr/share/rplayhub/fonts/Inter-SemiBold.ttf",
        "/usr/share/fonts/truetype/inter/Inter-SemiBold.ttf",
        "/usr/share/fonts/opentype/inter/Inter-SemiBold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        g_medium_font_path,
    });
    g_bold_font_path = find_existing_file({
        base + "fonts/Inter-Bold.ttf",
        base + "../fonts/Inter-Bold.ttf",
        "client-c/fonts/Inter-Bold.ttf",
        "fonts/Inter-Bold.ttf",
        "/usr/share/rplayhub/fonts/Inter-Bold.ttf",
        "/usr/share/fonts/truetype/inter/Inter-Bold.ttf",
        "/usr/share/fonts/opentype/inter/Inter-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        g_semibold_font_path,
    });
    g_mono_font_path = find_existing_file({
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        g_regular_font_path,
    });

    struct CjkCandidate { const char *path; int face; };
    static const CjkCandidate kCjkCandidates[] = {
        {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", 2},
        {"/usr/share/fonts/opentype/noto/NotoSansCJK-Medium.ttc", 2},
        {"/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc", 2},
        {"/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", 2},
        {"/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf", 0},
        {"/usr/share/fonts/truetype/wqy/wqy-microhei.ttc", 0},
    };
    for (const auto &c : kCjkCandidates) {
        if (access(c.path, R_OK) == 0) {
            g_cjk_font_path = c.path;
            g_cjk_font_face = c.face;
            break;
        }
    }
}

static void rebuild_fonts()
{
    ImGuiIO &io = ImGui::GetIO();
    io.Fonts->Clear();

    static ImVector<ImWchar> ranges;
    ranges.clear();
    {
        ImFontGlyphRangesBuilder builder;
        static const ImWchar base_ranges[] = {
            0x0020, 0x024F, /* Basic Latin + Latin-1 + Latin Extended */
            0x2000, 0x206F, /* General Punctuation: — ’ “ ” … • */
            0x2190, 0x21FF, /* Arrows */
            0x25A0, 0x25FF, /* Geometric shapes */
            0,
        };
        builder.AddRanges(base_ranges);
        for (ImWchar c : g_extra_codepoints) builder.AddChar(c);
        builder.BuildRanges(&ranges);
    }

    static ImVector<ImWchar> cjk_ranges;
    cjk_ranges.clear();
    if (!g_cjk_font_path.empty()) {
        ImFontGlyphRangesBuilder b;
        b.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        static const ImWchar extra[] = { 0x3040, 0x30FF, 0xAC00, 0xD7A3, 0 };
        b.AddRanges(extra);
        for (ImWchar c : g_extra_codepoints) b.AddChar(c);
        b.BuildRanges(&cjk_ranges);
    }

    auto load_with_cjk = [&](const std::string &path, float size_px) -> ImFont * {
        if (path.empty()) return nullptr;
        ImFont *f = io.Fonts->AddFontFromFileTTF(path.c_str(), size_px, nullptr, ranges.Data);
        if (f && !g_cjk_font_path.empty()) {
            ImFontConfig mcfg;
            mcfg.MergeMode = true;
            mcfg.FontNo = g_cjk_font_face;
            mcfg.OversampleH = 1;
            io.Fonts->AddFontFromFileTTF(g_cjk_font_path.c_str(), size_px, &mcfg, cjk_ranges.Data);
        }
        return f;
    };

    /* Exact font scale hierarchy from rplay-hub-android-dev/linux/src/ui/gui_app.cc */
    const float s = g_scale;
    const float base_reg  = 16.0f * s;
    const float base_cap  = 13.5f * s;
    const float base_med  = 16.0f * s;
    const float base_bold = 16.5f * s;
    const float base_mono = 14.0f * s;

    g_font_regular  = load_with_cjk(g_regular_font_path,  base_reg);
    g_font_caption  = load_with_cjk(g_regular_font_path,  base_cap);
    g_font_medium   = load_with_cjk(g_medium_font_path,   base_med);
    g_font_semibold = load_with_cjk(g_semibold_font_path, base_bold);
    g_font_bold     = load_with_cjk(g_bold_font_path,     base_bold);
    g_font_mono     = load_with_cjk(g_mono_font_path,     base_mono);

    if (!g_font_regular)  g_font_regular  = io.Fonts->AddFontDefault();
    if (!g_font_medium)   g_font_medium   = g_font_regular;
    if (!g_font_semibold) g_font_semibold = g_font_medium;
    if (!g_font_bold)     g_font_bold     = g_font_semibold;
    if (!g_font_caption)  g_font_caption  = g_font_regular;
    if (!g_font_mono)     g_font_mono     = g_font_regular;

    io.FontDefault = g_font_regular;
    if (io.BackendRendererUserData) {
        ImGui_ImplSDLRenderer2_DestroyFontsTexture();
        ImGui_ImplSDLRenderer2_CreateFontsTexture();
    }
    g_fonts_dirty = false;
}

/* =========================================================================
 * 7. Data fetchers (devices, settings, reports, info, apps, profiles, files, syslog)
 * ========================================================================= */
static void refresh_settings();
static void refresh_reports();
static void refresh_device_info();
static void refresh_apps();
static void refresh_profiles();
static void refresh_files();

static void refresh_devices()
{
    api_async("list_devices", {}, [](const json &r) {
        if (!r.value("ok", false)) return;
        std::vector<DeviceRow> list;
        std::string prev_bound = g_bound_udid;
        g_bound_udid.clear();

        auto arr = r["result"].value("devices", json::array());
        int bound_idx = -1;
        for (const auto &d : arr) {
            DeviceRow row;
            row.udid         = d.value("udid", "");
            row.name         = d.value("name", "");
            row.product_type = d.value("product_type", "");
            row.os_version   = d.value("os_version", d.value("product_version", ""));
            row.connection   = d.value("connection", "USB");
            row.screen_w     = d.value("screen_width", 0);
            row.screen_h     = d.value("screen_height", 0);
            row.bound        = d.value("bound", false);
            ensure_glyphs(row.name);
            if (row.bound) {
                bound_idx            = (int)list.size();
                g_bound_udid         = row.udid;
                g_bound_name         = row.display_name();
                g_bound_product_type = row.product_type;
                g_bound_os_version   = row.os_version;
                g_bound_screen_w     = row.screen_w;
                g_bound_screen_h     = row.screen_h;
            }
            list.push_back(std::move(row));
        }
        g_devices = std::move(list);
        if (bound_idx >= 0) {
            g_selected_device_idx = bound_idx;
        } else if (g_selected_device_idx < 0 && !g_devices.empty()) {
            g_selected_device_idx = 0;
        } else if (g_selected_device_idx >= (int)g_devices.size()) {
            g_selected_device_idx = (int)g_devices.size() - 1;
        }

        if (!g_bound_udid.empty() && g_bound_udid != prev_bound) {
            g_switching_device   = false;
            g_settings_loaded    = false;
            g_reports_loaded     = false;
            g_device_info_loaded = false;
            g_apps_loaded        = false;
            g_profiles_loaded    = false;
            g_files_loaded       = false;
            if (g_popout_win) g_popout_win->setTitle(g_bound_name);
        }
        if (!g_bound_udid.empty()) {
            std::string tname = g_bound_name.empty() ? "iPhone" : g_bound_name;
            if (g_screen_tabs.empty()) {
                g_screen_tabs.push_back({g_bound_udid, tname});
                g_current_tab = 0;
            } else if (g_current_tab >= 0 && g_current_tab < (int)g_screen_tabs.size()) {
                g_screen_tabs[g_current_tab].udid = g_bound_udid;
                g_screen_tabs[g_current_tab].title = tname;
            }
        }
    }, 5);
}

static void switch_to_device(const std::string &udid, const std::string &name)
{
    if (udid.empty() || udid == g_bound_udid) return;
    g_switching_device = true;
    show_toast("Switching engine to " + name + "…", 5000);
    api_async("select_device", {{"udid", udid}}, [](const json &r) {
        if (!r.value("ok", false)) {
            g_switching_device = false;
            show_toast("Switch failed: " + json_err_str(r, "unknown"), 6000);
            return;
        }
        /* cdhost re-execs itself and runs tunnel + RSD handshake (~3s). */
        std::thread([]() {
            usleep(3200 * 1000);
            post_to_ui([]() { refresh_devices(); });
        }).detach();
    });
}

static void update_screen_tabs(const std::string &udid, const std::string &title)
{
    if (udid.empty()) return;
    if (g_screen_tabs.empty()) {
        g_screen_tabs.push_back({udid, title.empty() ? "iPhone" : title});
        g_current_tab = 0;
    } else if (g_current_tab >= 0 && g_current_tab < (int)g_screen_tabs.size()) {
        g_screen_tabs[g_current_tab].udid = udid;
        g_screen_tabs[g_current_tab].title = title.empty() ? "iPhone" : title;
    }
}

static void select_screen_tab(int index)
{
    if (index >= 0 && index < (int)g_screen_tabs.size() && index != g_current_tab) {
        g_current_tab = index;
        switch_to_device(g_screen_tabs[index].udid, g_screen_tabs[index].title);
    }
}

static void open_screen_tab()
{
    std::string udid = g_bound_udid;
    std::string name = g_bound_name.empty() ? "iPhone" : g_bound_name;
    for (const auto &d : g_devices) {
        bool found = false;
        for (const auto &t : g_screen_tabs) {
            if (t.udid == d.udid) { found = true; break; }
        }
        if (!found) {
            udid = d.udid;
            name = d.display_name();
            break;
        }
    }
    g_screen_tabs.push_back({udid, name});
    g_current_tab = (int)g_screen_tabs.size() - 1;
    switch_to_device(udid, name);
    show_toast("Opened " + name + " in new tab");
}

static void close_screen_tab(int index)
{
    if (g_screen_tabs.size() > 1 && index >= 0 && index < (int)g_screen_tabs.size()) {
        bool was_current = (index == g_current_tab);
        g_screen_tabs.erase(g_screen_tabs.begin() + index);
        if (was_current) {
            g_current_tab = std::min(index, (int)g_screen_tabs.size() - 1);
            switch_to_device(g_screen_tabs[g_current_tab].udid, g_screen_tabs[g_current_tab].title);
        } else if (index < g_current_tab) {
            g_current_tab--;
        }
    }
}

static void refresh_settings()
{
    if (g_settings_loading) return;
    g_settings_loading = true;
    api_async("get_settings", {}, [](const json &r) {
        g_settings_loading = false;
        g_settings_loaded  = true;
        if (!r.value("ok", false) || !r.contains("result") || !r["result"].is_object()) return;
        const json &res = r["result"];
        std::string app = res.value("appearance", "light");
        g_set_appearance = (app == "dark") ? 1 : 0;
        if (res.contains("liquidGlass") && res["liquidGlass"].is_number())
            g_set_liquid_glass = std::clamp(res["liquidGlass"].get<float>(), 0.0f, 1.0f);
        if (res.contains("colorFilter") && res["colorFilter"].is_number_integer())
            g_set_color_filter = res["colorFilter"].get<int>() > 0 ? 1 : 0;
        if (res.contains("textSize") && res["textSize"].is_string()) {
            std::string ts = res["textSize"].get<std::string>();
            for (int i = 0; i < 7; i++) {
                if (ts == kTextSizes[i]) { g_set_text_size_idx = i; break; }
            }
        }
        auto get_bool = [&](const char *k, bool &dst) {
            if (!res.contains(k)) return;
            if (res[k].is_boolean()) dst = res[k].get<bool>();
            else if (res[k].is_number_integer()) dst = res[k].get<int>() != 0;
        };
        get_bool("reduceMotion",       g_set_reduce_motion);
        get_bool("increaseContrast",   g_set_increase_contrast);
        get_bool("showBorders",        g_set_show_borders);
        get_bool("reduceTransparency", g_set_reduce_transp);
        get_bool("voiceOver",          g_set_voiceover);
    });
}

/* Parse human process name from a crash report filename (ported from ReportPanel.swift). */
static std::string process_name_from_filename(const std::string &name)
{
    std::string base = name;
    auto slash = base.rfind('/');
    if (slash != std::string::npos) base = base.substr(slash + 1);

    /* Strip known extensions */
    for (const char *ext : {".ips", ".crash", ".spin", ".hang", ".panic", ".log", ".diag"}) {
        size_t elen = strlen(ext);
        if (base.size() > elen && base.compare(base.size() - elen, elen, ext) == 0) {
            base = base.substr(0, base.size() - elen);
            break;
        }
    }
    /* Match -YYYY-MM-DD or _YYYY-MM-DD timestamp suffix */
    for (size_t i = 1; i + 10 <= base.size(); i++) {
        if ((base[i] == '-' || base[i] == '_') &&
            std::isdigit((unsigned char)base[i + 1]) &&
            std::isdigit((unsigned char)base[i + 2]) &&
            std::isdigit((unsigned char)base[i + 3]) &&
            std::isdigit((unsigned char)base[i + 4]) &&
            base[i + 5] == '-' &&
            std::isdigit((unsigned char)base[i + 6]) &&
            std::isdigit((unsigned char)base[i + 7])) {
            return base.substr(0, i);
        }
    }
    return base;
}

static bool report_matches_category(const std::string &name, int cat)
{
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    bool is_spin = lower.find(".spin") != std::string::npos || lower.find(".hang") != std::string::npos ||
                   lower.find("tailspin") != std::string::npos;
    bool is_diag = lower.find("sysdiagnose") != std::string::npos || lower.find(".diag") != std::string::npos ||
                   lower.find("jetsam") != std::string::npos;
    bool is_log  = lower.find(".log") != std::string::npos || lower.find(".txt") != std::string::npos ||
                   lower.find(".plist") != std::string::npos;
    bool is_crash = lower.find(".ips") != std::string::npos || lower.find(".crash") != std::string::npos ||
                    lower.find(".panic") != std::string::npos;
    switch (cat) {
    case 0: /* Crashes */     return is_crash && !is_spin && !is_diag;
    case 1: /* Spins */       return is_spin;
    case 2: /* Logs */        return is_log;
    case 3: /* Diagnostics */ return is_diag || (!is_crash && !is_spin && !is_log);
    default: return true;
    }
}

static void refresh_reports()
{
    if (g_reports_loading) return;
    g_reports_loading = true;
    std::thread([]() {
        std::vector<FileRow> combined;
        for (const char *dir : {"/", "/DiagnosticLogs"}) {
            json r = api_call("list_dir", {{"service", "crash"}, {"path", dir}}, 15);
            if (!r.value("ok", false)) continue;
            for (const auto &e : r["result"].value("entries", json::array())) {
                std::string type = e.value("type", "file");
                std::string name = e.value("name", "");
                if (type == "dir" || name.empty() || name[0] == '.') continue;
                FileRow row;
                row.name      = name;
                row.full_path = (std::string(dir) == "/") ? ("/" + name) : (std::string(dir) + "/" + name);
                row.type      = type;
                row.size      = e.value("size", (int64_t)0);
                row.mtime     = e.value("mtime", (int64_t)0);
                combined.push_back(std::move(row));
            }
        }
        std::sort(combined.begin(), combined.end(), [](const FileRow &a, const FileRow &b) {
            if (a.mtime != b.mtime) return a.mtime > b.mtime;
            return a.name < b.name;
        });
        post_to_ui([combined = std::move(combined)]() mutable {
            g_reports_loading = false;
            g_reports_loaded  = true;
            for (const auto &row : combined) ensure_glyphs(row.name);
            g_reports = std::move(combined);
        });
    }).detach();
}

static void refresh_device_info()
{
    if (g_device_info_loading) return;
    g_device_info_loading = true;
    api_async("device_info", {}, [](const json &r) {
        g_device_info_loading = false;
        g_device_info_loaded  = true;
        if (r.value("ok", false)) {
            g_device_info = r["result"];
        }
    });
}

static void refresh_processes()
{
    api_async("list_processes", {}, [](const json &r) {
        if (!r.value("ok", false) || !r.contains("result")) return;
        std::map<std::string, int> pids;
        const auto &res = r["result"];
        auto process_token = [&](const json &p) {
            if (!p.is_object()) return;
            int pid = (int)json_get_int(p, "pid", json_get_int(p, "processIdentifier", 0));
            std::string url = json_get_str(p, "executableURL", json_get_str(p, "executable", ""));
            if (pid <= 0 || url.empty()) return;
            auto slash = url.rfind('/');
            std::string exe = (slash != std::string::npos) ? url.substr(slash + 1) : url;
            /* Decode %20 in executable URLs */
            std::string clean;
            for (size_t i = 0; i < exe.size(); i++) {
                if (i + 2 < exe.size() && exe[i] == '%' && exe[i + 1] == '2' && exe[i + 2] == '0') {
                    clean.push_back(' ');
                    i += 2;
                } else {
                    clean.push_back(exe[i]);
                }
            }
            pids[clean] = pid;
        };
        if (res.is_array()) {
            for (const auto &p : res) process_token(p);
        } else if (res.is_object()) {
            if (res.contains("processTokens") && res["processTokens"].is_array()) {
                for (const auto &p : res["processTokens"]) process_token(p);
            }
        }
        g_running_pids = std::move(pids);
    });
}

static void refresh_apps()
{
    if (g_apps_loading) return;
    g_apps_loading = true;
    api_async("list_apps", {}, [](const json &r) {
        g_apps_loading = false;
        g_apps_loaded  = true;
        if (!r.value("ok", false) || !r.contains("result") || !r["result"].is_array()) {
            show_toast("list_apps: " + json_err_str(r, "failed"), 5000);
            return;
        }
        std::vector<AppRow> rows;
        for (const auto &a : r["result"]) {
            if (!a.is_object()) continue;
            AppRow row;
            row.bundle_id   = json_get_str(a, "bundleIdentifier", "");
            row.name        = json_get_str(a, "name", row.bundle_id);
            row.version     = json_get_str(a, "version", "");
            row.first_party = json_get_bool(a, "isFirstParty", false);
            row.developer   = json_get_bool(a, "isDeveloper", false);
            row.app_clip    = json_get_bool(a, "isAppClip", false);
            auto it = g_app_icon_cache.find(row.bundle_id);
            if (it != g_app_icon_cache.end()) {
                row.icon_tex = it->second;
                row.icon_requested = true;
            }
            ensure_glyphs(row.name);
            rows.push_back(std::move(row));
        }
        std::sort(rows.begin(), rows.end(), [](const AppRow &a, const AppRow &b) {
            return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
        });
        g_apps = std::move(rows);
        refresh_processes();
    });
}

static void request_app_icon(AppRow &app)
{
    if (app.icon_requested || app.bundle_id.empty() || g_icons_in_flight >= 4) return;
    auto cached = g_app_icon_cache.find(app.bundle_id);
    if (cached != g_app_icon_cache.end()) {
        app.icon_tex = cached->second;
        app.icon_requested = true;
        return;
    }
    app.icon_requested = true;
    g_icons_in_flight++;
    std::string bid = app.bundle_id;
    std::thread([bid]() {
        json r = api_call("get_app_icon", {{"bundle_id", bid}}, 10);
        rplayhub::RgbaImage decoded;
        if (r.value("ok", false)) {
            std::string b64 = r["result"].value("png_b64", "");
            if (!b64.empty()) {
                auto bytes = b64_decode(b64);
                decoded = rplayhub::decodePngToRgba(bytes.data(), bytes.size());
            }
        }
        post_to_ui([bid, img = std::move(decoded)]() {
            g_icons_in_flight = std::max(0, g_icons_in_flight - 1);
            SDL_Texture *tex = nullptr;
            if (img.valid() && g_ren) {
                tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, img.width, img.height);
                if (tex) {
                    SDL_UpdateTexture(tex, nullptr, img.rgba.data(), img.width * 4);
                    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
                    SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
                }
            }
            g_app_icon_cache[bid] = tex;
            for (auto &row : g_apps) {
                if (row.bundle_id == bid) { row.icon_tex = tex; break; }
            }
        });
    }).detach();
}

static void refresh_profiles()
{
    if (g_profiles_loading) return;
    g_profiles_loading = true;
    api_async("list_profiles", {}, [](const json &r) {
        g_profiles_loading = false;
        g_profiles_loaded  = true;
        if (!r.value("ok", false)) {
            show_toast("list_profiles: " + json_err_str(r, "failed"), 5000);
            return;
        }
        g_prov_profiles.clear();
        for (const auto &p : r["result"].value("provisioning", json::array())) {
            ProvProfile pr;
            pr.uuid        = p.value("uuid", "");
            pr.name        = p.value("name", pr.uuid);
            pr.app_id_name = p.value("app_id_name", "");
            pr.team        = p.value("team", "");
            pr.expiration  = p.value("expiration", "");
            ensure_glyphs(pr.name);
            ensure_glyphs(pr.team);
            g_prov_profiles.push_back(std::move(pr));
        }
        g_cfg_profiles.clear();
        for (const auto &c : r["result"].value("configuration", json::array())) {
            ConfigProfile cp;
            cp.identifier   = c.value("identifier", "");
            cp.uuid         = c.value("uuid", "");
            cp.display_name = c.value("display_name", cp.identifier);
            cp.organization = c.value("organization", "");
            ensure_glyphs(cp.display_name);
            ensure_glyphs(cp.organization);
            g_cfg_profiles.push_back(std::move(cp));
        }
        g_selected_prov_idx = -1;
        g_selected_cfg_idx  = -1;
    });
}

static void refresh_files()
{
    if (g_files_loading) return;
    g_files_loading = true;
    std::string svc  = g_file_service;
    std::string path = g_file_path;
    api_async("list_dir", {{"service", svc}, {"path", path}}, [](const json &r) {
        g_files_loading = false;
        g_files_loaded  = true;
        if (!r.value("ok", false)) {
            show_toast("list_dir: " + json_err_str(r, "failed"), 5000);
            return;
        }
        std::vector<FileRow> rows;
        for (const auto &e : r["result"].value("entries", json::array())) {
            FileRow row;
            row.name      = e.value("name", "");
            row.full_path = (g_file_path == "/" ? "" : g_file_path) + "/" + row.name;
            row.type      = e.value("type", "file");
            row.size      = e.value("size", (int64_t)0);
            row.mtime     = e.value("mtime", (int64_t)0);
            ensure_glyphs(row.name);
            rows.push_back(std::move(row));
        }
        std::sort(rows.begin(), rows.end(), [](const FileRow &a, const FileRow &b) {
            if ((a.type == "dir") != (b.type == "dir")) return a.type == "dir";
            return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
        });
        g_files = std::move(rows);
    });
}

static void save_remote_file(const std::string &service, const std::string &remote_path,
                             const std::string &filename, bool open_after = false)
{
    show_toast("Downloading " + filename + "…", 10000);
    api_async("read_file", {{"service", service}, {"path", remote_path}}, [filename, open_after](const json &r) {
        if (!r.value("ok", false)) {
            show_toast("Download failed: " + json_err_str(r, "unknown"), 6000);
            return;
        }
        auto bytes = b64_decode(r["result"].value("data_b64", ""));
        std::string out_path = ensure_media_dir("Downloads") + "/" + filename;
        FILE *fp = fopen(out_path.c_str(), "wb");
        if (!fp) {
            show_toast("Cannot write " + out_path, 6000);
            return;
        }
        fwrite(bytes.data(), 1, bytes.size(), fp);
        fclose(fp);
        show_toast("Saved " + out_path + " (" + format_bytes((int64_t)bytes.size()) + ")", 5000);
        if (open_after) {
#ifdef _WIN32
            ShellExecuteA(NULL, "open", out_path.c_str(), NULL, NULL, SW_SHOWNORMAL);
#else
            std::string cmd = "xdg-open '" + out_path + "' >/dev/null 2>&1 &";
            int rc = system(cmd.c_str());
            (void)rc;
#endif
        }
    }, 60);
}

static void start_syslog()
{
    if (g_syslog_running) return;
    g_syslog_running = true;
    std::thread([]() {
        int fd = tcp_connect(g_host, g_api_port);
        if (fd < 0) { g_syslog_running = false; return; }
        g_syslog_fd = fd;
        const char req[] = "{\"id\":1,\"method\":\"syslog\"}\n";
        if (send(fd, req, sizeof req - 1, 0) != (ssize_t)(sizeof req - 1)) {
            close(fd); g_syslog_fd = -1; g_syslog_running = false; return;
        }
        std::string line;
        char buf[4096];
        ssize_t n;
        while (g_syslog_running && (n = recv(fd, (char *)buf, (int)sizeof buf, 0)) > 0) {
            for (ssize_t i = 0; i < n; i++) {
                if (buf[i] == '\n') {
                    try {
                        auto j = json::parse(line);
                        if (j.value("event", "") == "syslog") {
                            std::string s = j.value("line", "");
                            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
                            std::lock_guard<std::mutex> lk(g_syslog_mu);
                            g_syslog_lines.push_back(std::move(s));
                            if (g_syslog_lines.size() > 2500) g_syslog_lines.pop_front();
                        }
                    } catch (...) {}
                    line.clear();
                } else {
                    line.push_back(buf[i]);
                }
            }
        }
        if (g_syslog_fd >= 0) { close(g_syslog_fd); g_syslog_fd = -1; }
        g_syslog_running = false;
    }).detach();
}

static void stop_syslog()
{
    g_syslog_running = false;
    int fd = g_syslog_fd.exchange(-1);
    if (fd >= 0) { shutdown(fd, SHUT_RDWR); close(fd); }
}

/* =========================================================================
 * 8. Device Actions (Screenshot, Recording, Install IPA/Profile, Pop-out Window)
 * ========================================================================= */
static void take_screenshot_action()
{
    show_toast("Taking screenshot…", 5000);
    api_async("take_screenshot", {}, [](const json &r) {
        if (!r.value("ok", false)) {
            show_toast("Screenshot failed: " + json_err_str(r, "unknown"), 6000);
            return;
        }
        auto png = b64_decode(r["result"].value("image_b64", ""));
        std::string path = ensure_media_dir("Pictures") + "/" + timestamp_filename("rPlayHub_iOS", ".png");
        FILE *f = fopen(path.c_str(), "wb");
        if (!f) { show_toast("Cannot write " + path, 6000); return; }
        fwrite(png.data(), 1, png.size(), f);
        fclose(f);
        show_toast("Saved " + path, 5000);
    });
}

static void toggle_recording_action()
{
    if (!g_recording) {
        api_async("start_recording", {}, [](const json &r) {
            if (r.value("ok", false)) {
                g_recording = true;
                g_recording_path = r["result"].value("path", "");
                show_toast("Recording to " + g_recording_path, 4000);
            } else {
                show_toast("Record failed: " + json_err_str(r, "unknown"), 6000);
            }
        });
    } else {
        api_async("stop_recording", {}, [](const json &r) {
            g_recording = false;
            if (r.value("ok", false)) {
                show_toast("Saved recording: " + r["result"].value("path", g_recording_path), 5000);
            } else {
                show_toast("Stop recording: " + json_err_str(r, "failed"), 6000);
            }
        });
    }
}

static void install_ipa_path(const std::string &path)
{
    if (path.empty()) return;
    std::string fname = path.substr(path.rfind('/') + 1);
    show_toast("Installing " + fname + "… (may take 10–30s)", 30000);
    api_async("install_app", {{"path", path}}, [fname](const json &r) {
        if (r.value("ok", false)) {
            show_toast("Installed " + fname, 5000);
            g_apps_loaded = false;
            refresh_apps();
        } else {
            show_toast("Install failed: " + json_err_str(r, "unknown"), 8000);
        }
    }, 120);
}

static void install_profile_path(const std::string &path)
{
    if (path.empty()) return;
    std::string fname = path.substr(path.rfind('/') + 1);
    show_toast("Installing profile " + fname + "…", 10000);
    api_async("install_profile", {{"path", path}}, [fname](const json &r) {
        if (r.value("ok", false)) {
            show_toast("Installed profile " + fname, 5000);
            g_profiles_loaded = false;
            refresh_profiles();
        } else {
            show_toast("Profile install failed: " + json_err_str(r, "unknown"), 8000);
        }
    }, 30);
}

static void handle_dropped_file(const std::string &path)
{
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".ipa") == 0) {
        install_ipa_path(path);
    } else if ((lower.size() > 16 && lower.compare(lower.size() - 16, 16, ".mobileprovision") == 0) ||
               (lower.size() > 13 && lower.compare(lower.size() - 13, 13, ".mobileconfig") == 0)) {
        install_profile_path(path);
    } else {
        show_toast("Dropped file: use .ipa to install an app or .mobileprovision / .mobileconfig for profiles", 5000);
    }
}

static void toggle_popout_window()
{
    if (g_popout_win && !g_popout_win->closeRequested()) {
        g_popout_win->requestClose("Bring Back");
        return;
    }
    int w = 380, h = 820;
    if (g_vtex_w > 0 && g_vtex_h > 0) {
        float aspect = (float)g_vtex_w / (float)g_vtex_h;
        w = (int)std::lround(370.0f * g_scale);
        h = rplayhub::DisplayWindow::bareHeightForWidth(w, aspect);
    }
    std::string title = g_bound_name.empty() ? "iPhone" : g_bound_name;
    g_popout_win = std::make_unique<rplayhub::DisplayWindow>(0, title, w, h, false);
    if (!g_popout_win->valid()) {
        g_popout_win.reset();
        show_toast("Failed to create pop-out window", 5000);
        return;
    }
    rplayhub::DisplayChrome chrome;
    chrome.regular_font = g_regular_font_path;
    chrome.bold_font    = g_bold_font_path;
    chrome.cjk_font     = g_cjk_font_path;
    chrome.cjk_face     = g_cjk_font_face;
    chrome.scale        = g_scale;
    chrome.recording    = []() { return g_recording; };
    chrome.on_action    = [](rplayhub::ChromeAction act) {
        switch (act) {
        case rplayhub::ChromeAction::Home:
        case rplayhub::ChromeAction::Back:
            api_async("press_button", {{"button", "home"}});
            break;
        case rplayhub::ChromeAction::Power:
            api_async("device_action", {{"action", "sleep"}});
            break;
        case rplayhub::ChromeAction::Rotate:
            g_rotation = (g_rotation + 1) & 3;
            break;
        case rplayhub::ChromeAction::Screenshot:
            take_screenshot_action();
            break;
        case rplayhub::ChromeAction::Record:
            toggle_recording_action();
            break;
        default:
            break;
        }
    };
    g_popout_win->setChrome(chrome);
    g_view_screen = true;
}

/* =========================================================================
 * 9. Borderless Window Hit-Testing & Resize Cursors
 * ========================================================================= */
static SDL_HitTestResult window_hit_test(SDL_Window *win, const SDL_Point *pt, void *)
{
    int w = 0, h = 0;
    SDL_GetWindowSize(win, &w, &h);
    const bool maximized = (SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED) != 0;
    const int edge = (int)std::max(8.0f, 7.0f * g_scale);
    if (!maximized) {
        bool l = pt->x < edge, r = pt->x >= w - edge, t = pt->y < edge, b = pt->y >= h - edge;
        if (t && l) return SDL_HITTEST_RESIZE_TOPLEFT;
        if (t && r) return SDL_HITTEST_RESIZE_TOPRIGHT;
        if (b && l) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
        if (b && r) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
        if (t) return SDL_HITTEST_RESIZE_TOP;
        if (b) return SDL_HITTEST_RESIZE_BOTTOM;
        if (l) return SDL_HITTEST_RESIZE_LEFT;
        if (r) return SDL_HITTEST_RESIZE_RIGHT;
    }
    if (pt->y < g_menu_h) {
        for (const ImVec4 &r : g_no_drag_rects) {
            if (pt->x >= r.x && pt->x < r.z && pt->y >= r.y && pt->y < r.w) {
                return SDL_HITTEST_NORMAL;
            }
        }
        return SDL_HITTEST_DRAGGABLE;
    }
    return SDL_HITTEST_NORMAL;
}

static void update_resize_cursor()
{
    if (g_system_titlebar || !g_win) return;
    Uint32 win_flags = SDL_GetWindowFlags(g_win);
    if (win_flags & SDL_WINDOW_MAXIMIZED) {
        if (g_cursor_overridden) {
            SDL_SetCursor(g_cursor_arrow);
            ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
            g_cursor_overridden = false;
        }
        g_is_resizing_border = false;
        g_active_resize_cursor = nullptr;
        return;
    }

    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(g_win, &win_w, &win_h);
    int mx = 0, my = 0;
    Uint32 buttons = SDL_GetMouseState(&mx, &my);
    const bool left_down = (buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;

    if (left_down) {
        if (g_is_resizing_border && g_active_resize_cursor) {
            SDL_SetCursor(g_active_resize_cursor);
            ImGui::SetMouseCursor(g_active_resize_imgui_cursor);
            g_cursor_overridden = true;
        }
        return;
    }

    g_is_resizing_border = false;
    g_active_resize_cursor = nullptr;

    if (mx < 0 || mx >= win_w || my < 0 || my >= win_h) {
        if (g_cursor_overridden) {
            SDL_SetCursor(g_cursor_arrow);
            ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
            g_cursor_overridden = false;
        }
        return;
    }

    const int edge = (int)std::max(8.0f, 7.0f * g_scale);
    bool l = (mx < edge), r = (mx >= win_w - edge), t = (my < edge), b = (my >= win_h - edge);

    SDL_Cursor *target = nullptr;
    ImGuiMouseCursor imgui_target = ImGuiMouseCursor_Arrow;
    if ((t && l) || (b && r)) { target = g_cursor_resize_nwse; imgui_target = ImGuiMouseCursor_ResizeNWSE; }
    else if ((t && r) || (b && l)) { target = g_cursor_resize_nesw; imgui_target = ImGuiMouseCursor_ResizeNESW; }
    else if (l || r) { target = g_cursor_resize_ew; imgui_target = ImGuiMouseCursor_ResizeEW; }
    else if (t || b) { target = g_cursor_resize_ns; imgui_target = ImGuiMouseCursor_ResizeNS; }

    if (target) {
        g_is_resizing_border = true;
        g_active_resize_cursor = target;
        g_active_resize_imgui_cursor = imgui_target;
        SDL_SetCursor(target);
        ImGui::SetMouseCursor(imgui_target);
        g_cursor_overridden = true;
    } else if (g_cursor_overridden) {
        SDL_SetCursor(g_cursor_arrow);
        ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
        g_cursor_overridden = false;
    }
}

/* =========================================================================
 * 10. UI Rendering — Titlebar / Unified Toolbar
 * ========================================================================= */
static void render_device_menu_items()
{
    if (rplayhub::MenuItemWithIcon("Refresh Devices", "Ctrl+R", rplayhub::Icons::drawRefresh, g_scale))
        refresh_devices();
    ImGui::Separator();
    if (rplayhub::MenuItemWithIcon("Home Button", "Ctrl+Shift+H", rplayhub::Icons::drawHome, g_scale))
        api_async("press_button", {{"button", "home"}});
    if (rplayhub::MenuItemWithIcon("Rotate View", "Ctrl+L", rplayhub::Icons::drawRotate, g_scale))
        g_rotation = (g_rotation + 1) & 3;
    if (rplayhub::MenuItemWithIcon("Take Screenshot", "Ctrl+S", rplayhub::Icons::drawCamera, g_scale))
        take_screenshot_action();
    if (rplayhub::MenuItemWithIcon(g_recording ? "Stop Recording" : "Start Recording", nullptr,
                                   rplayhub::Icons::drawRecord, g_scale))
        toggle_recording_action();
    ImGui::Separator();
    if (rplayhub::MenuItemWithIcon("Sleep / Wake", nullptr, rplayhub::Icons::drawPower, g_scale))
        api_async("device_action", {{"action", "sleep"}});
    if (rplayhub::MenuItemWithIcon("Restart Device", nullptr, rplayhub::Icons::drawRefresh, g_scale))
        api_async("device_action", {{"action", "restart"}});
    if (rplayhub::MenuItemWithIcon("Shut Down Device…", nullptr, rplayhub::Icons::drawDisconnect, g_scale))
        g_open_shutdown_modal = true;
}

static void render_view_menu_items()
{
    if (ImGui::MenuItem("Show Devices Sidebar", nullptr, !g_sidebar_hidden))
        g_sidebar_hidden = !g_sidebar_hidden;
    if (ImGui::MenuItem("Show Inspector Pane", nullptr, g_inspector_visible))
        g_inspector_visible = !g_inspector_visible;
    ImGui::Separator();
    if (ImGui::MenuItem("Settings Inspector", nullptr, g_inspector_visible && g_inspector_group == 0)) {
        g_inspector_visible = true; g_inspector_group = 0;
    }
    if (ImGui::MenuItem("Report Inspector", nullptr, g_inspector_visible && g_inspector_group == 1)) {
        g_inspector_visible = true; g_inspector_group = 1;
    }
    if (ImGui::MenuItem("Info Inspector", nullptr, g_inspector_visible && g_inspector_group == 2)) {
        g_inspector_visible = true; g_inspector_group = 2;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("3D Device View", nullptr, g_twin_mode)) {
        g_twin_mode = !g_twin_mode;
        if (g_twin_mode) { g_fold_only_view = false; g_twin.setFoldOnly(false); }
    }
    if (ImGui::MenuItem("Fold View", nullptr, g_fold_only_view)) {
        g_fold_only_view = !g_fold_only_view;
        if (g_fold_only_view) { g_twin_mode = false; g_twin_demo = false; g_twin.setFoldOnly(true); g_twin.noteShown(); }
        else g_twin.setFoldOnly(false);
    }
    if (ImGui::BeginMenu("Fold Look")) {
        if (ImGui::MenuItem("1  Hard cut (glued to glass)", "1", g_twin.renderMode() == 0)) g_twin.setRenderMode(0);
        if (ImGui::MenuItem("2  Locked (content plane fixed)", "2", g_twin.renderMode() == 1)) g_twin.setRenderMode(1);
        if (ImGui::MenuItem("3  Stylized (iPhone Duo look)", "3", g_twin.renderMode() == 2)) g_twin.setRenderMode(2);
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Show 3D Demo", nullptr, g_twin_demo)) {
        g_twin_demo = !g_twin_demo;
        if (g_twin_demo) {
            g_twin_mode = true;
            g_fold_only_view = false;
            g_twin.setFoldOnly(false);
            g_twin_demo_start = std::chrono::steady_clock::now();
        }
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Open Screen in New Tab", "Ctrl+T")) open_screen_tab();
    const bool popped_v = g_popout_win && !g_popout_win->closeRequested();
    if (ImGui::MenuItem(popped_v ? "Bring Screen Back" : "Open Screen in New Window", "Ctrl+N")) toggle_popout_window();
    if (ImGui::MenuItem("Naked View (Pop-out Window)")) {
        if (!popped_v) toggle_popout_window();
    }
    if (ImGui::MenuItem("Pin Window on Top", "Ctrl+P", g_pinned)) {
        g_pinned = !g_pinned;
        SDL_SetWindowAlwaysOnTop(g_win, g_pinned ? SDL_TRUE : SDL_FALSE);
    }
}

static void render_window_menu_items()
{
    if (ImGui::MenuItem("Pin Window on Top", "Ctrl+P", g_pinned)) {
        g_pinned = !g_pinned;
        SDL_SetWindowAlwaysOnTop(g_win, g_pinned ? SDL_TRUE : SDL_FALSE);
    }
    const bool popped = g_popout_win && !g_popout_win->closeRequested();
    if (ImGui::MenuItem(popped ? "Bring Screen Back" : "Open Screen in New Window", "Ctrl+N")) {
        toggle_popout_window();
    }
    if (ImGui::MenuItem("Open Screen in New Tab", "Ctrl+T")) {
        open_screen_tab();
    }
    if (ImGui::MenuItem("Naked View (Pop-out Window)")) {
        if (!popped) toggle_popout_window();
    }
}

static void render_menus_popup()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * g_scale, 8.0f * g_scale));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 8.0f * g_scale);
    if (ImGui::BeginPopup("##MainMenusPopup")) {
        if (ImGui::BeginMenu("Device")) { render_device_menu_items(); ImGui::EndMenu(); }
        if (ImGui::BeginMenu("View"))   { render_view_menu_items();   ImGui::EndMenu(); }
        if (ImGui::BeginMenu("Window")) { render_window_menu_items(); ImGui::EndMenu(); }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

#ifdef _WIN32
enum WinMenuId {
    IDM_DEV_REFRESH = 1001,
    IDM_DEV_HOME,
    IDM_DEV_ROTATE,
    IDM_DEV_SCREENSHOT,
    IDM_DEV_RECORD,
    IDM_DEV_SLEEP,
    IDM_DEV_RESTART,
    IDM_DEV_SHUTDOWN,

    IDM_VIEW_SIDEBAR = 1020,
    IDM_VIEW_INSPECTOR,
    IDM_VIEW_INSP_SETTINGS,
    IDM_VIEW_INSP_REPORT,
    IDM_VIEW_INSP_INFO,
    IDM_VIEW_TWIN_3D,
    IDM_VIEW_FOLD,
    IDM_VIEW_FOLD_LOOK_1,
    IDM_VIEW_FOLD_LOOK_2,
    IDM_VIEW_FOLD_LOOK_3,
    IDM_VIEW_TWIN_DEMO,
    IDM_VIEW_NEW_TAB,
    IDM_VIEW_NEW_WIN,
    IDM_VIEW_NAKED,

    IDM_WIN_ALWAYS_ON_TOP = 1040,
    IDM_WIN_POPOUT,
    IDM_WIN_NEW_TAB,
    IDM_WIN_NAKED,

    IDM_HELP_ABOUT = 1060,
};

static WNDPROC g_win32_orig_wndproc = nullptr;

static LRESULT CALLBACK Win32MenuSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (uMsg == WM_INITMENUPOPUP) {
        HMENU hMenu = (HMENU)wParam;
        CheckMenuItem(hMenu, IDM_VIEW_SIDEBAR, MF_BYCOMMAND | (!g_sidebar_hidden ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(hMenu, IDM_VIEW_INSPECTOR, MF_BYCOMMAND | (g_inspector_visible ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(hMenu, IDM_VIEW_INSP_SETTINGS, MF_BYCOMMAND | (g_inspector_visible && g_inspector_group == 0 ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(hMenu, IDM_VIEW_INSP_REPORT, MF_BYCOMMAND | (g_inspector_visible && g_inspector_group == 1 ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(hMenu, IDM_VIEW_INSP_INFO, MF_BYCOMMAND | (g_inspector_visible && g_inspector_group == 2 ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(hMenu, IDM_VIEW_TWIN_3D, MF_BYCOMMAND | (g_twin_mode ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(hMenu, IDM_VIEW_FOLD, MF_BYCOMMAND | (g_fold_only_view ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(hMenu, IDM_VIEW_FOLD_LOOK_1, MF_BYCOMMAND | (g_twin.renderMode() == 0 ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(hMenu, IDM_VIEW_FOLD_LOOK_2, MF_BYCOMMAND | (g_twin.renderMode() == 1 ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(hMenu, IDM_VIEW_FOLD_LOOK_3, MF_BYCOMMAND | (g_twin.renderMode() == 2 ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(hMenu, IDM_VIEW_TWIN_DEMO, MF_BYCOMMAND | (g_twin_demo ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(hMenu, IDM_WIN_ALWAYS_ON_TOP, MF_BYCOMMAND | (g_pinned ? MF_CHECKED : MF_UNCHECKED));

        const bool popped = g_popout_win && !g_popout_win->closeRequested();
        ModifyMenuW(hMenu, IDM_WIN_POPOUT, MF_BYCOMMAND | MF_STRING, IDM_WIN_POPOUT,
                    popped ? L"Bring Screen &Back\tCtrl+N" : L"&Open Screen in New Window\tCtrl+N");
        ModifyMenuW(hMenu, IDM_VIEW_NEW_WIN, MF_BYCOMMAND | MF_STRING, IDM_VIEW_NEW_WIN,
                    popped ? L"Bring Screen &Back\tCtrl+N" : L"&Open Screen in New Window\tCtrl+N");
        ModifyMenuW(hMenu, IDM_DEV_RECORD, MF_BYCOMMAND | MF_STRING, IDM_DEV_RECORD,
                    g_recording ? L"Stop &Recording" : L"Start &Recording");
    } else if (uMsg == WM_COMMAND) {
        int id = LOWORD(wParam);
        switch (id) {
        case IDM_DEV_REFRESH:
            refresh_devices();
            break;
        case IDM_DEV_HOME:
            api_async("press_button", {{"button", "home"}});
            break;
        case IDM_DEV_ROTATE:
            g_rotation = (g_rotation + 1) & 3;
            break;
        case IDM_DEV_SCREENSHOT:
            take_screenshot_action();
            break;
        case IDM_DEV_RECORD:
            toggle_recording_action();
            break;
        case IDM_DEV_SLEEP:
            api_async("device_action", {{"action", "sleep"}});
            break;
        case IDM_DEV_RESTART:
            api_async("device_action", {{"action", "restart"}});
            break;
        case IDM_DEV_SHUTDOWN:
            g_open_shutdown_modal = true;
            break;
        case IDM_VIEW_SIDEBAR:
            g_sidebar_hidden = !g_sidebar_hidden;
            break;
        case IDM_VIEW_INSPECTOR:
            g_inspector_visible = !g_inspector_visible;
            break;
        case IDM_VIEW_INSP_SETTINGS:
            g_inspector_visible = true; g_inspector_group = 0;
            break;
        case IDM_VIEW_INSP_REPORT:
            g_inspector_visible = true; g_inspector_group = 1;
            break;
        case IDM_VIEW_INSP_INFO:
            g_inspector_visible = true; g_inspector_group = 2;
            break;
        case IDM_VIEW_TWIN_3D:
            g_twin_mode = !g_twin_mode;
            if (g_twin_mode) { g_fold_only_view = false; g_twin.setFoldOnly(false); }
            break;
        case IDM_VIEW_FOLD:
            g_fold_only_view = !g_fold_only_view;
            if (g_fold_only_view) { g_twin_mode = false; g_twin_demo = false; g_twin.setFoldOnly(true); g_twin.noteShown(); }
            else g_twin.setFoldOnly(false);
            break;
        case IDM_VIEW_FOLD_LOOK_1:
            g_twin.setRenderMode(0);
            break;
        case IDM_VIEW_FOLD_LOOK_2:
            g_twin.setRenderMode(1);
            break;
        case IDM_VIEW_FOLD_LOOK_3:
            g_twin.setRenderMode(2);
            break;
        case IDM_VIEW_TWIN_DEMO:
            g_twin_demo = !g_twin_demo;
            if (g_twin_demo) {
                g_twin_mode = true;
                g_fold_only_view = false;
                g_twin.setFoldOnly(false);
                g_twin_demo_start = std::chrono::steady_clock::now();
            }
            break;
        case IDM_VIEW_NEW_TAB:
        case IDM_WIN_NEW_TAB:
            open_screen_tab();
            break;
        case IDM_VIEW_NEW_WIN:
        case IDM_WIN_POPOUT:
            toggle_popout_window();
            break;
        case IDM_VIEW_NAKED:
        case IDM_WIN_NAKED:
            if (!g_popout_win || g_popout_win->closeRequested()) {
                toggle_popout_window();
            }
            break;
        case IDM_WIN_ALWAYS_ON_TOP:
            g_pinned = !g_pinned;
            SDL_SetWindowAlwaysOnTop(g_win, g_pinned ? SDL_TRUE : SDL_FALSE);
            break;
        case IDM_HELP_ABOUT:
            MessageBoxW(hWnd, L"rPlayHub \xe2\x80\x94 High-performance iOS Screen Mirroring & Control\nVersion 1.0.0\n\nCopyright \xc2\xa9 2026 rPlayAI Team", L"About rPlayHub", MB_OK | MB_ICONINFORMATION);
            break;
        }
        return 0;
    }
    return CallWindowProc(g_win32_orig_wndproc, hWnd, uMsg, wParam, lParam);
}

static void setup_win32_native_menu(HWND hwnd)
{
    HMENU hMenuBar = CreateMenu();

    // Device Menu
    HMENU hMenuDev = CreatePopupMenu();
    AppendMenuW(hMenuDev, MF_STRING, IDM_DEV_REFRESH, L"&Refresh Devices\tCtrl+R");
    AppendMenuW(hMenuDev, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenuDev, MF_STRING, IDM_DEV_HOME, L"&Home Button\tCtrl+Shift+H");
    AppendMenuW(hMenuDev, MF_STRING, IDM_DEV_ROTATE, L"Ro&tate View\tCtrl+L");
    AppendMenuW(hMenuDev, MF_STRING, IDM_DEV_SCREENSHOT, L"Take &Screenshot\tCtrl+S");
    AppendMenuW(hMenuDev, MF_STRING, IDM_DEV_RECORD, L"Start &Recording");
    AppendMenuW(hMenuDev, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenuDev, MF_STRING, IDM_DEV_SLEEP, L"&Sleep / Wake");
    AppendMenuW(hMenuDev, MF_STRING, IDM_DEV_RESTART, L"Res&tart Device");
    AppendMenuW(hMenuDev, MF_STRING, IDM_DEV_SHUTDOWN, L"Shut &Down Device…");
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hMenuDev, L"&Device");

    // View Menu
    HMENU hMenuView = CreatePopupMenu();
    AppendMenuW(hMenuView, MF_STRING, IDM_VIEW_SIDEBAR, L"Show Devices &Sidebar");
    AppendMenuW(hMenuView, MF_STRING, IDM_VIEW_INSPECTOR, L"Show &Inspector Pane");
    AppendMenuW(hMenuView, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenuView, MF_STRING, IDM_VIEW_INSP_SETTINGS, L"&Settings Inspector");
    AppendMenuW(hMenuView, MF_STRING, IDM_VIEW_INSP_REPORT, L"&Report Inspector");
    AppendMenuW(hMenuView, MF_STRING, IDM_VIEW_INSP_INFO, L"&Info Inspector");
    AppendMenuW(hMenuView, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenuView, MF_STRING, IDM_VIEW_TWIN_3D, L"&3D Device View");
    AppendMenuW(hMenuView, MF_STRING, IDM_VIEW_FOLD, L"&Fold View");

    HMENU hMenuFoldLook = CreatePopupMenu();
    AppendMenuW(hMenuFoldLook, MF_STRING, IDM_VIEW_FOLD_LOOK_1, L"&1  Hard cut (glued to glass)");
    AppendMenuW(hMenuFoldLook, MF_STRING, IDM_VIEW_FOLD_LOOK_2, L"&2  Locked (content plane fixed)");
    AppendMenuW(hMenuFoldLook, MF_STRING, IDM_VIEW_FOLD_LOOK_3, L"&3  Stylized (iPhone Duo look)");
    AppendMenuW(hMenuView, MF_POPUP, (UINT_PTR)hMenuFoldLook, L"Fold &Look");

    AppendMenuW(hMenuView, MF_STRING, IDM_VIEW_TWIN_DEMO, L"Show 3D &Demo");
    AppendMenuW(hMenuView, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenuView, MF_STRING, IDM_VIEW_NEW_TAB, L"Open Screen in &New Tab\tCtrl+T");
    AppendMenuW(hMenuView, MF_STRING, IDM_VIEW_NEW_WIN, L"&Open Screen in New Window\tCtrl+N");
    AppendMenuW(hMenuView, MF_STRING, IDM_VIEW_NAKED, L"Na&ked View (Pop-out Window)");
    AppendMenuW(hMenuView, MF_STRING, IDM_WIN_ALWAYS_ON_TOP, L"&Pin Window on Top\tCtrl+P");
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hMenuView, L"&View");

    // Window Menu
    HMENU hMenuWin = CreatePopupMenu();
    AppendMenuW(hMenuWin, MF_STRING, IDM_WIN_ALWAYS_ON_TOP, L"&Pin Window on Top\tCtrl+P");
    AppendMenuW(hMenuWin, MF_STRING, IDM_WIN_POPOUT, L"&Open Screen in New Window\tCtrl+N");
    AppendMenuW(hMenuWin, MF_STRING, IDM_WIN_NEW_TAB, L"Open Screen in &New Tab\tCtrl+T");
    AppendMenuW(hMenuWin, MF_STRING, IDM_WIN_NAKED, L"Na&ked View (Pop-out Window)");
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hMenuWin, L"&Window");

    // Help Menu
    HMENU hMenuHelp = CreatePopupMenu();
    AppendMenuW(hMenuHelp, MF_STRING, IDM_HELP_ABOUT, L"&About rPlayHub");
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hMenuHelp, L"&Help");

    SetMenu(hwnd, hMenuBar);
    DrawMenuBar(hwnd);

    g_win32_orig_wndproc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)Win32MenuSubclassProc);
}
#endif

static char g_device_search_filter[128] = "";

static void render_titlebar(float win_w)
{
    const float s = g_scale;
    g_menu_h = g_system_titlebar ? 42.0f * s : 48.0f * s;
    g_no_drag_rects.clear();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(win_w, g_menu_h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(247 / 255.f, 247 / 255.f, 250 / 255.f, 1.0f));
    ImGui::Begin("##TitleBar", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(0, g_menu_h - 1.0f), ImVec2(win_w, g_menu_h - 1.0f), IM_COL32(226, 226, 231, 255), 1.0f);

    const ImVec2 tb(28.0f * s, 26.0f * s);
    const float bar_y = (g_menu_h - tb.y) * 0.5f;

    auto note_no_drag = []() {
        g_no_drag_rects.push_back(ImVec4(ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y,
                                         ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y));
    };

    /* 1. Traffic lights + Left toolbar group (matching gui_app.cc::renderMenuBar) */
    float left_group_x = 14.0f * s;
    if (!g_system_titlebar) {
        const float r = 7.5f * s, gap = 24.0f * s;
        const ImVec2 pos(18.0f * s, g_menu_h * 0.5f);
        const ImU32 cols[3] = { IM_COL32(255, 95, 87, 255), IM_COL32(254, 188, 46, 255), IM_COL32(40, 200, 64, 255) };
        const ImU32 rims[3] = { IM_COL32(225, 70, 62, 255), IM_COL32(222, 160, 30, 255), IM_COL32(30, 170, 50, 255) };
        ImVec2 box_min(pos.x - r - 4.0f * s, pos.y - r - 4.0f * s);
        ImVec2 box_sz(gap * 2.0f + r * 2.0f + 8.0f * s, r * 2.0f + 8.0f * s);
        ImGui::SetCursorScreenPos(box_min);
        ImGui::InvisibleButton("##TrafficLights", box_sz);
        note_no_drag();
        bool group_hover = ImGui::IsItemHovered();
        ImVec2 mouse = ImGui::GetMousePos();
        for (int i = 0; i < 3; i++) {
            ImVec2 c(pos.x + i * gap, pos.y);
            dl->AddCircleFilled(c, r, cols[i], 24);
            dl->AddCircle(c, r, rims[i], 24, 1.0f);
            if (group_hover) {
                float k = r * 0.42f;
                if (i == 0) {
                    dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), IM_COL32(70, 20, 15, 200), 1.3f * s);
                    dl->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), IM_COL32(70, 20, 15, 200), 1.3f * s);
                } else if (i == 1) {
                    dl->AddLine(ImVec2(c.x - k, c.y), ImVec2(c.x + k, c.y), IM_COL32(120, 80, 10, 220), 1.3f * s);
                } else {
                    ImU32 gg = IM_COL32(10, 80, 20, 220);
                    dl->AddTriangleFilled(ImVec2(c.x - k, c.y + k * 0.2f), ImVec2(c.x - k, c.y - k), ImVec2(c.x + k * 0.2f, c.y - k), gg);
                    dl->AddTriangleFilled(ImVec2(c.x + k, c.y - k * 0.2f), ImVec2(c.x + k, c.y + k), ImVec2(c.x - k * 0.2f, c.y + k), gg);
                }
            }
            bool over = std::fabs(mouse.x - c.x) <= r + 2.0f && std::fabs(mouse.y - c.y) <= r + 2.0f;
            if (over && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                if (i == 0) g_quit = true;
                else if (i == 1) SDL_MinimizeWindow(g_win);
                else if (SDL_GetWindowFlags(g_win) & SDL_WINDOW_MAXIMIZED) SDL_RestoreWindow(g_win);
                else SDL_MaximizeWindow(g_win);
            }
        }
        left_group_x = 92.0f * s;
    }

    /* Left white toolbar pill: Refresh + Sidebar Toggle */
    {
        ImVec2 g0(left_group_x, bar_y);
        float gw = tb.x * 2 + 2.0f * s;
        dl->AddRectFilled(ImVec2(g0.x - 4.0f * s, g0.y - 3.0f * s),
                          ImVec2(g0.x + gw + 4.0f * s, g0.y + tb.y + 3.0f * s),
                          IM_COL32(255, 255, 255, 255), 7.0f * s);
        dl->AddRect(ImVec2(g0.x - 4.0f * s, g0.y - 3.0f * s),
                    ImVec2(g0.x + gw + 4.0f * s, g0.y + tb.y + 3.0f * s),
                    IM_COL32(222, 222, 228, 255), 7.0f * s);
        ImGui::SetCursorScreenPos(g0);
        if (rplayhub::IconButton("##RefreshBtn", rplayhub::Icons::drawListMenu, tb, "Refresh devices", false)) {
            refresh_devices();
        }
        note_no_drag();
        ImGui::SameLine(0, 2.0f * s);
        if (rplayhub::IconButton("##ToggleSidebar", rplayhub::Icons::drawSidebarToggle, tb, "Toggle sidebar", !g_sidebar_hidden)) {
            g_sidebar_hidden = !g_sidebar_hidden;
        }
        note_no_drag();
    }

    /* 2. Device Pill (name over iOS version/UDID) + Stream Status (matching gui_app.cc lines 1245-1279) */
    const bool have_device = !g_bound_name.empty() || (g_selected_device_idx >= 0 && g_selected_device_idx < (int)g_devices.size());
    std::string dev_name = "No device";
    std::string dev_sub;
    if (!g_bound_name.empty()) {
        dev_name = g_bound_name;
        if (!g_bound_os_version.empty()) dev_sub = "iOS " + g_bound_os_version;
        else dev_sub = g_bound_udid;
    } else if (have_device) {
        dev_name = g_devices[g_selected_device_idx].display_name();
        if (!g_devices[g_selected_device_idx].os_version.empty())
            dev_sub = "iOS " + g_devices[g_selected_device_idx].os_version;
        else
            dev_sub = g_devices[g_selected_device_idx].udid;
    }

    ImVec2 pill_pos(left_group_x + tb.x * 2 + 24.0f * s, 5.0f * s);
    float name_w = g_font_medium ? g_font_medium->CalcTextSizeA(14.0f * s, FLT_MAX, 0.0f, dev_name.c_str()).x : ImGui::CalcTextSize(dev_name.c_str()).x;
    float sub_w  = (g_font_caption && !dev_sub.empty()) ? g_font_caption->CalcTextSizeA(11.5f * s, FLT_MAX, 0.0f, dev_sub.c_str()).x : 0.0f;
    float pill_w = std::max(name_w, sub_w) + 24.0f * s;
    float pill_h = g_menu_h - 10.0f * s;
    dl->AddRectFilled(pill_pos, ImVec2(pill_pos.x + pill_w, pill_pos.y + pill_h), IM_COL32(255, 255, 255, 255), 8.0f * s);
    dl->AddRect(pill_pos, ImVec2(pill_pos.x + pill_w, pill_pos.y + pill_h), IM_COL32(222, 222, 228, 255), 8.0f * s);
    if (dev_sub.empty()) {
        dl->AddText(g_font_medium, 14.0f * s,
                    ImVec2(pill_pos.x + 12.0f * s, pill_pos.y + (pill_h - 14.0f * s) * 0.5f),
                    IM_COL32(28, 28, 30, 255), dev_name.c_str());
    } else {
        dl->AddText(g_font_medium, 14.0f * s,
                    ImVec2(pill_pos.x + 12.0f * s, pill_pos.y + 3.0f * s),
                    IM_COL32(28, 28, 30, 255), dev_name.c_str());
        dl->AddText(g_font_caption, 11.5f * s,
                    ImVec2(pill_pos.x + 12.0f * s, pill_pos.y + pill_h - 15.0f * s),
                    IM_COL32(142, 142, 147, 255), dev_sub.c_str());
    }

    /* Live stream status text next to the pill */
    std::string status_str;
    ImU32 status_col = IM_COL32(120, 120, 128, 255);
    if (g_switching_device) {
        status_str = "Switching device…";
        status_col = IM_COL32(255, 149, 0, 255);
    } else if (g_stream_connected && g_vtex_w > 0) {
        char sbuf[128];
        double mbps = g_stream_info.is_object() ? g_stream_info.value("mbps", 0.0) : 0.0;
        float fps = g_live_fps.load();
        if (fps > 0.0f) {
            snprintf(sbuf, sizeof sbuf, "%s \xc2\xb7 %.1f Mbps \xc2\xb7 %.0f FPS", g_h264 ? "H.264" : "HEVC", mbps, fps);
        } else {
            snprintf(sbuf, sizeof sbuf, "%s \xc2\xb7 %.1f Mbps", g_h264 ? "H.264" : "HEVC", mbps);
        }
        status_str = sbuf;
        status_col = IM_COL32(52, 160, 90, 255);
    } else if (!g_bound_udid.empty()) {
        status_str = g_stream_connected ? "Connected" : "Waiting for stream…";
    }
    if (!status_str.empty() && g_font_caption) {
        dl->AddText(g_font_caption, 12.5f * s,
                    ImVec2(pill_pos.x + pill_w + 14.0f * s, pill_pos.y + (pill_h - 12.5f * s) * 0.5f),
                    status_col, status_str.c_str());
    }

    /* 3. Right Toolbar: Inspector Group Icons (Settings, Report, Info) + Primary Menu (3 dots) */
    const float insp_group_w = tb.x * 3 + 4.0f * s;
    const ImVec2 insp_g0(win_w - insp_group_w - (g_system_titlebar ? 12.0f * s : (tb.x + 28.0f * s)), bar_y);

    /* Inspector Group pill: Settings | Report | Info (matching gui_app.cc lines 1281-1295) */
    dl->AddRectFilled(ImVec2(insp_g0.x - 4.0f * s, insp_g0.y - 3.0f * s),
                      ImVec2(insp_g0.x + insp_group_w + 4.0f * s, insp_g0.y + tb.y + 3.0f * s),
                      IM_COL32(255, 255, 255, 255), 7.0f * s);
    dl->AddRect(ImVec2(insp_g0.x - 4.0f * s, insp_g0.y - 3.0f * s),
                ImVec2(insp_g0.x + insp_group_w + 4.0f * s, insp_g0.y + tb.y + 3.0f * s),
                IM_COL32(222, 222, 228, 255), 7.0f * s);
    struct InspBtn {
        const char *id;
        void (*icon)(ImDrawList *, ImVec2, float, ImU32);
        const char *tip;
        int group;
    };
    static const InspBtn kInspBtns[3] = {
        {"##InspSettings", rplayhub::Icons::drawSettings, "Settings", 0},
        {"##InspReport",   rplayhub::Icons::drawReport,   "Report (Crash & Diagnostic Logs)", 1},
        {"##InspInfo",     rplayhub::Icons::drawInfo,     "Info, Apps, Profiles, Files & Console", 2},
    };
    ImGui::SetCursorScreenPos(insp_g0);
    for (int i = 0; i < 3; i++) {
        if (i > 0) ImGui::SameLine(0, 2.0f * s);
        bool active_group = g_inspector_visible && (g_inspector_group == kInspBtns[i].group);
        if (rplayhub::IconButton(kInspBtns[i].id, kInspBtns[i].icon, tb, kInspBtns[i].tip, active_group)) {
            if (g_inspector_visible && g_inspector_group == kInspBtns[i].group) {
                g_inspector_visible = false;
            } else {
                g_inspector_visible = true;
                g_inspector_group = kInspBtns[i].group;
            }
            g_focused_pane = FocusPane::Inspector;
        }
        note_no_drag();
    }

    /* Primary Menu (three vertical dots at far right, omitted on Windows where native menu bar is used) */
    if (!g_system_titlebar) {
        ImGui::SetCursorScreenPos(ImVec2(win_w - tb.x - 10.0f * s, bar_y));
        auto dots = [](ImDrawList *d, ImVec2 p, float sz, ImU32 col) {
            float r = sz * 0.075f;
            for (int i = -1; i <= 1; ++i)
                d->AddCircleFilled(ImVec2(p.x + sz * 0.5f, p.y + sz * 0.5f + i * sz * 0.28f), r, col);
        };
        if (rplayhub::IconButton("##PrimaryMenu", dots, tb, "Menu", false)) {
            ImGui::OpenPopup("##MainMenusPopup");
        }
        note_no_drag();
        ImGui::SetNextWindowPos(ImVec2(win_w - 8.0f * s, g_menu_h), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        render_menus_popup();
    }

    /* Double-click on empty titlebar zooms window */
    ImVec2 m = ImGui::GetMousePos();
    if (m.y < g_menu_h && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered()) {
        if (SDL_GetWindowFlags(g_win) & SDL_WINDOW_MAXIMIZED) SDL_RestoreWindow(g_win);
        else SDL_MaximizeWindow(g_win);
    }

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

/* =========================================================================
 * 11. Left Device Sidebar (260pt, matching gui_app.cc::renderLeftSidebar & sidebarRow)
 * ========================================================================= */
static void render_sidebar(float height)
{
    if (g_sidebar_hidden) return;
    const float s = g_scale;
    const float width = 260.0f * s;

    ImGui::SetNextWindowPos(ImVec2(0, g_menu_h));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, rplayhub::Theme::ColorBgSidebar);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * s, 14.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    ImGui::Begin("##Sidebar", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) g_focused_pane = FocusPane::Sidebar;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(width - 1.0f, g_menu_h), ImVec2(width - 1.0f, g_menu_h + height),
                IM_COL32(230, 230, 235, 255), 1.0f);

    /* Pill-shaped Search Bar with Search Icon (matching gui_app.cc lines 2217-2226) */
    ImVec2 search_pos = ImGui::GetCursorScreenPos();
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(32.0f * s, 6.0f * s));
    ImGui::PushItemWidth(width - 24.0f * s);
    ImGui::InputTextWithHint("##DevSearch", "Search", g_device_search_filter, sizeof g_device_search_filter);
    ImGui::PopItemWidth();
    ImGui::PopStyleVar(2);
    rplayhub::Icons::drawSearch(dl, ImVec2(search_pos.x + 10.0f * s, search_pos.y + 7.0f * s),
                                16.0f * s, IM_COL32(140, 140, 145, 255));

    ImGui::Spacing();
    if (g_font_caption) ImGui::PushFont(g_font_caption);
    ImGui::TextColored(rplayhub::Theme::ColorTextSecondary, "Available");
    if (g_font_caption) ImGui::PopFont();
    ImGui::Spacing();

    const bool pane_lit = (g_focused_pane == FocusPane::Sidebar) &&
                          ((SDL_GetWindowFlags(g_win) & SDL_WINDOW_INPUT_FOCUS) != 0);
    const float row_w = width - 24.0f * s;
    const float row_h = 46.0f * s;

    int listed_devices = 0;
    for (int i = 0; i < (int)g_devices.size(); i++) {
        const auto &d = g_devices[i];
        std::string title = d.display_name();
        if (g_device_search_filter[0]) {
            if (!strcasestr(title.c_str(), g_device_search_filter) &&
                !strcasestr(d.udid.c_str(), g_device_search_filter) &&
                !strcasestr(d.product_type.c_str(), g_device_search_filter)) {
                continue;
            }
        }
        listed_devices++;

        ImGui::PushID(i);
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##devrow", ImVec2(row_w, row_h));
        bool hovered = ImGui::IsItemHovered();
        bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        bool double_clicked = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        bool selected = (i == g_selected_device_idx);
        bool lit = selected && pane_lit;

        if (selected) {
            dl->AddRectFilled(pos, ImVec2(pos.x + row_w, pos.y + row_h),
                              lit ? IM_COL32(0, 122, 255, 255) : IM_COL32(220, 220, 220, 255), 7.0f * s);
        } else if (hovered) {
            dl->AddRectFilled(pos, ImVec2(pos.x + row_w, pos.y + row_h),
                              IM_COL32(236, 236, 238, 255), 7.0f * s);
        }

        ImU32 dot_col = d.bound ? IM_COL32(52, 199, 89, 255) : IM_COL32(180, 180, 185, 255);
        dl->AddCircleFilled(ImVec2(pos.x + 13.0f * s, pos.y + row_h * 0.5f), 4.5f * s, dot_col);

        const ImU32 icon_col  = lit ? IM_COL32(255, 255, 255, 255) : IM_COL32(110, 110, 116, 255);
        const ImU32 title_col = lit ? IM_COL32(255, 255, 255, 255) : IM_COL32(28, 28, 30, 255);
        const ImU32 sub_col   = lit ? IM_COL32(235, 240, 255, 255) : IM_COL32(142, 142, 147, 255);
        const ImU32 right_col = lit ? IM_COL32(235, 240, 255, 255) : IM_COL32(128, 128, 134, 255);

        rplayhub::Icons::drawPhone(dl, ImVec2(pos.x + 24.0f * s, pos.y + (row_h - 20.0f * s) * 0.5f),
                                   20.0f * s, icon_col);

        std::string right = d.os_version;
        float right_w = 0.0f;
        if (!right.empty() && g_font_caption) {
            ImVec2 rs = g_font_caption->CalcTextSizeA(12.5f * s, FLT_MAX, 0.0f, right.c_str());
            right_w = rs.x + 12.0f * s;
            dl->AddText(g_font_caption, 12.5f * s,
                        ImVec2(pos.x + row_w - rs.x - 10.0f * s, pos.y + (row_h - rs.y) * 0.5f),
                        right_col, right.c_str());
        }

        std::string sub;
        if (const auto *m = lookup_device_model(d.product_type)) sub = m->marketing_name;
        else sub = d.product_type;
        if (!d.connection.empty()) sub += " \xc2\xb7 " + d.connection;

        const float text_x = pos.x + 54.0f * s;
        const ImVec4 clip(text_x, pos.y, pos.x + row_w - right_w, pos.y + row_h);
        if (g_font_medium) {
            dl->AddText(g_font_medium, g_font_medium->FontSize, ImVec2(text_x, pos.y + 6.0f * s),
                        title_col, title.c_str(), nullptr, 0.0f, &clip);
        } else {
            dl->AddText(ImVec2(text_x, pos.y + 6.0f * s), title_col, title.c_str());
        }
        if (g_font_caption) {
            dl->AddText(g_font_caption, 12.0f * s, ImVec2(text_x, pos.y + 25.0f * s),
                        sub_col, sub.c_str(), nullptr, 0.0f, &clip);
        }

        if (clicked) {
            g_selected_device_idx = i;
            g_focused_pane = FocusPane::Sidebar;
            if (!d.bound) {
                switch_to_device(d.udid, d.display_name());
            }
        }
        if (double_clicked) {
            g_view_screen = true;
            g_focused_pane = FocusPane::Stage;
            api_async("activate", {});
        }
        if (ImGui::BeginPopupContextItem("DeviceContextMenu")) {
            g_selected_device_idx = i;
            if (rplayhub::MenuItemWithIcon("Open in New Tab", "Ctrl+T", rplayhub::Icons::drawPlus, s)) {
                if (!d.bound) switch_to_device(d.udid, d.display_name());
                g_screen_tabs.push_back({d.udid, d.display_name()});
                g_current_tab = (int)g_screen_tabs.size() - 1;
                show_toast("Opened " + d.display_name() + " in new tab");
            }
            const bool popped_w = g_popout_win && !g_popout_win->closeRequested();
            if (rplayhub::MenuItemWithIcon(popped_w ? "Bring Screen Back" : "Open in New Window", "Ctrl+N",
                                           rplayhub::Icons::drawScreen, s)) {
                if (!d.bound) switch_to_device(d.udid, d.display_name());
                toggle_popout_window();
            }
            if (rplayhub::MenuItemWithIcon("Naked View", nullptr, rplayhub::Icons::drawScreen, s)) {
                if (!d.bound) switch_to_device(d.udid, d.display_name());
                if (!popped_w) toggle_popout_window();
            }
            ImGui::Separator();
            if (rplayhub::MenuItemWithIcon("Take Screenshot", "Ctrl+S", rplayhub::Icons::drawCamera, s)) {
                if (!d.bound) switch_to_device(d.udid, d.display_name());
                take_screenshot_action();
            }
            if (rplayhub::MenuItemWithIcon(g_recording ? "Stop Recording" : "Start / Stop Recording",
                                           nullptr, rplayhub::Icons::drawRecord, s)) {
                if (!d.bound) switch_to_device(d.udid, d.display_name());
                toggle_recording_action();
            }
            if (rplayhub::MenuItemWithIcon("Press Home", "Ctrl+Shift+H", rplayhub::Icons::drawHome, s)) {
                if (!d.bound) switch_to_device(d.udid, d.display_name());
                api_async("press_button", {{"button", "home"}});
            }
            ImGui::Separator();
            if (rplayhub::MenuItemWithIcon("Pin Window on Top", "Ctrl+P", rplayhub::Icons::drawPin, s, g_pinned)) {
                g_pinned = !g_pinned;
                SDL_SetWindowAlwaysOnTop(g_win, g_pinned ? SDL_TRUE : SDL_FALSE);
            }
            ImGui::Separator();
            if (rplayhub::MenuItemWithIcon("Sleep (Lock)…", nullptr, rplayhub::Icons::drawPower, s)) {
                if (!d.bound) switch_to_device(d.udid, d.display_name());
                api_async("device_action", {{"action", "sleep"}});
            }
            if (rplayhub::MenuItemWithIcon("Restart…", nullptr, rplayhub::Icons::drawRefresh, s)) {
                if (!d.bound) switch_to_device(d.udid, d.display_name());
                api_async("device_action", {{"action", "restart"}});
            }
            if (rplayhub::MenuItemWithIcon("Shut Down…", nullptr, rplayhub::Icons::drawDisconnect, s)) {
                if (!d.bound) switch_to_device(d.udid, d.display_name());
                g_open_shutdown_modal = true;
            }
            ImGui::Separator();
            if (rplayhub::MenuItemWithIcon("Copy UDID", nullptr, rplayhub::Icons::drawCopy, s)) {
                SDL_SetClipboardText(d.udid.c_str());
                show_toast("Copied UDID: " + d.udid);
            }
            if (rplayhub::MenuItemWithIcon("Reconnect", nullptr, rplayhub::Icons::drawRefresh, s)) {
                switch_to_device(d.udid, d.display_name());
            }
            ImGui::EndPopup();
        }
        ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + row_h + 2.0f * s));
        ImGui::PopID();
    }

    if (listed_devices == 0) {
        ImGui::Spacing();
        ImGui::TextColored(rplayhub::Theme::ColorTextSecondary, "No devices found");
        ImGui::TextColored(rplayhub::Theme::ColorTextTertiary, "Plug in USB & start cdhost");
    }

    /* Bottom status bar matching gui_app.cc lines 2402-2412 */
    ImGui::SetCursorPos(ImVec2(12.0f * s, height - 30.0f * s));
    char status_txt[128];
    if (g_devices.empty()) {
        snprintf(status_txt, sizeof status_txt, "cdhost %d — no devices", g_api_port);
    } else {
        snprintf(status_txt, sizeof status_txt, "cdhost %d — %zu %s",
                 g_api_port, g_devices.size(), g_devices.size() == 1 ? "device" : "devices");
    }
    if (g_font_caption) ImGui::PushFont(g_font_caption);
    ImGui::TextColored(rplayhub::Theme::ColorTextTertiary, "%s", status_txt);
    if (g_font_caption) ImGui::PopFont();

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

/* =========================================================================
 * 12. Center Stage — Phone Mockup, Live Mirror, 3D TwinView, Control Strip
 * ========================================================================= */
static void upload_latest_video_frame()
{
    std::lock_guard<std::mutex> lk(g_frame_mu);
    if (!g_frame_dirty || !g_latest_frame) return;
    AVFrame *f = g_latest_frame;

    /* Determine true cropped screen size so textures (including 3D TwinView & Pop-out) have zero padding */
    int model_w = g_bound_screen_w, model_h = g_bound_screen_h;
    if ((model_w <= 0 || model_h <= 0) && !g_bound_product_type.empty()) {
        if (const auto *m = lookup_device_model(g_bound_product_type)) {
            model_w = m->screen_w;
            model_h = m->screen_h;
        }
    }

    int fw = f->width;
    int fh = f->height;
    if (fw <= 0 || fh <= 0) return;

    /* g_vtex is created with the full decoded frame size (SPS dimensions, e.g. 1184x2576).
     * It is never destroyed on resolution downshifts, avoiding DirectX texture recreation stalls. */
    if (!g_vtex || g_vtex_w != fw || g_vtex_h != fh) {
        if (g_vtex) SDL_DestroyTexture(g_vtex);
        g_vtex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, fw, fh);
        if (g_vtex) SDL_SetTextureBlendMode(g_vtex, SDL_BLENDMODE_BLEND);
        g_vtex_w = fw;
        g_vtex_h = fh;
    }
    if (g_vtex) {
        SDL_UpdateYUVTexture(g_vtex, nullptr,
                             f->data[0], f->linesize[0],
                             f->data[1], f->linesize[1],
                             f->data[2], f->linesize[2]);
    }

    /* As documented in doc/RENDERING-HANDOFF.md:
     * - Full-size tier (g_active_w >= fw or g_active_w <= 0):
     *   The frame is screen plus alignment padding (e.g. 1170x2532 inside 1184x2576).
     *   The live region is model_w x model_h.
     * - Reduced tiers (g_active_w < fw):
     *   The encoder squeezes the whole screen into top-left activeSize (1088x1920 or 720x1280)
     *   with ZERO padding. Conflating them causes the picture to twitch/vibrate on downshifts! */
    int live_w = fw;
    int live_h = fh;
    if (g_active_w > 0 && g_active_h > 0 && (g_active_w < fw || g_active_h < fh)) {
        live_w = g_active_w;
        live_h = g_active_h;
    } else if (model_w > 0 && model_h > 0 && model_w <= fw && model_h <= fh) {
        live_w = model_w;
        live_h = model_h;
    }
    g_live_w = live_w;
    g_live_h = live_h;

    g_vtex_uv0 = ImVec2(0.0f, 0.0f);
    g_vtex_uv1 = ImVec2((float)live_w / (float)fw, (float)live_h / (float)fh);

    /* If pop-out DisplayWindow is active, copy planes into g_popout_frame */
    if (g_popout_win && !g_popout_win->closeRequested()) {
        int pw = live_w & ~1;
        int ph = live_h & ~1;
        g_popout_frame.width = pw;
        g_popout_frame.height = ph;
        g_popout_frame.displayWidth = pw;
        g_popout_frame.displayHeight = ph;
        g_popout_frame.displayOrientation = g_rotation;
        g_popout_frame.displayOrientationCorrection = g_rotation;
        g_popout_frame.format = rplayhub::FrameFormat::I420;
        g_popout_frame.pitch[0] = pw;
        g_popout_frame.pitch[1] = pw / 2;
        g_popout_frame.pitch[2] = pw / 2;
        g_popout_frame.planes[0].resize((size_t)pw * ph);
        g_popout_frame.planes[1].resize((size_t)(pw / 2) * (ph / 2));
        g_popout_frame.planes[2].resize((size_t)(pw / 2) * (ph / 2));
        for (int y = 0; y < ph; y++)
            memcpy(&g_popout_frame.planes[0][(size_t)y * pw], f->data[0] + y * f->linesize[0], (size_t)pw);
        for (int y = 0; y < ph / 2; y++) {
            memcpy(&g_popout_frame.planes[1][(size_t)y * (pw / 2)], f->data[1] + y * f->linesize[1], (size_t)(pw / 2));
            memcpy(&g_popout_frame.planes[2][(size_t)y * (pw / 2)], f->data[2] + y * f->linesize[2], (size_t)(pw / 2));
        }
        g_popout_frame.frameNumber = g_frame_seq;
    }
    g_frame_dirty = 0;
}

static void render_phone_mockup(ImVec2 center, ImVec2 max_size, bool popped_out)
{
    const float s = g_scale;
    ImDrawList *dl = ImGui::GetWindowDrawList();

    float phone_w = std::min(max_size.x * 0.42f, 185.0f * s);
    float phone_h = phone_w * 2.16f;
    if (phone_h > max_size.y * 0.58f) {
        phone_h = max_size.y * 0.58f;
        phone_w = phone_h / 2.16f;
    }

    float phone_cy = center.y - 46.0f * s;
    ImVec2 tl(center.x - phone_w * 0.5f, phone_cy - phone_h * 0.5f);
    ImVec2 br(center.x + phone_w * 0.5f, phone_cy + phone_h * 0.5f);

    /* Outer iPhone chassis */
    dl->AddRectFilled(tl, br, IM_COL32(12, 12, 16, 255), 24.0f * s);
    dl->AddRect(tl, br, IM_COL32(38, 38, 44, 255), 24.0f * s, 0, 1.5f * s);

    /* Inner screen area */
    float bezel = 5.0f * s;
    ImVec2 stl(tl.x + bezel, tl.y + bezel);
    ImVec2 sbr(br.x - bezel, br.y - bezel);
    dl->AddRectFilled(stl, sbr, IM_COL32(45, 52, 68, 255), 20.0f * s);

    /* Notch or Dynamic Island on the mockup */
    int cutout = infer_cutout_type(g_bound_product_type);
    if (cutout == 2) {
        float iw = phone_w * 0.28f, ih = phone_h * 0.032f;
        dl->AddRectFilled(ImVec2(center.x - iw * 0.5f, stl.y + 7.0f * s),
                          ImVec2(center.x + iw * 0.5f, stl.y + 7.0f * s + ih),
                          IM_COL32(12, 12, 16, 255), ih * 0.5f);
    } else {
        float nw = phone_w * 0.40f, nh = phone_h * 0.038f;
        dl->AddRectFilled(ImVec2(center.x - nw * 0.5f, stl.y - 1.0f),
                          ImVec2(center.x + nw * 0.5f, stl.y + nh),
                          IM_COL32(12, 12, 16, 255), nh * 0.45f);
    }

    /* Device label + iOS version below mockup */
    std::string dev_label = "No device selected";
    std::string sub_label;
    if (!g_bound_name.empty()) {
        dev_label = g_bound_name;
        if (!g_bound_os_version.empty()) sub_label = "iOS " + g_bound_os_version;
    } else if (g_selected_device_idx >= 0 && g_selected_device_idx < (int)g_devices.size()) {
        dev_label = g_devices[g_selected_device_idx].display_name();
        if (!g_devices[g_selected_device_idx].os_version.empty())
            sub_label = "iOS " + g_devices[g_selected_device_idx].os_version;
    }

    float text_y = br.y + 18.0f * s;
    ImVec2 tsz = g_font_bold->CalcTextSizeA(15.5f * s, FLT_MAX, 0.0f, dev_label.c_str());
    dl->AddText(g_font_bold, 15.5f * s, ImVec2(center.x - tsz.x * 0.5f, text_y),
                IM_COL32(28, 28, 30, 255), dev_label.c_str());
    text_y += tsz.y + 3.0f * s;

    if (popped_out) sub_label = "Showing in its own window";
    if (!sub_label.empty()) {
        ImVec2 ssz = g_font_caption->CalcTextSizeA(12.5f * s, FLT_MAX, 0.0f, sub_label.c_str());
        dl->AddText(g_font_caption, 12.5f * s, ImVec2(center.x - ssz.x * 0.5f, text_y),
                    IM_COL32(120, 120, 128, 255), sub_label.c_str());
        text_y += ssz.y + 4.0f * s;
    }

    /* "View Screen" Pill Button (matching macOS MirrorView.swift & Android GuiApp::renderPhoneMockup) */
    float btn_w = 156.0f * s, btn_h = 35.0f * s;
    float btn_x = center.x - btn_w * 0.5f, btn_y = text_y + 10.0f * s;
    ImGui::SetCursorScreenPos(ImVec2(btn_x, btn_y));
    bool clicked = ImGui::InvisibleButton("##ViewScreenBtn", ImVec2(btn_w, btn_h));
    bool active  = ImGui::IsItemActive();

    const bool lit = (g_focused_pane == FocusPane::Stage) || active;
    ImU32 btn_bg = lit ? IM_COL32(97, 85, 245, 255) : IM_COL32(220, 220, 220, 255);
    ImU32 fg_col = lit ? IM_COL32(255, 255, 255, 255) : IM_COL32(28, 28, 30, 255);
    if (active) btn_bg = IM_COL32(80, 70, 201, 255);

    dl->AddRectFilled(ImVec2(btn_x, btn_y), ImVec2(btn_x + btn_w, btn_y + btn_h), btn_bg, btn_h * 0.5f);

    float icon_sz = 17.0f * s, gap = 10.0f * s;
    const char *label = popped_out ? "Bring Back" : "View Screen";
    ImVec2 lsz = g_font_medium->CalcTextSizeA(14.0f * s, FLT_MAX, 0.0f, label);
    float total_w = icon_sz + gap + lsz.x;
    float cx = btn_x + (btn_w - total_w) * 0.5f;

    rplayhub::Icons::drawViewScreen(dl, ImVec2(cx, btn_y + (btn_h - icon_sz) * 0.5f), icon_sz, fg_col, btn_bg);
    dl->AddText(g_font_medium, 14.0f * s, ImVec2(cx + icon_sz + gap, btn_y + (btn_h - lsz.y) * 0.5f), fg_col, label);

    if (clicked) {
        g_focused_pane = FocusPane::Stage;
        if (popped_out && g_popout_win) {
            g_popout_win->requestClose("Bring Back");
        } else {
            g_view_screen = true;
            api_async("activate", {});
        }
    }
}

/* Map screen-space point inside the rotated mirror quad back to portrait normalized [0..1]x[0..1]. */
static void map_mirror_touch(ImVec2 pos, ImVec2 quad_tl, ImVec2 quad_sz, float &out_fx, float &out_fy)
{
    float rx = std::clamp((pos.x - quad_tl.x) / std::max(1.0f, quad_sz.x), 0.0f, 1.0f);
    float ry = std::clamp((pos.y - quad_tl.y) / std::max(1.0f, quad_sz.y), 0.0f, 1.0f);
    switch (g_rotation & 3) {
    case 0: out_fx = rx;        out_fy = ry;        break;
    case 1: out_fx = 1.0f - ry; out_fy = rx;        break;
    case 2: out_fx = 1.0f - rx; out_fy = 1.0f - ry; break;
    case 3: out_fx = ry;        out_fy = 1.0f - rx; break;
    }
}

static void render_live_mirror(ImVec2 origin, ImVec2 size)
{
    const float s = g_scale;
    ImDrawList *dl = ImGui::GetWindowDrawList();

    /* Presentation aspect ratio: always keep the device's physical screen aspect ratio
     * (or the full coded frame aspect ratio), NOT the anamorphic downshift tier ratio.
     * Downshift tiers (1088x1920, 720x1280) are a 16:9 anamorphic squeeze of the full 19.5:9 screen. */
    int model_w = g_bound_screen_w, model_h = g_bound_screen_h;
    if ((model_w <= 0 || model_h <= 0) && !g_bound_product_type.empty()) {
        if (const auto *m = lookup_device_model(g_bound_product_type)) {
            model_w = m->screen_w;
            model_h = m->screen_h;
        }
    }
    float native_aspect = 9.0f / 19.5f;
    if (model_w > 0 && model_h > 0) {
        native_aspect = (float)model_w / (float)model_h;
    } else {
        std::lock_guard<std::mutex> lk(g_frame_mu);
        if (g_latest_frame && g_latest_frame->width > 0 && g_latest_frame->height > 0) {
            native_aspect = (float)g_latest_frame->width / (float)g_latest_frame->height;
        }
    }
    float aspect = (g_rotation & 1) ? (1.0f / native_aspect) : native_aspect;

    const float bezel  = 10.0f * s;
    const float margin = bezel + 4.0f * s;
    float target_h = std::max(60.0f, size.y - 2.0f * margin);
    float target_w = target_h * aspect;
    if (target_w > size.x - 2.0f * margin) {
        target_w = std::max(60.0f, size.x - 2.0f * margin);
        target_h = target_w / aspect;
    }

    float px = origin.x + (size.x - target_w) * 0.5f;
    float py = origin.y + (size.y - target_h) * 0.5f;

    /* Phone chassis bezel surround */
    dl->AddRectFilled(ImVec2(px - bezel, py - bezel),
                      ImVec2(px + target_w + bezel, py + target_h + bezel),
                      IM_COL32(18, 18, 22, 255), 28.0f * s);

    rplayhub::DrawImageTurned(dl, (ImTextureID)(intptr_t)g_vtex,
                              ImVec2(px, py), ImVec2(px + target_w, py + target_h),
                              g_rotation, IM_COL32_WHITE, 20.0f * s, g_vtex_uv0, g_vtex_uv1);

    /* Notch / Dynamic Island cutout in portrait mode (ported from DeviceModel.cutoutRect) */
    if ((g_rotation & 3) == 0) {
        int cutout = infer_cutout_type(g_bound_product_type);
        if (cutout == 1) {
            float nw = target_w * 0.41f;
            float nh = target_h * 0.036f;
            dl->AddRectFilled(ImVec2(px + (target_w - nw) * 0.5f, py - 1.0f),
                              ImVec2(px + (target_w + nw) * 0.5f, py + nh),
                              IM_COL32(0, 0, 0, 255), nh * 0.42f);
        } else if (cutout == 2) {
            float iw = target_w * 0.30f;
            float ih = target_h * 0.030f;
            float iy = py + target_h * 0.011f;
            dl->AddRectFilled(ImVec2(px + (target_w - iw) * 0.5f, iy),
                              ImVec2(px + (target_w + iw) * 0.5f, iy + ih),
                              IM_COL32(0, 0, 0, 255), ih * 0.5f);
        }
    }

    /* Interactive surface: click = tap, drag = swipe, wheel = scroll swipe */
    ImGui::SetCursorScreenPos(ImVec2(px, py));
    ImGui::InvisibleButton("##MirrorSurface", ImVec2(target_w, target_h));
    const bool hovered = ImGui::IsItemHovered();
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        g_focused_pane = FocusPane::Stage;
        g_drag_active = true;
        map_mirror_touch(ImGui::GetIO().MousePos, ImVec2(px, py), ImVec2(target_w, target_h), g_drag_fx0, g_drag_fy0);
        g_drag_t0_ms = now_ms();
    }
    if (g_drag_active && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        g_drag_active = false;
        float fx1 = 0.0f, fy1 = 0.0f;
        map_mirror_touch(ImGui::GetIO().MousePos, ImVec2(px, py), ImVec2(target_w, target_h), fx1, fy1);
        float dist_px = std::hypot((fx1 - g_drag_fx0) * target_w, (fy1 - g_drag_fy0) * target_h);
        int dur_ms = (int)std::clamp<uint64_t>(now_ms() - g_drag_t0_ms, 80, 1000);
        if (dist_px < 8.0f * s) {
            api_async("tap", {{"fx", g_drag_fx0}, {"fy", g_drag_fy0}});
        } else {
            api_async("swipe", {{"fx0", g_drag_fx0}, {"fy0", g_drag_fy0},
                                {"fx1", fx1}, {"fy1", fy1}, {"duration_ms", dur_ms}});
        }
    }
    if (hovered) {
        float wy = ImGui::GetIO().MouseWheel;
        if (std::fabs(wy) > 0.05f) {
            float fx = 0.5f, fy = 0.5f;
            map_mirror_touch(ImGui::GetIO().MousePos, ImVec2(px, py), ImVec2(target_w, target_h), fx, fy);
            float dy = (wy > 0.0f) ? 0.24f : -0.24f;
            float fy1 = std::clamp(fy + dy, 0.05f, 0.95f);
            api_async("swipe", {{"fx0", fx}, {"fy0", fy}, {"fx1", fx}, {"fy1", fy1}, {"duration_ms", 180}});
        }
    }
}

static void render_twin_stage(ImVec2 origin, ImVec2 size)
{
    const float s = g_scale;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilledMultiColor(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                                IM_COL32(58, 60, 70, 255), IM_COL32(58, 60, 70, 255),
                                IM_COL32(22, 22, 28, 255), IM_COL32(22, 22, 28, 255));

    if (g_twin_demo) {
        float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - g_twin_demo_start).count();
        float yaw = std::sin(elapsed * 0.7f) * 0.65f;
        float pitch = std::sin(elapsed * 0.45f) * 0.25f;
        rplayhub::Quat q{ std::sin(pitch * 0.5f), std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
        g_twin.setOrientation(q, true);
    }

    if (g_fold_only_view) {
        const auto now = std::chrono::steady_clock::now();
        float dt = g_fold_clock.time_since_epoch().count() == 0
                 ? (1.0f / 60.0f)
                 : std::clamp(std::chrono::duration<float>(now - g_fold_clock).count(), 0.001f, 0.1f);
        g_fold_clock = now;
        g_fold.tick(dt);
        g_twin.setFold(true, g_fold.angle(), true);
        if (!ImGui::GetIO().WantTextInput && !ImGui::GetIO().KeyCtrl) {
            if (ImGui::IsKeyPressed(ImGuiKey_1)) g_twin.setRenderMode(0);
            if (ImGui::IsKeyPressed(ImGuiKey_2)) g_twin.setRenderMode(1);
            if (ImGui::IsKeyPressed(ImGuiKey_3)) g_twin.setRenderMode(2);
        }
    } else {
        g_twin.setFold(false, 180.0f, false);
    }

    int dw = g_vtex_w > 0 ? g_vtex_w : 1170;
    int dh = g_vtex_h > 0 ? g_vtex_h : 2532;
    g_twin.render(dl, origin, size, (ImTextureID)(intptr_t)g_vtex, (ImTextureID)(intptr_t)g_back_texture,
                  dw, dh, g_rotation, s, 0, g_ren, 0);

    ImVec2 mouse = ImGui::GetMousePos();
    bool over = mouse.x >= origin.x && mouse.x <= origin.x + size.x &&
                mouse.y >= origin.y && mouse.y <= origin.y + size.y;
    int tx = 0, ty = 0;
    if (over && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        g_focused_pane = FocusPane::Stage;
        if (g_twin.hitTest(mouse, tx, ty) && dw > 0 && dh > 0) {
            api_async("tap", {{"fx", (float)tx / (float)dw}, {"fy", (float)ty / (float)dh}});
            g_orbit_dragging = false;
        } else if (!g_fold_only_view) {
            g_orbit_dragging = true;
        }
    } else if (g_orbit_dragging && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.5f)) {
        ImVec2 delta = ImGui::GetIO().MouseDelta;
        g_twin.addOrbit(delta.x * 0.0085f, delta.y * 0.0085f);
    } else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        g_orbit_dragging = false;
    }
}

static void render_screen_tab_bar(ImVec2 pos, float width)
{
    const float s = g_scale;
    if (g_screen_tabs.size() <= 1) return;
    ImDrawList *dl = ImGui::GetWindowDrawList();

    const float bar_h = 28.0f * s;
    const float add_w = 28.0f * s;
    const float gap   = 6.0f * s;
    const float pill_w = std::max(60.0f * s, width - add_w - gap);

    // Outer pill container (light gray capsule)
    dl->AddRectFilled(pos, ImVec2(pos.x + pill_w, pos.y + bar_h),
                      IM_COL32(234, 234, 238, 255), bar_h * 0.5f);

    int count = (int)g_screen_tabs.size();
    float seg_w = pill_w / std::max(1, count);
    ImVec2 mouse = ImGui::GetMousePos();

    for (int i = 0; i < count; i++) {
        ImVec2 seg_min(pos.x + i * seg_w, pos.y);
        ImVec2 seg_max(seg_min.x + seg_w, pos.y + bar_h);
        bool hovered = mouse.x >= seg_min.x && mouse.x < seg_max.x &&
                       mouse.y >= seg_min.y && mouse.y < seg_max.y;
        bool selected = (i == g_current_tab);

        if (selected) {
            // White capsule for active tab with subtle shadow
            dl->AddRectFilled(ImVec2(seg_min.x + 2.0f * s, seg_min.y + 2.0f * s),
                              ImVec2(seg_max.x - 2.0f * s, seg_max.y - 2.0f * s),
                              IM_COL32(255, 255, 255, 255), (bar_h - 4.0f * s) * 0.5f);
            dl->AddRect(ImVec2(seg_min.x + 2.0f * s, seg_min.y + 2.0f * s),
                        ImVec2(seg_max.x - 2.0f * s, seg_max.y - 2.0f * s),
                        IM_COL32(0, 0, 0, 20), (bar_h - 4.0f * s) * 0.5f, 0, 1.0f);
        } else if (hovered) {
            dl->AddRectFilled(ImVec2(seg_min.x + 2.0f * s, seg_min.y + 2.0f * s),
                              ImVec2(seg_max.x - 2.0f * s, seg_max.y - 2.0f * s),
                              IM_COL32(0, 0, 0, 12), (bar_h - 4.0f * s) * 0.5f);
        }

        // Close button 'x' on hover over active tab (when count > 1)
        float close_sz = 16.0f * s;
        bool over_close = false;
        if (count > 1 && hovered) {
            ImVec2 cpos(seg_max.x - close_sz - 6.0f * s, pos.y + (bar_h - close_sz) * 0.5f);
            over_close = mouse.x >= cpos.x && mouse.x < cpos.x + close_sz &&
                         mouse.y >= cpos.y && mouse.y < cpos.y + close_sz;
            if (over_close) {
                dl->AddCircleFilled(ImVec2(cpos.x + close_sz * 0.5f, cpos.y + close_sz * 0.5f),
                                    close_sz * 0.45f, IM_COL32(0, 0, 0, 30));
            }
            float k = close_sz * 0.28f;
            float mx = cpos.x + close_sz * 0.5f, my = cpos.y + close_sz * 0.5f;
            ImU32 xcol = over_close ? IM_COL32(30, 30, 30, 255) : IM_COL32(120, 120, 125, 255);
            dl->AddLine(ImVec2(mx - k, my - k), ImVec2(mx + k, my + k), xcol, 1.3f * s);
            dl->AddLine(ImVec2(mx - k, my + k), ImVec2(mx + k, my - k), xcol, 1.3f * s);
        }

        // Title text
        const char *tname = g_screen_tabs[i].title.c_str();
        ImVec2 tsz = g_font_medium ? g_font_medium->CalcTextSizeA(12.0f * s, FLT_MAX, 0.0f, tname)
                                   : ImGui::CalcTextSize(tname);
        float text_max_w = seg_w - (count > 1 ? close_sz + 12.0f * s : 12.0f * s);
        float tx = seg_min.x + (seg_w - (count > 1 ? close_sz * 0.5f : 0.0f) - std::min(tsz.x, text_max_w)) * 0.5f;
        float ty = pos.y + (bar_h - tsz.y) * 0.5f;
        ImU32 tcol = selected ? IM_COL32(28, 28, 30, 255) : IM_COL32(110, 110, 115, 255);
        if (g_font_medium) dl->AddText(g_font_medium, 12.0f * s, ImVec2(tx, ty), tcol, tname);
        else dl->AddText(ImVec2(tx, ty), tcol, tname);

        // Invisible button to handle clicks
        ImGui::SetCursorScreenPos(seg_min);
        char btn_id[32];
        snprintf(btn_id, sizeof btn_id, "##TabSeg%d", i);
        if (ImGui::InvisibleButton(btn_id, ImVec2(seg_w, bar_h))) {
            if (over_close) {
                close_screen_tab(i);
            } else {
                select_screen_tab(i);
            }
        }
    }

    // Add Tab '+' button at right
    ImVec2 add_pos(pos.x + pill_w + gap, pos.y);
    ImGui::SetCursorScreenPos(add_pos);
    if (rplayhub::IconButton("##AddScreenTab", rplayhub::Icons::drawPlus, ImVec2(add_w, bar_h), "Open Screen in New Tab")) {
        open_screen_tab();
    }
}

static void render_control_strip(ImVec2 pos, float width)
{
    const float s = g_scale;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddLine(pos, ImVec2(pos.x + width, pos.y), IM_COL32(230, 230, 235, 255), 1.0f);

    struct StripBtn {
        const char *id;
        void (*icon)(ImDrawList *, ImVec2, float, ImU32);
        const char *tip;
        std::function<void()> action;
    };
    StripBtn group1[] = {
        {"##StripPin",     rplayhub::Icons::drawPin,        g_pinned ? "Unpin Window" : "Pin Window on Top", []() {
            g_pinned = !g_pinned;
            SDL_SetWindowAlwaysOnTop(g_win, g_pinned ? SDL_TRUE : SDL_FALSE);
        }},
        {"##StripHome",    rplayhub::Icons::drawHome,       "Home Button",      []() { api_async("press_button", {{"button", "home"}}); }},
        {"##StripRotate",  rplayhub::Icons::drawRotate,     "Rotate View",      []() { g_rotation = (g_rotation + 1) & 3; }},
        {"##StripShot",    rplayhub::Icons::drawCamera,     "Take Screenshot",  []() { take_screenshot_action(); }},
        {"##StripRecord",  rplayhub::Icons::drawRecord,     "Record Screen",    []() { toggle_recording_action(); }},
    };
    StripBtn group2[] = {
        {"##StripSleep",   rplayhub::Icons::drawPower,      "Sleep / Wake",     []() { api_async("device_action", {{"action", "sleep"}}); }},
        {"##StripRestart", rplayhub::Icons::drawRefresh,    "Restart Device…",  []() { api_async("device_action", {{"action", "restart"}}); }},
        {"##StripShut",    rplayhub::Icons::drawDisconnect, "Shut Down Device…",[]() { g_open_shutdown_modal = true; }},
    };
    const int n1 = 5, n2 = 3;
    const float btn_w = 38.0f * s, btn_h = 34.0f * s;
    float spacing = 44.0f * s;
    const float gap = 16.0f * s; /* 16pt gap separating capture actions from power actions (matching ControlStrip.swift:64) */
    float total_w = (n1 - 1) * spacing + btn_w + gap + (n2 - 1) * spacing + btn_w;
    if (total_w > width - 16.0f * s) {
        spacing = std::max(btn_w, (width - 16.0f * s - btn_w - gap) / std::max(1, n1 + n2 - 2));
        total_w = (n1 - 1) * spacing + btn_w + gap + (n2 - 1) * spacing + btn_w;
    }
    float start_x = pos.x + (width - total_w) * 0.5f;

    // Group 1: Pin, Home, Rotate, Screenshot, Record
    for (int i = 0; i < n1; i++) {
        float bx = start_x + i * spacing;
        ImGui::SetCursorScreenPos(ImVec2(bx, pos.y + 6.0f * s));
        if (i == 0 && g_pinned) {
            if (rplayhub::FlatNavButton(group1[i].id, group1[i].icon, ImVec2(btn_w, btn_h), group1[i].tip, s, true)) {
                group1[i].action();
            }
        } else if (i == 4 && g_recording) {
            ImVec2 c(bx + btn_w * 0.5f, pos.y + 6.0f * s + btn_h * 0.5f);
            dl->AddCircleFilled(c, 6.0f * s, IM_COL32(255, 59, 48, 255));
            if (rplayhub::FlatNavButton(group1[i].id, [](ImDrawList *, ImVec2, float, ImU32) {},
                                        ImVec2(btn_w, btn_h), "Stop Recording", s)) {
                group1[i].action();
            }
        } else {
            if (rplayhub::FlatNavButton(group1[i].id, group1[i].icon, ImVec2(btn_w, btn_h), group1[i].tip, s)) {
                group1[i].action();
            }
        }
    }

    // Group 2: Sleep, Restart, Shutdown (with 16pt gap)
    float start_x2 = start_x + (n1 - 1) * spacing + btn_w + gap;
    for (int i = 0; i < n2; i++) {
        float bx = start_x2 + i * spacing;
        ImGui::SetCursorScreenPos(ImVec2(bx, pos.y + 6.0f * s));
        if (rplayhub::FlatNavButton(group2[i].id, group2[i].icon, ImVec2(btn_w, btn_h), group2[i].tip, s)) {
            group2[i].action();
        }
    }
}

static void render_center_stage(float start_x, float width, float height)
{
    const float s = g_scale;
    ImGui::SetNextWindowPos(ImVec2(start_x, g_menu_h));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, rplayhub::Theme::ColorBgStage);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    ImGui::Begin("##CenterStage", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings);
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) g_focused_pane = FocusPane::Stage;

    const float strip_h = 46.0f * s;
    const float stage_h = std::max(100.0f, height - strip_h);
    const bool popped_out = g_popout_win && !g_popout_win->closeRequested();
    const bool have_live = g_view_screen && g_vtex && g_vtex_w > 0 && !popped_out;

    const bool show_tabs = (g_screen_tabs.size() > 1 && !popped_out);
    const float tab_bar_h = show_tabs ? 34.0f * s : 0.0f;
    if (show_tabs) {
        float pill_w = std::min(width - 48.0f * s, (float)g_screen_tabs.size() * 160.0f * s + 36.0f * s);
        render_screen_tab_bar(ImVec2(start_x + (width - pill_w) * 0.5f, g_menu_h + 6.0f * s), pill_w);
    }

    float stage_y = g_menu_h + 6.0f * s + tab_bar_h;
    float content_h = stage_h - 12.0f * s - tab_bar_h;

    if (g_fold_only_view || (have_live && g_twin_mode)) {
        g_twin.setFoldOnly(g_fold_only_view || !g_twin_mode);
        render_twin_stage(ImVec2(start_x + 12.0f * s, stage_y),
                          ImVec2(width - 24.0f * s, content_h));
    } else if (have_live) {
        render_live_mirror(ImVec2(start_x + 12.0f * s, stage_y),
                           ImVec2(width - 24.0f * s, content_h));
    } else {
        render_phone_mockup(ImVec2(start_x + width * 0.5f, stage_y + content_h * 0.46f),
                            ImVec2(width - 24.0f * s, content_h),
                            popped_out);
    }

    render_control_strip(ImVec2(start_x, g_menu_h + height - strip_h), width);

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

/* =========================================================================
 * 13. Right Inspector — Group 0: Settings (app/rPlayHub/SettingsPanel.swift)
 * ========================================================================= */
static void render_settings_group(float width)
{
    if (!g_settings_loaded && !g_settings_loading && !g_bound_udid.empty()) {
        refresh_settings();
    }
    const float s = g_scale;
    const float pad = 16.0f * s;
    const float box_w = width - pad * 2.0f;
    const float row_h = 38.0f * s;
    ImDrawList *dl = ImGui::GetWindowDrawList();

    ImGui::SetCursorPos(ImVec2(pad, 14.0f * s));
    dl->AddText(g_font_semibold, 15.0f * s, ImGui::GetCursorScreenPos(),
                IM_COL32(28, 28, 30, 255), "Accessibility & Appearance");
    ImGui::SameLine(width - pad - 28.0f * s);
    if (rplayhub::IconButton("##RefSet", rplayhub::Icons::drawRefresh, ImVec2(28.0f * s, 26.0f * s), "Reload Settings")) {
        g_settings_loaded = false;
        refresh_settings();
    }
    ImGui::Dummy(ImVec2(0, 8.0f * s));

    /* Card 1: 9 rows matching SettingsPanel.swift */
    const int n_rows = 9;
    const float card1_h = n_rows * row_h + 8.0f * s;
    ImGui::SetCursorPosX(pad);
    ImVec2 c1 = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(c1, ImVec2(c1.x + box_w, c1.y + card1_h), IM_COL32(246, 246, 248, 255), 10.0f * s);
    dl->AddRect(c1, ImVec2(c1.x + box_w, c1.y + card1_h), IM_COL32(230, 230, 235, 255), 10.0f * s);

    auto row_label = [&](int idx, const char *label) -> float {
        float ry = c1.y + 4.0f * s + idx * row_h;
        if (idx > 0) {
            dl->AddLine(ImVec2(c1.x + 12.0f * s, ry), ImVec2(c1.x + box_w - 12.0f * s, ry),
                        IM_COL32(228, 228, 232, 255), 1.0f);
        }
        dl->AddText(g_font_regular, g_font_regular->FontSize,
                    ImVec2(c1.x + 12.0f * s, ry + (row_h - g_font_regular->FontSize) * 0.5f),
                    IM_COL32(28, 28, 30, 255), label);
        return ry;
    };

    const float ctrl_w = 136.0f * s;
    const float ctrl_x = c1.x + box_w - ctrl_w - 10.0f * s;

    /* Row 0: Appearance (Light / Dark) */
    {
        float ry = row_label(0, "Appearance");
        ImGui::SetCursorScreenPos(ImVec2(ctrl_x, ry + 5.0f * s));
        ImGui::SetNextItemWidth(ctrl_w);
        const char *items[] = {"Light", "Dark"};
        if (ImGui::Combo("##Appearance", &g_set_appearance, items, 2)) {
            api_async("set_setting", {{"key", "appearance"}, {"value", g_set_appearance == 1 ? "dark" : "light"}});
        }
    }
    /* Row 1: Liquid Glass (0..1) */
    {
        float ry = row_label(1, "Liquid Glass");
        ImGui::SetCursorScreenPos(ImVec2(ctrl_x, ry + 6.0f * s));
        ImGui::SetNextItemWidth(ctrl_w);
        if (ImGui::SliderFloat("##LiquidGlass", &g_set_liquid_glass, 0.0f, 1.0f, "%.2f")) {
            /* Send on release so dragging doesn't flood XPC */
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            api_async("set_setting", {{"key", "liquidGlass"}, {"value", g_set_liquid_glass}});
        }
    }
    /* Row 2: Color Filter (None / On) */
    {
        float ry = row_label(2, "Color Filter");
        ImGui::SetCursorScreenPos(ImVec2(ctrl_x, ry + 5.0f * s));
        ImGui::SetNextItemWidth(ctrl_w);
        const char *items[] = {"None", "On"};
        if (ImGui::Combo("##ColorFilter", &g_set_color_filter, items, 2)) {
            api_async("set_setting", {{"key", "colorFilter"}, {"value", g_set_color_filter}});
        }
    }
    /* Row 3: Text Size (7-step slider) */
    {
        float ry = row_label(3, "Text Size");
        ImGui::SetCursorScreenPos(ImVec2(ctrl_x, ry + 6.0f * s));
        ImGui::SetNextItemWidth(ctrl_w);
        if (ImGui::SliderInt("##TextSize", &g_set_text_size_idx, 0, 6, kTextSizeLabels[std::clamp(g_set_text_size_idx, 0, 6)])) {
            api_async("set_setting", {{"key", "textSize"}, {"value", kTextSizes[std::clamp(g_set_text_size_idx, 0, 6)]}});
        }
    }
    /* Rows 4..8: macOS-style ToggleSwitchBare */
    auto toggle_row = [&](int idx, const char *label, const char *id, const char *key, bool &val) {
        float ry = row_label(idx, label);
        ImGui::SetCursorScreenPos(ImVec2(c1.x + box_w - 50.0f * s, ry + (row_h - 22.0f * s) * 0.5f));
        if (rplayhub::ToggleSwitchBare(id, &val, s)) {
            api_async("set_setting", {{"key", key}, {"value", val}});
        }
    };
    toggle_row(4, "Reduce Motion",       "##RedMotion",   "reduceMotion",       g_set_reduce_motion);
    toggle_row(5, "Increase Contrast",   "##IncContrast", "increaseContrast",   g_set_increase_contrast);
    toggle_row(6, "Show Borders",        "##ShowBorders", "showBorders",        g_set_show_borders);
    toggle_row(7, "Reduce Transparency", "##RedTransp",   "reduceTransparency", g_set_reduce_transp);
    toggle_row(8, "VoiceOver",           "##VoiceOver",   "voiceOver",          g_set_voiceover);

    ImGui::SetCursorScreenPos(ImVec2(pad, c1.y + card1_h + 18.0f * s));
    dl->AddText(g_font_semibold, 15.0f * s, ImGui::GetCursorScreenPos(),
                IM_COL32(28, 28, 30, 255), "Simulated Location");
    ImGui::Dummy(ImVec2(0, 24.0f * s));

    /* Card 2: Location row matching SettingsPanel.swift */
    ImGui::SetCursorPosX(pad);
    ImVec2 c2 = ImGui::GetCursorScreenPos();
    float card2_h = row_h + 8.0f * s;
    dl->AddRectFilled(c2, ImVec2(c2.x + box_w, c2.y + card2_h), IM_COL32(246, 246, 248, 255), 10.0f * s);
    dl->AddRect(c2, ImVec2(c2.x + box_w, c2.y + card2_h), IM_COL32(230, 230, 235, 255), 10.0f * s);
    dl->AddText(g_font_regular, g_font_regular->FontSize,
                ImVec2(c2.x + 12.0f * s, c2.y + 4.0f * s + (row_h - g_font_regular->FontSize) * 0.5f),
                IM_COL32(28, 28, 30, 255), "Location");

    std::string loc_preview = "None";
    const int n_presets = (int)(sizeof(kLocationPresets) / sizeof(kLocationPresets[0]));
    if (g_set_location_idx >= 1 && g_set_location_idx <= n_presets) {
        loc_preview = kLocationPresets[g_set_location_idx - 1].name;
    } else if (g_set_location_idx == n_presets + 1 && !g_custom_loc_label.empty()) {
        loc_preview = g_custom_loc_label;
    }

    ImGui::SetCursorScreenPos(ImVec2(c2.x + box_w - 156.0f * s, c2.y + 9.0f * s));
    ImGui::SetNextItemWidth(146.0f * s);
    if (ImGui::BeginCombo("##LocationCombo", loc_preview.c_str())) {
        if (ImGui::Selectable("None", g_set_location_idx == 0)) {
            g_set_location_idx = 0;
            api_async("clear_location", {}, [](const json &r) {
                if (r.value("ok", false)) show_toast("Cleared simulated location");
            });
        }
        ImGui::Separator();
        for (int i = 0; i < n_presets; i++) {
            bool sel = (g_set_location_idx == i + 1);
            if (ImGui::Selectable(kLocationPresets[i].name, sel)) {
                g_set_location_idx = i + 1;
                double lat = kLocationPresets[i].lat, lon = kLocationPresets[i].lon;
                std::string city = kLocationPresets[i].name;
                api_async("set_location", {{"latitude", lat}, {"longitude", lon}}, [city](const json &r) {
                    if (r.value("ok", false)) show_toast("Location set to " + city);
                    else show_toast("Location failed: " + json_err_str(r, "unknown"), 6000);
                });
            }
        }
        ImGui::Separator();
        ImGui::BeginDisabled();
        ImGui::Selectable("Trips (GPX playback not yet wired)", false);
        ImGui::Selectable("  Freeway Drive", false);
        ImGui::Selectable("  City Bicycle Ride", false);
        ImGui::Selectable("  City Run", false);
        ImGui::EndDisabled();
        ImGui::Separator();
        if (ImGui::Selectable("Custom Coordinates…", g_set_location_idx == n_presets + 1)) {
            g_open_custom_loc_modal = true;
        }
        ImGui::EndCombo();
    }
    ImGui::SetCursorScreenPos(ImVec2(pad, c2.y + card2_h + 12.0f * s));
}

/* =========================================================================
 * 14. Right Inspector — Group 1: Report (app/rPlayHub/ReportPanel.swift)
 * ========================================================================= */
static void render_report_group(float width, float height)
{
    if (!g_reports_loaded && !g_reports_loading && !g_bound_udid.empty()) {
        refresh_reports();
    }
    const float s = g_scale;
    ImDrawList *dl = ImGui::GetWindowDrawList();

    /* Header row */
    ImGui::SetCursorPos(ImVec2(14.0f * s, 12.0f * s));
    dl->AddText(g_font_semibold, 15.0f * s, ImGui::GetCursorScreenPos(),
                IM_COL32(28, 28, 30, 255), "Device Reports");
    ImGui::SameLine(width - 126.0f * s);
    if (ImGui::Button("Export All", ImVec2(82.0f * s, 26.0f * s))) {
        std::string dir = ensure_media_dir("Downloads") + "/crashes";
        mkdir(dir.c_str(), 0755);
        show_toast("Exporting crash reports…", 10000);
        api_async("export_crashes", {{"dir", dir}}, [dir](const json &r) {
            if (r.value("ok", false)) {
                int n = r["result"].value("exported", 0);
                show_toast("Exported " + std::to_string(n) + " reports to " + dir, 5000);
            } else {
                show_toast("Export failed: " + json_err_str(r, "unknown"), 6000);
            }
        }, 60);
    }
    ImGui::SameLine(width - 38.0f * s);
    if (rplayhub::IconButton("##RefRep", rplayhub::Icons::drawRefresh, ImVec2(28.0f * s, 26.0f * s), "Refresh Reports")) {
        g_reports_loaded = false;
        refresh_reports();
    }

    const float bottom_bar_h = 46.0f * s;
    const float list_top = 42.0f * s;
    const float list_h = std::max(80.0f, height - list_top - bottom_bar_h);

    ImGui::SetCursorPos(ImVec2(0, list_top));
    ImGui::BeginChild("##ReportList", ImVec2(width, list_h), false);

    /* Filter reports by category + search string */
    std::vector<const FileRow *> visible;
    for (const auto &r : g_reports) {
        if (!report_matches_category(r.name, g_report_category)) continue;
        std::string proc = process_name_from_filename(r.name);
        if (g_report_filter[0]) {
            bool m1 = strcasestr(proc.c_str(), g_report_filter) != nullptr;
            bool m2 = strcasestr(r.name.c_str(), g_report_filter) != nullptr;
            if (!m1 && !m2) continue;
        }
        visible.push_back(&r);
    }

    static const char *const kCatNames[4] = {"Crashes", "Spins", "Logs", "Diagnostics"};
    if (g_reports_loading) {
        ImGui::SetCursorPos(ImVec2(16.0f * s, 20.0f * s));
        ImGui::TextDisabled("Loading device reports…");
    } else if (visible.empty()) {
        std::string empty_msg = std::string("No ") + kCatNames[g_report_category];
        ImVec2 tsz = g_font_medium->CalcTextSizeA(15.0f * s, FLT_MAX, 0.0f, empty_msg.c_str());
        ImVec2 wp = ImGui::GetWindowPos();
        dl->AddText(g_font_medium, 15.0f * s,
                    ImVec2(wp.x + (width - tsz.x) * 0.5f, wp.y + list_h * 0.42f),
                    IM_COL32(142, 142, 147, 255), empty_msg.c_str());
    } else {
        const float row_h = 48.0f * s;
        for (size_t i = 0; i < visible.size(); i++) {
            const FileRow &rep = *visible[i];
            ImGui::PushID((int)i);
            ImVec2 p = ImGui::GetCursorScreenPos();
            bool clicked = ImGui::InvisibleButton("##reprow", ImVec2(width, row_h));
            bool dbl     = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
            bool hovered = ImGui::IsItemHovered();
            if (hovered) {
                dl->AddRectFilled(ImVec2(p.x + 6.0f * s, p.y + 2.0f * s),
                                  ImVec2(p.x + width - 6.0f * s, p.y + row_h - 2.0f * s),
                                  IM_COL32(0, 0, 0, 12), 6.0f * s);
            }
            rplayhub::Icons::drawFile(dl, ImVec2(p.x + 14.0f * s, p.y + (row_h - 20.0f * s) * 0.5f),
                                      20.0f * s, IM_COL32(110, 110, 118, 255));
            std::string proc = process_name_from_filename(rep.name);
            std::string sub  = format_relative_time(rep.mtime);
            if (!sub.empty()) sub += " \xc2\xb7 ";
            sub += format_bytes(rep.size);

            float tx = p.x + 42.0f * s;
            ImVec4 clip(tx, p.y, p.x + width - 12.0f * s, p.y + row_h);
            dl->AddText(g_font_medium, g_font_medium->FontSize, ImVec2(tx, p.y + 6.0f * s),
                        IM_COL32(28, 28, 30, 255), proc.c_str(), nullptr, 0.0f, &clip);
            dl->AddText(g_font_caption, 12.5f * s, ImVec2(tx, p.y + 26.0f * s),
                        IM_COL32(130, 130, 135, 255), sub.c_str(), nullptr, 0.0f, &clip);
            if (hovered) ImGui::SetTooltip("%s\nDouble-click to save & open", rep.full_path.c_str());
            if (dbl || (clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))) {
                save_remote_file("crash", rep.full_path, rep.name, true);
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    /* Bottom bar: Filter input + Category dropdown (matching ReportPanel.swift & gui_app.cc) */
    ImVec2 wp = ImGui::GetWindowPos();
    float by = wp.y + height - bottom_bar_h;
    dl->AddLine(ImVec2(wp.x, by), ImVec2(wp.x + width, by), IM_COL32(224, 224, 229, 255), 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(wp.x + 12.0f * s, by + 8.0f * s));
    ImVec2 fpos = ImGui::GetCursorScreenPos();
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 14.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(30.0f * s, 6.0f * s));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.95f, 0.95f, 0.96f, 1.0f));
    ImGui::SetNextItemWidth(width - 144.0f * s);
    ImGui::InputTextWithHint("##RepFilter", "Filter", g_report_filter, sizeof g_report_filter);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    rplayhub::Icons::drawSearch(dl, ImVec2(fpos.x + 10.0f * s, fpos.y + 6.5f * s),
                                15.0f * s, IM_COL32(142, 142, 147, 255));
    ImGui::SameLine(0, 6.0f * s);
    ImGui::SetNextItemWidth(114.0f * s);
    ImGui::Combo("##RepCat", &g_report_category, kCatNames, 4);
}

/* =========================================================================
 * 15. Right Inspector — Group 2: Info (Sub-tabs: Info | Apps | Profiles | Files | Console)
 * ========================================================================= */
static void draw_section_card_ui(float width, float pad, float s,
                                 const char *title, const std::vector<std::pair<std::string, std::string>> &rows)
{
    if (rows.empty()) return;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImGui::SetCursorPosX(pad);
    dl->AddText(g_font_semibold, 15.0f * s, ImGui::GetCursorScreenPos(),
                IM_COL32(28, 28, 30, 255), title);
    ImGui::Dummy(ImVec2(0, 22.0f * s));

    const float row_h = 34.0f * s;
    const float box_w = width - pad * 2.0f;
    const float box_h = rows.size() * row_h + 6.0f * s;
    ImGui::SetCursorPosX(pad);
    ImVec2 c = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(c, ImVec2(c.x + box_w, c.y + box_h), IM_COL32(246, 246, 248, 255), 10.0f * s);
    dl->AddRect(c, ImVec2(c.x + box_w, c.y + box_h), IM_COL32(230, 230, 235, 255), 10.0f * s);

    for (size_t i = 0; i < rows.size(); i++) {
        float ry = c.y + 3.0f * s + i * row_h;
        if (i > 0) {
            dl->AddLine(ImVec2(c.x + 12.0f * s, ry), ImVec2(c.x + box_w - 12.0f * s, ry),
                        IM_COL32(228, 228, 232, 255), 1.0f);
        }
        dl->AddText(g_font_regular, 14.0f * s, ImVec2(c.x + 12.0f * s, ry + (row_h - 15.0f * s) * 0.5f),
                    IM_COL32(110, 110, 115, 255), rows[i].first.c_str());
        ImVec2 vsz = g_font_medium->CalcTextSizeA(14.0f * s, FLT_MAX, 0.0f, rows[i].second.c_str());
        float vx = std::max(c.x + 115.0f * s, c.x + box_w - vsz.x - 12.0f * s);
        ImVec4 clip(c.x + 110.0f * s, ry, c.x + box_w - 8.0f * s, ry + row_h);
        dl->AddText(g_font_medium, 14.0f * s, ImVec2(vx, ry + (row_h - 15.0f * s) * 0.5f),
                    IM_COL32(28, 28, 30, 255), rows[i].second.c_str(), nullptr, 0.0f, &clip);
    }
    ImGui::SetCursorScreenPos(ImVec2(pad, c.y + box_h + 14.0f * s));
}

static void render_info_subtab_device(float width, float avail_h)
{
    if (!g_device_info_loaded && !g_device_info_loading && !g_bound_udid.empty()) {
        refresh_device_info();
    }
    const float s = g_scale;
    const float pad = 14.0f * s;

    ImGui::BeginChild("##InfoScroll", ImVec2(width, avail_h), false);

    auto draw_section_card = [&](const char *title, const std::vector<std::pair<std::string, std::string>> &rows) {
        draw_section_card_ui(width, pad, s, title, rows);
    };

    auto val_str = [](const json &obj, const char *key) -> std::string {
        if (!obj.is_object() || !obj.contains(key)) return "";
        const auto &v = obj[key];
        if (v.is_string()) return v.get<std::string>();
        if (v.is_boolean()) return v.get<bool>() ? "Yes" : "No";
        if (v.is_number_integer()) return std::to_string(v.get<int64_t>());
        if (v.is_number()) return std::to_string(v.get<double>());
        return "";
    };

    if (g_device_info.is_object()) {
        const json def = g_device_info.value("default", json::object());
        std::vector<std::pair<std::string, std::string>> dev_rows;
        auto add_if = [&](const char *label, const std::string &v) {
            if (!v.empty()) dev_rows.push_back({label, v});
        };
        add_if("Name",         val_str(def, "DeviceName"));
        std::string pt = val_str(def, "ProductType");
        if (const auto *m = lookup_device_model(pt)) add_if("Model", std::string(m->marketing_name) + " (" + pt + ")");
        else add_if("Model", pt);
        add_if("Model Number", val_str(def, "ModelNumber"));
        add_if("iOS Version",  val_str(def, "ProductVersion"));
        add_if("Build",        val_str(def, "BuildVersion"));
        add_if("CPU",          val_str(def, "CPUArchitecture"));
        add_if("Hardware",     val_str(def, "HardwareModel"));
        add_if("Serial",       val_str(def, "SerialNumber"));
        add_if("UDID",         val_str(def, "UniqueDeviceID"));
        add_if("Wi-Fi MAC",    val_str(def, "WiFiAddress"));
        draw_section_card("Device", dev_rows);

        const json bat = g_device_info.value("com.apple.mobile.battery", json::object());
        if (bat.is_object() && !bat.empty()) {
            std::vector<std::pair<std::string, std::string>> bat_rows;
            std::string cap = val_str(bat, "BatteryCurrentCapacity");
            if (!cap.empty()) bat_rows.push_back({"Level", cap + "%"});
            auto add_bat_if = [&](const char *l, const std::string &v) { if (!v.empty()) bat_rows.push_back({l, v}); };
            add_bat_if("Charging",       val_str(bat, "BatteryIsCharging"));
            add_bat_if("External Power", val_str(bat, "ExternalConnected"));
            add_bat_if("Fully Charged",  val_str(bat, "FullyCharged"));
            draw_section_card("Battery", bat_rows);
        }

        const json dsk = g_device_info.value("com.apple.disk_usage", json::object());
        if (dsk.is_object() && !dsk.empty()) {
            std::vector<std::pair<std::string, std::string>> dsk_rows;
            auto add_gb = [&](const char *label, const char *key) {
                if (dsk.contains(key) && dsk[key].is_number()) {
                    dsk_rows.push_back({label, format_bytes(dsk[key].get<int64_t>())});
                }
            };
            add_gb("Total Disk",     "TotalDiskCapacity");
            add_gb("Data Capacity",  "TotalDataCapacity");
            add_gb("Data Available", "TotalDataAvailable");
            add_gb("System Size",    "TotalSystemCapacity");
            draw_section_card("Storage", dsk_rows);
        }
    }

    /* Live Stream section from stream_info */
    std::vector<std::pair<std::string, std::string>> stream_rows;
    stream_rows.push_back({"Video Stream", g_stream_connected ? (g_h264 ? "Connected (H.264)" : "Connected (HEVC)") : "Disconnected"});
    if (g_vtex_w > 0 && g_vtex_h > 0) {
        stream_rows.push_back({"Active Picture", std::to_string(g_vtex_w) + "\xc3\x97" + std::to_string(g_vtex_h)});
    }
    stream_rows.push_back({"Frames Decoded", std::to_string(g_frames_decoded.load())});
    float cur_fps = g_live_fps.load();
    if (cur_fps > 0.0f) {
        char fbuf[32];
        snprintf(fbuf, sizeof fbuf, "%.1f FPS", cur_fps);
        stream_rows.push_back({"Framerate", fbuf});
    }
    if (g_stream_info.is_object() && !g_stream_info.empty()) {
        char mbuf[32];
        snprintf(mbuf, sizeof mbuf, "%.2f Mbps", g_stream_info.value("mbps", 0.0));
        stream_rows.push_back({"Bitrate", mbuf});
        stream_rows.push_back({"RTP Packets", std::to_string(g_stream_info.value("rtp_packets", (uint64_t)0))});
        stream_rows.push_back({"Uptime", std::to_string(g_stream_info.value("uptime_s", 0)) + " s"});
        if (g_stream_info.contains("audio") && g_stream_info["audio"].is_object()) {
            const auto &au = g_stream_info["audio"];
            stream_rows.push_back({"Audio Frames", std::to_string(au.value("frames", (uint64_t)0))});
        }
    }
    draw_section_card("Stream Diagnostics", stream_rows);

    ImGui::EndChild();
}

static void render_info_subtab_apps(float width, float avail_h)
{
    if (!g_apps_loaded && !g_apps_loading && !g_bound_udid.empty()) {
        refresh_apps();
    }
    const float s = g_scale;
    ImDrawList *dl = ImGui::GetWindowDrawList();

    /* Action header: count + Install IPA (+) + Refresh */
    ImGui::SetCursorPos(ImVec2(14.0f * s, ImGui::GetCursorPosY() + 4.0f * s));
    ImGui::TextDisabled("%zu apps", g_apps.size());
    ImGui::SameLine(width - 70.0f * s);
    if (rplayhub::IconButton("##AddIpa", rplayhub::Icons::drawPlus, ImVec2(28.0f * s, 26.0f * s), "Install .ipa…")) {
        pick_file_async("Install IPA", "iOS App (*.ipa) | *.ipa", "*.ipa", [](const std::string &picked) {
            if (picked == "!nodialog") {
                g_open_install_ipa_modal = true;
            } else if (!picked.empty()) {
                install_ipa_path(picked);
            }
        });
    }
    ImGui::SameLine(width - 38.0f * s);
    if (rplayhub::IconButton("##RefApps", rplayhub::Icons::drawRefresh, ImVec2(28.0f * s, 26.0f * s), "Refresh Apps")) {
        g_apps_loaded = false;
        refresh_apps();
    }

    const float bottom_bar_h = 46.0f * s;
    const float list_h = std::max(80.0f, avail_h - 32.0f * s - bottom_bar_h);

    ImGui::BeginChild("##AppsList", ImVec2(width, list_h), false);

    /* Filter apps by category + search string (matching AppsPanel.swift) */
    std::vector<AppRow *> visible;
    for (auto &a : g_apps) {
        if (g_app_category == 1 && !a.app_clip) continue;      /* App Clips */
        if (g_app_category == 2 && !a.first_party) continue;   /* Default */
        if (g_app_category == 3 && !a.developer) continue;     /* Developer */
        if (g_app_filter[0]) {
            bool m1 = strcasestr(a.name.c_str(), g_app_filter) != nullptr;
            bool m2 = strcasestr(a.bundle_id.c_str(), g_app_filter) != nullptr;
            if (!m1 && !m2) continue;
        }
        visible.push_back(&a);
    }

    static const char *const kAppCats[4] = {"All Apps", "App Clips", "Default", "Developer"};
    if (g_apps_loading) {
        ImGui::SetCursorPos(ImVec2(16.0f * s, 20.0f * s));
        ImGui::TextDisabled("Loading installed apps…");
    } else if (visible.empty()) {
        std::string empty_msg = std::string("No ") + kAppCats[g_app_category];
        ImVec2 tsz = g_font_medium->CalcTextSizeA(15.0f * s, FLT_MAX, 0.0f, empty_msg.c_str());
        ImVec2 wp = ImGui::GetWindowPos();
        dl->AddText(g_font_medium, 15.0f * s,
                    ImVec2(wp.x + (width - tsz.x) * 0.5f, wp.y + list_h * 0.42f),
                    IM_COL32(142, 142, 147, 255), empty_msg.c_str());
    } else {
        const float row_h = 50.0f * s;
        for (size_t i = 0; i < visible.size(); i++) {
            AppRow &app = *visible[i];
            ImGui::PushID(app.bundle_id.c_str());
            ImVec2 p = ImGui::GetCursorScreenPos();
            bool clicked = ImGui::InvisibleButton("##approw", ImVec2(width, row_h));
            bool dbl     = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
            bool hovered = ImGui::IsItemHovered();

            /* Lazily fetch PNG app icon when visible on screen */
            if (ImGui::IsItemVisible() && !app.icon_requested) {
                request_app_icon(app);
            }

            if (hovered) {
                dl->AddRectFilled(ImVec2(p.x + 6.0f * s, p.y + 2.0f * s),
                                  ImVec2(p.x + width - 6.0f * s, p.y + row_h - 2.0f * s),
                                  IM_COL32(0, 0, 0, 12), 6.0f * s);
            }

            /* 34pt rounded app icon (real PNG texture or colorful drawAppBadge fallback) */
            float icon_sz = 34.0f * s;
            ImVec2 ip(p.x + 12.0f * s, p.y + (row_h - icon_sz) * 0.5f);
            if (app.icon_tex) {
                dl->AddImageRounded((ImTextureID)(intptr_t)app.icon_tex, ip,
                                    ImVec2(ip.x + icon_sz, ip.y + icon_sz),
                                    ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, icon_sz * 0.22f);
            } else {
                rplayhub::Icons::drawAppBadge(dl, ip, icon_sz, app.name, app.bundle_id, g_font_bold);
            }

            int pid = 0;
            auto pit = g_running_pids.find(app.name);
            if (pit != g_running_pids.end()) pid = pit->second;

            float tx = p.x + 54.0f * s;
            ImVec4 clip(tx, p.y, p.x + width - (pid > 0 ? 62.0f * s : 12.0f * s), p.y + row_h);
            dl->AddText(g_font_medium, g_font_medium->FontSize, ImVec2(tx, p.y + 6.0f * s),
                        IM_COL32(28, 28, 30, 255), app.name.c_str(), nullptr, 0.0f, &clip);
            std::string sub = app.bundle_id;
            if (!app.version.empty()) sub += " \xc2\xb7 " + app.version;
            dl->AddText(g_font_caption, 12.5f * s, ImVec2(tx, p.y + 26.0f * s),
                        IM_COL32(130, 130, 135, 255), sub.c_str(), nullptr, 0.0f, &clip);

            if (pid > 0) {
                ImVec2 bmin(p.x + width - 56.0f * s, p.y + (row_h - 20.0f * s) * 0.5f);
                dl->AddRectFilled(bmin, ImVec2(bmin.x + 46.0f * s, bmin.y + 20.0f * s),
                                  IM_COL32(52, 199, 89, 38), 10.0f * s);
                dl->AddText(g_font_caption, 11.5f * s, ImVec2(bmin.x + 8.0f * s, bmin.y + 3.0f * s),
                            IM_COL32(36, 150, 65, 255), "Active");
            }

            if (dbl || (clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))) {
                std::string bid = app.bundle_id, aname = app.name;
                api_async("launch_app", {{"bundle_id", bid}}, [aname](const json &r) {
                    if (r.value("ok", false)) {
                        show_toast("Launched " + aname);
                        refresh_processes();
                    } else {
                        show_toast("Launch failed: " + json_err_str(r, "unknown"), 6000);
                    }
                });
            }

            if (ImGui::BeginPopupContextItem("##AppCtx")) {
                if (ImGui::MenuItem("Launch")) {
                    std::string bid = app.bundle_id, aname = app.name;
                    api_async("launch_app", {{"bundle_id", bid}}, [aname](const json &r) {
                        if (r.value("ok", false)) { show_toast("Launched " + aname); refresh_processes(); }
                    });
                }
                if (pid > 0 && ImGui::MenuItem("Terminate")) {
                    std::string aname = app.name;
                    api_async("terminate_app", {{"pid", pid}}, [aname](const json &r) {
                        if (r.value("ok", false)) { show_toast("Terminated " + aname); refresh_processes(); }
                    });
                }
                if (ImGui::MenuItem("Copy Bundle Identifier")) {
                    SDL_SetClipboardText(app.bundle_id.c_str());
                    show_toast("Copied " + app.bundle_id);
                }
                if (!app.first_party) {
                    ImGui::Separator();
                    if (ImGui::MenuItem("Uninstall…")) {
                        g_confirm_uninstall_bundle = app.bundle_id;
                        g_confirm_uninstall_name   = app.name;
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    /* Bottom bar: Filter input + Category dropdown (All Apps | App Clips | Default | Developer) */
    ImVec2 wp = ImGui::GetWindowPos();
    float by = wp.y + ImGui::GetWindowHeight() - bottom_bar_h;
    dl->AddLine(ImVec2(wp.x, by), ImVec2(wp.x + width, by), IM_COL32(224, 224, 229, 255), 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(wp.x + 12.0f * s, by + 8.0f * s));
    ImVec2 fpos = ImGui::GetCursorScreenPos();
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 14.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(30.0f * s, 6.0f * s));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.95f, 0.95f, 0.96f, 1.0f));
    ImGui::SetNextItemWidth(width - 140.0f * s);
    ImGui::InputTextWithHint("##AppFilter", "Filter", g_app_filter, sizeof g_app_filter);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    rplayhub::Icons::drawSearch(dl, ImVec2(fpos.x + 10.0f * s, fpos.y + 6.5f * s),
                                15.0f * s, IM_COL32(142, 142, 147, 255));
    ImGui::SameLine(0, 6.0f * s);
    ImGui::SetNextItemWidth(110.0f * s);
    ImGui::Combo("##AppCat", &g_app_category, kAppCats, 4);
}

static void render_info_subtab_profiles(float width, float avail_h)
{
    if (!g_profiles_loaded && !g_profiles_loading && !g_bound_udid.empty()) {
        refresh_profiles();
    }
    const float s = g_scale;
    const float bottom_bar_h = 42.0f * s;
    const float list_h = std::max(80.0f, avail_h - bottom_bar_h);
    ImDrawList *dl = ImGui::GetWindowDrawList();

    ImGui::BeginChild("##ProfilesList", ImVec2(width, list_h), false);
    const float pad = 14.0f * s;

    ImGui::SetCursorPos(ImVec2(pad, 8.0f * s));
    dl->AddText(g_font_semibold, 15.0f * s, ImGui::GetCursorScreenPos(),
                IM_COL32(28, 28, 30, 255), "Provisioning Profiles");
    ImGui::Dummy(ImVec2(0, 22.0f * s));

    if (g_prov_profiles.empty()) {
        ImGui::SetCursorPosX(pad);
        ImGui::TextDisabled("No provisioning profiles installed.");
    } else {
        const float row_h = 48.0f * s;
        for (int i = 0; i < (int)g_prov_profiles.size(); i++) {
            const auto &pr = g_prov_profiles[i];
            ImGui::PushID(pr.uuid.c_str());
            ImVec2 p = ImGui::GetCursorScreenPos();
            bool clicked = ImGui::InvisibleButton("##prov", ImVec2(width, row_h));
            bool sel = (g_selected_prov_idx == i);
            if (clicked) { g_selected_prov_idx = i; g_selected_cfg_idx = -1; }
            if (sel) {
                dl->AddRectFilled(ImVec2(p.x + 8.0f * s, p.y + 1.0f * s),
                                  ImVec2(p.x + width - 8.0f * s, p.y + row_h - 1.0f * s),
                                  IM_COL32(0, 122, 255, 35), 6.0f * s);
            }
            dl->AddText(g_font_medium, g_font_medium->FontSize, ImVec2(p.x + 14.0f * s, p.y + 5.0f * s),
                        IM_COL32(28, 28, 30, 255), pr.name.c_str());
            std::string sub = pr.team;
            if (!pr.expiration.empty()) sub += " \xc2\xb7 Exp " + pr.expiration.substr(0, 10);
            dl->AddText(g_font_caption, 12.5f * s, ImVec2(p.x + 14.0f * s, p.y + 25.0f * s),
                        IM_COL32(130, 130, 135, 255), sub.c_str());
            ImGui::PopID();
        }
    }

    ImGui::Dummy(ImVec2(0, 12.0f * s));
    ImGui::SetCursorPosX(pad);
    dl->AddText(g_font_semibold, 15.0f * s, ImGui::GetCursorScreenPos(),
                IM_COL32(28, 28, 30, 255), "Configuration Profiles (MDM / Certs)");
    ImGui::Dummy(ImVec2(0, 22.0f * s));

    if (g_cfg_profiles.empty()) {
        ImGui::SetCursorPosX(pad);
        ImGui::TextDisabled("No configuration profiles installed.");
    } else {
        const float row_h = 48.0f * s;
        for (int i = 0; i < (int)g_cfg_profiles.size(); i++) {
            const auto &cp = g_cfg_profiles[i];
            ImGui::PushID(cp.identifier.c_str());
            ImVec2 p = ImGui::GetCursorScreenPos();
            bool clicked = ImGui::InvisibleButton("##cfg", ImVec2(width, row_h));
            bool sel = (g_selected_cfg_idx == i);
            if (clicked) { g_selected_cfg_idx = i; g_selected_prov_idx = -1; }
            if (sel) {
                dl->AddRectFilled(ImVec2(p.x + 8.0f * s, p.y + 1.0f * s),
                                  ImVec2(p.x + width - 8.0f * s, p.y + row_h - 1.0f * s),
                                  IM_COL32(0, 122, 255, 35), 6.0f * s);
            }
            dl->AddText(g_font_medium, g_font_medium->FontSize, ImVec2(p.x + 14.0f * s, p.y + 5.0f * s),
                        IM_COL32(28, 28, 30, 255), cp.display_name.c_str());
            std::string sub = cp.identifier;
            if (!cp.organization.empty()) sub = cp.organization + " \xc2\xb7 " + sub;
            dl->AddText(g_font_caption, 12.5f * s, ImVec2(p.x + 14.0f * s, p.y + 25.0f * s),
                        IM_COL32(130, 130, 135, 255), sub.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    /* Bottom action bar: + (install), - (remove selected), Refresh */
    ImVec2 wp = ImGui::GetWindowPos();
    float by = wp.y + ImGui::GetWindowHeight() - bottom_bar_h;
    dl->AddLine(ImVec2(wp.x, by), ImVec2(wp.x + width, by), IM_COL32(224, 224, 229, 255), 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(wp.x + 10.0f * s, by + 8.0f * s));
    if (rplayhub::IconButton("##AddProf", rplayhub::Icons::drawPlus, ImVec2(28.0f * s, 26.0f * s),
                             "Install .mobileprovision or .mobileconfig…")) {
        pick_file_async("Install Profile", "Profiles | *.mobileprovision *.mobileconfig",
                        "*.mobileprovision *.mobileconfig", [](const std::string &picked) {
            if (picked == "!nodialog") g_open_install_profile_modal = true;
            else if (!picked.empty()) install_profile_path(picked);
        });
    }
    ImGui::SameLine(0, 4.0f * s);
    bool has_sel = (g_selected_prov_idx >= 0 && g_selected_prov_idx < (int)g_prov_profiles.size()) ||
                   (g_selected_cfg_idx >= 0 && g_selected_cfg_idx < (int)g_cfg_profiles.size());
    ImGui::BeginDisabled(!has_sel);
    if (rplayhub::IconButton("##RemProf", rplayhub::Icons::drawMinus, ImVec2(28.0f * s, 26.0f * s),
                             "Remove Selected Profile")) {
        if (g_selected_prov_idx >= 0 && g_selected_prov_idx < (int)g_prov_profiles.size()) {
            g_confirm_remove_profile_type = "provisioning";
            g_confirm_remove_profile_id   = g_prov_profiles[g_selected_prov_idx].uuid;
            g_confirm_remove_profile_name = g_prov_profiles[g_selected_prov_idx].name;
        } else if (g_selected_cfg_idx >= 0 && g_selected_cfg_idx < (int)g_cfg_profiles.size()) {
            g_confirm_remove_profile_type = "configuration";
            g_confirm_remove_profile_id   = g_cfg_profiles[g_selected_cfg_idx].identifier;
            g_confirm_remove_profile_name = g_cfg_profiles[g_selected_cfg_idx].display_name;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine(width - 38.0f * s);
    if (rplayhub::IconButton("##RefProf", rplayhub::Icons::drawRefresh, ImVec2(28.0f * s, 26.0f * s), "Refresh Profiles")) {
        g_profiles_loaded = false;
        refresh_profiles();
    }
}

static void render_info_subtab_files(float width, float avail_h)
{
    if (!g_files_loaded && !g_files_loading && !g_bound_udid.empty()) {
        refresh_files();
    }
    const float s = g_scale;
    ImDrawList *dl = ImGui::GetWindowDrawList();

    /* Service selector + Up + Path + Refresh */
    ImGui::SetCursorPos(ImVec2(12.0f * s, ImGui::GetCursorPosY() + 4.0f * s));
    bool is_media = (strcmp(g_file_service, "media") == 0);
    if (ImGui::RadioButton("Media", is_media)) {
        g_file_service = "media"; g_file_path = "/"; g_files_loaded = false; refresh_files();
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Crashes", !is_media)) {
        g_file_service = "crash"; g_file_path = "/"; g_files_loaded = false; refresh_files();
    }
    ImGui::SameLine(width - 38.0f * s);
    if (rplayhub::IconButton("##RefFiles", rplayhub::Icons::drawRefresh, ImVec2(28.0f * s, 26.0f * s), "Refresh Directory")) {
        g_files_loaded = false;
        refresh_files();
    }

    ImGui::SetCursorPosX(12.0f * s);
    ImGui::BeginDisabled(g_file_path == "/");
    if (ImGui::Button("Up", ImVec2(48.0f * s, 26.0f * s))) {
        auto pos = g_file_path.rfind('/');
        g_file_path = (pos == 0 || pos == std::string::npos) ? "/" : g_file_path.substr(0, pos);
        g_files_loaded = false;
        refresh_files();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", g_file_path.c_str());

    const float list_h = std::max(80.0f, avail_h - 68.0f * s);
    ImGui::BeginChild("##FilesList", ImVec2(width, list_h), false);
    const float row_h = 40.0f * s;
    for (size_t i = 0; i < g_files.size(); i++) {
        const auto &f = g_files[i];
        ImGui::PushID((int)i);
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool clicked = ImGui::InvisibleButton("##filerow", ImVec2(width, row_h));
        bool dbl     = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        bool hovered = ImGui::IsItemHovered();
        if (hovered) {
            dl->AddRectFilled(ImVec2(p.x + 6.0f * s, p.y + 1.0f * s),
                              ImVec2(p.x + width - 6.0f * s, p.y + row_h - 1.0f * s),
                              IM_COL32(0, 0, 0, 12), 6.0f * s);
        }
        if (f.type == "dir") {
            rplayhub::Icons::drawFolder(dl, ImVec2(p.x + 12.0f * s, p.y + (row_h - 18.0f * s) * 0.5f),
                                        18.0f * s, IM_COL32(0, 122, 255, 255));
        } else {
            rplayhub::Icons::drawFile(dl, ImVec2(p.x + 12.0f * s, p.y + (row_h - 18.0f * s) * 0.5f),
                                      18.0f * s, IM_COL32(120, 120, 128, 255));
        }
        float tx = p.x + 38.0f * s;
        ImVec4 clip(tx, p.y, p.x + width - 78.0f * s, p.y + row_h);
        dl->AddText(g_font_regular, g_font_regular->FontSize,
                    ImVec2(tx, p.y + (row_h - g_font_regular->FontSize) * 0.5f),
                    IM_COL32(28, 28, 30, 255), f.name.c_str(), nullptr, 0.0f, &clip);

        if (f.type != "dir") {
            std::string sz = format_bytes(f.size);
            ImVec2 ssz = g_font_caption->CalcTextSizeA(12.5f * s, FLT_MAX, 0.0f, sz.c_str());
            dl->AddText(g_font_caption, 12.5f * s,
                        ImVec2(p.x + width - ssz.x - 12.0f * s, p.y + (row_h - 13.0f * s) * 0.5f),
                        IM_COL32(130, 130, 135, 255), sz.c_str());
        }
        if (dbl || (clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))) {
            if (f.type == "dir") {
                g_file_path = f.full_path;
                g_files_loaded = false;
                refresh_files();
            } else {
                save_remote_file(g_file_service, f.full_path, f.name, false);
            }
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

static void render_info_subtab_console(float width, float avail_h)
{
    const float s = g_scale;
    ImDrawList *dl = ImGui::GetWindowDrawList();

    ImGui::SetCursorPos(ImVec2(12.0f * s, ImGui::GetCursorPosY() + 4.0f * s));
    if (!g_syslog_running) {
        if (ImGui::Button("Start", ImVec2(64.0f * s, 26.0f * s))) start_syslog();
    } else {
        if (ImGui::Button("Stop", ImVec2(64.0f * s, 26.0f * s))) stop_syslog();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Follow", &g_syslog_follow);
    ImGui::SameLine();
    if (ImGui::Button("Clear", ImVec2(64.0f * s, 26.0f * s))) {
        std::lock_guard<std::mutex> lk(g_syslog_mu);
        g_syslog_lines.clear();
    }

    const float bottom_bar_h = 44.0f * s;
    const float log_h = std::max(80.0f, avail_h - 38.0f * s - bottom_bar_h);

    ImGui::PushFont(g_font_mono);
    ImGui::BeginChild("##SyslogScroll", ImVec2(width, log_h), true, ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::lock_guard<std::mutex> lk(g_syslog_mu);
        for (const auto &line : g_syslog_lines) {
            if (g_syslog_filter[0] && !strcasestr(line.c_str(), g_syslog_filter)) continue;
            ImGui::TextUnformatted(line.c_str());
        }
        if (g_syslog_follow && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20.0f) {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();
    ImGui::PopFont();

    ImVec2 wp = ImGui::GetWindowPos();
    float by = wp.y + ImGui::GetWindowHeight() - bottom_bar_h;
    dl->AddLine(ImVec2(wp.x, by), ImVec2(wp.x + width, by), IM_COL32(224, 224, 229, 255), 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(wp.x + 12.0f * s, by + 8.0f * s));
    ImVec2 fpos = ImGui::GetCursorScreenPos();
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 14.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(30.0f * s, 6.0f * s));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.95f, 0.95f, 0.96f, 1.0f));
    ImGui::SetNextItemWidth(width - 24.0f * s);
    ImGui::InputTextWithHint("##SyslogFilter", "Filter syslog…", g_syslog_filter, sizeof g_syslog_filter);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    rplayhub::Icons::drawSearch(dl, ImVec2(fpos.x + 10.0f * s, fpos.y + 6.5f * s),
                                15.0f * s, IM_COL32(142, 142, 147, 255));
}

static void render_info_subtab_controls(float width, float avail_h)
{
    const float s = g_scale;
    const float pad = 12.0f * s;
    ImGui::BeginChild("##ControlsSubtab", ImVec2(width, avail_h), false);

    // Section 1: Quick Controls (matching ControlPanel.swift)
    std::vector<std::pair<std::string, std::string>> ctrl_rows;
    ctrl_rows.push_back({"Window Pin", g_pinned ? "Pinned on Top" : "Normal"});
    ctrl_rows.push_back({"Screen Mirroring", g_view_screen ? "Active" : "Stopped"});
    ctrl_rows.push_back({"Recording", g_recording ? "Recording…" : "Idle"});
    ctrl_rows.push_back({"Orientation", (g_rotation == 0) ? "Portrait (0°)" :
                                       (g_rotation == 1) ? "Landscape Right (90°)" :
                                       (g_rotation == 2) ? "Upside Down (180°)" : "Landscape Left (270°)"});
    draw_section_card_ui(width, pad, s, "Device Controls", ctrl_rows);

    const float btn_w = (width - pad * 2.0f - 8.0f * s) * 0.5f;
    const float btn_h = 32.0f * s;

    ImGui::SetCursorPos(ImVec2(pad, ImGui::GetCursorPosY() + 4.0f * s));
    if (ImGui::Button(g_pinned ? "Unpin Window" : "Pin Window on Top", ImVec2(btn_w, btn_h))) {
        g_pinned = !g_pinned;
        SDL_SetWindowAlwaysOnTop(g_win, g_pinned ? SDL_TRUE : SDL_FALSE);
    }
    ImGui::SameLine(0, 8.0f * s);
    if (ImGui::Button("Home Button", ImVec2(btn_w, btn_h))) {
        api_async("press_button", {{"button", "home"}});
    }

    ImGui::SetCursorPos(ImVec2(pad, ImGui::GetCursorPosY() + 4.0f * s));
    if (ImGui::Button("Rotate Screen", ImVec2(btn_w, btn_h))) {
        g_rotation = (g_rotation + 1) & 3;
    }
    ImGui::SameLine(0, 8.0f * s);
    if (ImGui::Button("Take Screenshot", ImVec2(btn_w, btn_h))) {
        take_screenshot_action();
    }

    ImGui::SetCursorPos(ImVec2(pad, ImGui::GetCursorPosY() + 4.0f * s));
    if (ImGui::Button(g_recording ? "Stop Recording" : "Record Screen", ImVec2(btn_w, btn_h))) {
        toggle_recording_action();
    }
    ImGui::SameLine(0, 8.0f * s);
    if (ImGui::Button("Sleep / Wake", ImVec2(btn_w, btn_h))) {
        api_async("device_action", {{"action", "sleep"}});
    }

    ImGui::SetCursorPos(ImVec2(pad, ImGui::GetCursorPosY() + 4.0f * s));
    if (ImGui::Button("Restart Device…", ImVec2(btn_w, btn_h))) {
        api_async("device_action", {{"action", "restart"}});
    }
    ImGui::SameLine(0, 8.0f * s);
    if (ImGui::Button("Shut Down Device…", ImVec2(btn_w, btn_h))) {
        g_open_shutdown_modal = true;
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // Section 2: Live Stream Diagnostics (matching ControlPanel.swift lines 85-98)
    std::vector<std::pair<std::string, std::string>> diag_rows;
    char fps_buf[64], br_buf[64], res_buf[64];
    float cur_fps = g_live_fps.load();
    snprintf(fps_buf, sizeof fps_buf, "%.1f FPS (negotiated 60 FPS)", cur_fps > 0.0f ? cur_fps : 60.0f);
    double mbps = (g_stream_info.is_object() && g_stream_info.contains("mbps")) ? g_stream_info.value("mbps", 0.0) : 0.0;
    snprintf(br_buf, sizeof br_buf, "%.2f Mbps (cap 15.0 Mbps)", mbps > 0.01 ? mbps : 0.0);
    snprintf(res_buf, sizeof res_buf, "%d × %d", g_vtex_w, g_vtex_h);

    diag_rows.push_back({"Framerate", fps_buf});
    diag_rows.push_back({"Bitrate", br_buf});
    diag_rows.push_back({"Coded Resolution", res_buf});
    diag_rows.push_back({"Video Codec", "HEVC / H.265 (Low Latency D3D11/DXVA2)"});
    diag_rows.push_back({"Audio Stream", "AAC-ELD 44.1 kHz Stereo"});
    if (g_stream_info.is_object()) {
        diag_rows.push_back({"Stream Uptime", std::to_string(g_stream_info.value("uptime_s", 0)) + " s"});
        diag_rows.push_back({"Total Frames", std::to_string(g_stream_info.value("total_frames", (uint64_t)0))});
    }
    draw_section_card_ui(width, pad, s, "Live Stream Diagnostics", diag_rows);

    ImGui::EndChild();
}

static void render_info_group(float width, float height)
{
    const float s = g_scale;
    ImDrawList *dl = ImGui::GetWindowDrawList();

    /* Segmented pill sub-tab bar matching InspectorPane.swift line 106: Info | Apps | Profiles | Files | Console | Controls */
    static const char *const kSubTabs[6] = {"Info", "Apps", "Profiles", "Files", "Console", "Controls"};
    const float pad_x = 12.0f * s;
    const float bar_y = 10.0f * s;
    const float bar_h = 32.0f * s;
    const float bar_w = width - pad_x * 2.0f;
    ImVec2 wp = ImGui::GetWindowPos();
    ImVec2 bmin(wp.x + pad_x, wp.y + bar_y);
    dl->AddRectFilled(bmin, ImVec2(bmin.x + bar_w, bmin.y + bar_h), IM_COL32(236, 236, 240, 255), 16.0f * s);

    const float seg_w = bar_w / 6.0f;
    ImGui::SetCursorScreenPos(ImVec2(bmin.x + 1.0f * s, bmin.y + 1.0f * s));
    for (int i = 0; i < 6; i++) {
        bool sel = (g_info_subtab == i);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 15.0f * s);
        ImGui::PushStyleColor(ImGuiCol_Button,        sel ? rplayhub::Theme::ColorAccent : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, sel ? rplayhub::Theme::ColorAccent : ImVec4(0, 0, 0, 0.05f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  rplayhub::Theme::ColorAccent);
        ImGui::PushStyleColor(ImGuiCol_Text,          sel ? ImVec4(1, 1, 1, 1) : rplayhub::Theme::ColorTextPrimary);
        if (g_font_medium) ImGui::PushFont(g_font_medium);
        if (ImGui::Button(kSubTabs[i], ImVec2(seg_w - 2.0f * s, 30.0f * s))) {
            g_info_subtab = i;
        }
        if (g_font_medium) ImGui::PopFont();
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();
        if (i < 5) ImGui::SameLine(0, 2.0f * s);
    }

    const float content_top = bar_y + bar_h + 10.0f * s;
    const float avail_h = std::max(80.0f, height - content_top);
    ImGui::SetCursorPos(ImVec2(0, content_top));

    switch (g_info_subtab) {
    case 0: render_info_subtab_device(width, avail_h);   break;
    case 1: render_info_subtab_apps(width, avail_h);     break;
    case 2: render_info_subtab_profiles(width, avail_h); break;
    case 3: render_info_subtab_files(width, avail_h);    break;
    case 4: render_info_subtab_console(width, avail_h);  break;
    case 5: render_info_subtab_controls(width, avail_h); break;
    }
}

static void render_inspector_pane(float start_x, float width, float height)
{
    if (!g_inspector_visible) return;
    ImGui::SetNextWindowPos(ImVec2(start_x, g_menu_h));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, rplayhub::Theme::ColorBgInspector);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    ImGui::Begin("##InspectorPane", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) g_focused_pane = FocusPane::Inspector;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(start_x, g_menu_h), ImVec2(start_x, g_menu_h + height),
                IM_COL32(224, 224, 229, 255), 1.0f);

    switch (g_inspector_group) {
    case 0: render_settings_group(width);         break;
    case 1: render_report_group(width, height);   break;
    case 2: render_info_group(width, height);     break;
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

/* =========================================================================
 * 16. Modals (Shut Down, Custom Location, Install IPA/Profile, Uninstall/Remove) & Toast
 * ========================================================================= */
static void render_modals_and_toast(int win_w, int win_h)
{
    const float s = g_scale;

    if (g_open_shutdown_modal) {
        ImGui::OpenPopup("Shut Down Device?");
        g_open_shutdown_modal = false;
    }
    if (ImGui::BeginPopupModal("Shut Down Device?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Are you sure you want to shut down %s?",
                    g_bound_name.empty() ? "this device" : g_bound_name.c_str());
        ImGui::TextDisabled("You will need physical access to turn it back on.");
        ImGui::Spacing();
        if (ImGui::Button("Shut Down", ImVec2(110.0f * s, 0))) {
            api_async("device_action", {{"action", "shutdown"}});
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110.0f * s, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (g_open_custom_loc_modal) {
        ImGui::OpenPopup("Custom Coordinates");
        g_open_custom_loc_modal = false;
    }
    if (ImGui::BeginPopupModal("Custom Coordinates", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Enter decimal-degree coordinates:");
        ImGui::InputText("Latitude", g_custom_lat_buf, sizeof g_custom_lat_buf);
        ImGui::InputText("Longitude", g_custom_lon_buf, sizeof g_custom_lon_buf);
        ImGui::Spacing();
        if (ImGui::Button("Set Location", ImVec2(120.0f * s, 0))) {
            double lat = std::strtod(g_custom_lat_buf, nullptr);
            double lon = std::strtod(g_custom_lon_buf, nullptr);
            char lbl[64];
            snprintf(lbl, sizeof lbl, "%.4f, %.4f", lat, lon);
            g_custom_loc_label = lbl;
            g_set_location_idx = (int)(sizeof(kLocationPresets) / sizeof(kLocationPresets[0])) + 1;
            api_async("set_location", {{"latitude", lat}, {"longitude", lon}}, [lbl_s = std::string(lbl)](const json &r) {
                if (r.value("ok", false)) show_toast("Location set to " + lbl_s);
                else show_toast("Location failed: " + json_err_str(r, "unknown"), 6000);
            });
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100.0f * s, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (g_open_install_ipa_modal) {
        ImGui::OpenPopup("Install .ipa");
        g_open_install_ipa_modal = false;
    }
    if (ImGui::BeginPopupModal("Install .ipa", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Local .ipa path (or drag & drop an .ipa onto the window):");
        ImGui::SetNextItemWidth(360.0f * s);
        ImGui::InputText("##IpaModalPath", g_ipa_path_buf, sizeof g_ipa_path_buf);
        ImGui::Spacing();
        if (ImGui::Button("Install", ImVec2(110.0f * s, 0))) {
            install_ipa_path(g_ipa_path_buf);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110.0f * s, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (g_open_install_profile_modal) {
        ImGui::OpenPopup("Install Profile");
        g_open_install_profile_modal = false;
    }
    if (ImGui::BeginPopupModal("Install Profile", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Local .mobileprovision or .mobileconfig path:");
        ImGui::SetNextItemWidth(360.0f * s);
        ImGui::InputText("##ProfModalPath", g_profile_path_buf, sizeof g_profile_path_buf);
        ImGui::Spacing();
        if (ImGui::Button("Install", ImVec2(110.0f * s, 0))) {
            install_profile_path(g_profile_path_buf);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110.0f * s, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (!g_confirm_uninstall_bundle.empty()) {
        ImGui::OpenPopup("Uninstall App?");
    }
    if (ImGui::BeginPopupModal("Uninstall App?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Uninstall \"%s\" (%s)?", g_confirm_uninstall_name.c_str(), g_confirm_uninstall_bundle.c_str());
        ImGui::TextDisabled("All app data on the device will be removed.");
        ImGui::Spacing();
        if (ImGui::Button("Uninstall", ImVec2(110.0f * s, 0))) {
            std::string bid = g_confirm_uninstall_bundle, aname = g_confirm_uninstall_name;
            g_confirm_uninstall_bundle.clear();
            api_async("uninstall_app", {{"bundle_id", bid}}, [aname](const json &r) {
                if (r.value("ok", false)) {
                    show_toast("Uninstalled " + aname);
                    g_apps_loaded = false;
                    refresh_apps();
                } else {
                    show_toast("Uninstall failed: " + json_err_str(r, "unknown"), 6000);
                }
            });
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110.0f * s, 0))) {
            g_confirm_uninstall_bundle.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (!g_confirm_remove_profile_id.empty()) {
        ImGui::OpenPopup("Remove Profile?");
    }
    if (ImGui::BeginPopupModal("Remove Profile?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Remove profile \"%s\"?", g_confirm_remove_profile_name.c_str());
        ImGui::Spacing();
        if (ImGui::Button("Remove", ImVec2(110.0f * s, 0))) {
            std::string ptype = g_confirm_remove_profile_type;
            std::string pid   = g_confirm_remove_profile_id;
            std::string pname = g_confirm_remove_profile_name;
            g_confirm_remove_profile_id.clear();
            json params = {{"type", ptype}};
            if (ptype == "provisioning") params["uuid"] = pid;
            else params["identifier"] = pid;
            api_async("remove_profile", params, [pname](const json &r) {
                if (r.value("ok", false)) {
                    show_toast("Removed profile " + pname);
                    g_profiles_loaded = false;
                    refresh_profiles();
                } else {
                    show_toast("Remove failed: " + json_err_str(r, "unknown"), 6000);
                }
            });
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110.0f * s, 0))) {
            g_confirm_remove_profile_id.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    /* Floating macOS-style toast banner at the bottom center */
    if (!g_toast_msg.empty() && now_ms() < g_toast_until_ms) {
        ImDrawList *fg = ImGui::GetForegroundDrawList();
        ImVec2 tsz = g_font_medium->CalcTextSizeA(14.0f * s, FLT_MAX, 0.0f, g_toast_msg.c_str());
        float pw = tsz.x + 28.0f * s, ph = tsz.y + 14.0f * s;
        ImVec2 pmin((win_w - pw) * 0.5f, win_h - ph - 56.0f * s);
        fg->AddRectFilled(pmin, ImVec2(pmin.x + pw, pmin.y + ph), IM_COL32(32, 32, 36, 235), ph * 0.5f);
        fg->AddText(g_font_medium, 14.0f * s, ImVec2(pmin.x + 14.0f * s, pmin.y + 7.0f * s),
                    IM_COL32(255, 255, 255, 255), g_toast_msg.c_str());
    }
}

/* =========================================================================
 * 17. Main Entry Point
 * ========================================================================= */
int main(int argc, char **argv)
{
    int rvra = 1;
    int audio = 1;
    float cli_scale = 0.0f;

    if (const char *env_s = getenv("RPLAYHUB_SCALE")) cli_scale = std::strtof(env_s, nullptr);
    if (const char *env_tb = getenv("RPLAYHUB_SYSTEM_TITLEBAR")) g_system_titlebar = (atoi(env_tb) != 0);
    if (const char *env_av = getenv("RPLAYHUB_AUTO_VIEW")) g_view_screen = (atoi(env_av) != 0);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") && i + 1 < argc) g_host = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) g_video_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-A") && i + 1 < argc) g_api_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-audio")) audio = 0;
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) rvra = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-scale") && i + 1 < argc) cli_scale = std::strtof(argv[++i], nullptr);
        else if (!strcmp(argv[i], "--system-titlebar")) g_system_titlebar = true;
        else if (!strcmp(argv[i], "--auto-connect")) g_view_screen = true;
        else if (!strcmp(argv[i], "--help")) {
            printf("usage: %s [-h host] [-p video_port] [-A api_port] [--no-audio] [-r 0|1] [-scale F] [--system-titlebar] [--auto-connect]\n", argv[0]);
            return 0;
        }
        else {
            fprintf(stderr, "usage: %s [-h host] [-p video_port] [-A api_port] [--no-audio] [-r 0|1] [-scale F] [--system-titlebar] [--auto-connect]\n", argv[0]);
            return 2;
        }
    }
    setenv("RPLAY_RVRA", rvra ? "1" : "0", 1);

    SDL_SetHint(SDL_HINT_VIDEO_HIGHDPI_DISABLED, "0");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    /* Auto-detect UI scale factor from display resolution matching rplay-hub-android-dev/linux/src/ui/gui_app.cc */
    SDL_DisplayMode dm{};
    SDL_GetCurrentDisplayMode(0, &dm);
    if (cli_scale > 0.25f) {
        g_scale = std::clamp(cli_scale, 0.75f, 3.0f);
    } else if (dm.w >= 3000 || dm.h >= 1800) {
        g_scale = 1.5f;   /* 4K / HiDPI display */
    } else if (dm.w >= 2400 || dm.h >= 1400) {
        g_scale = 1.3f;   /* QHD / 1440p display */
#ifdef _WIN32
    } else {
        g_scale = 1.0f;
    }
#else
    } else {
        g_scale = 1.15f;  /* 1080p display — slightly boosted for crispness */
    }
#endif

    int init_w = (int)std::lround(1350.0f * g_scale);
    int init_h = (int)std::lround(840.0f * g_scale);
    if (dm.w > 0 && init_w > dm.w - 80) init_w = dm.w - 80;
    if (dm.h > 0 && init_h > dm.h - 80) init_h = dm.h - 80;
    Uint32 win_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_OPENGL;
    if (!g_system_titlebar) win_flags |= SDL_WINDOW_BORDERLESS;

    const std::string argb_vis = g_system_titlebar ? "" : rplayhub::argbVisualId();
    for (int attempt = 0; attempt < 2 && !g_ren; attempt++) {
        bool try_argb = (attempt == 0 && !argb_vis.empty());
        if (attempt == 1 && argb_vis.empty()) break;
        if (try_argb) SDL_SetHint(SDL_HINT_VIDEO_X11_WINDOW_VISUALID, argb_vis.c_str());
        g_win = SDL_CreateWindow("rPlayHub", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 init_w, init_h, win_flags);
        if (g_win) {
            g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
            if (!g_ren) g_ren = SDL_CreateRenderer(g_win, -1, 0);
        }
        if (try_argb) SDL_SetHint(SDL_HINT_VIDEO_X11_WINDOW_VISUALID, "");
        if (!g_win || !g_ren) {
            if (g_win) { SDL_DestroyWindow(g_win); g_win = nullptr; }
            continue;
        }
        g_argb = try_argb;
    }
    if (!g_win || !g_ren) {
        fprintf(stderr, "SDL window/renderer failed: %s\n", SDL_GetError());
        return 1;
    }
    bool has_vsync = false;
    SDL_RendererInfo rinfo;
    if (SDL_GetRendererInfo(g_ren, &rinfo) == 0) {
        has_vsync = (rinfo.flags & SDL_RENDERER_PRESENTVSYNC) != 0;
    }
    SDL_SetWindowMinimumSize(g_win, (int)std::lround(760.0f * g_scale), (int)std::lround(520.0f * g_scale));
    SDL_ShowWindow(g_win);
    SDL_RaiseWindow(g_win);
#ifdef _WIN32
    timeBeginPeriod(1);
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    if (SDL_GetWindowWMInfo(g_win, &wmInfo)) {
        HWND hwnd = wmInfo.info.win.window;
        HINSTANCE hInst = GetModuleHandle(NULL);
        HICON hIcon = LoadIcon(hInst, MAKEINTRESOURCE(1));
        if (hIcon) {
            SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
            SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
        }
        if (g_system_titlebar) {
            setup_win32_native_menu(hwnd);
        }
    }
#endif

    if (!g_system_titlebar) {
        SDL_SetWindowHitTest(g_win, window_hit_test, nullptr);
    }
    g_cursor_arrow       = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
    g_cursor_resize_ew   = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEWE);
    g_cursor_resize_ns   = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZENS);
    g_cursor_resize_nwse = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZENWSE);
    g_cursor_resize_nesw = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZENESW);

    /* Wire pop-out DisplayWindow touch/scroll/button callbacks to cdhost API */
    {
        static float pop_fx0 = 0.0f, pop_fy0 = 0.0f;
        static uint64_t pop_t0 = 0;
        g_popout_session.on_touch = [](int x, int y, int action, int32_t) {
            float dw = (float)std::max(1, g_vtex_w);
            float dh = (float)std::max(1, g_vtex_h);
            float fx = std::clamp(x / dw, 0.0f, 1.0f);
            float fy = std::clamp(y / dh, 0.0f, 1.0f);
            if (action == rplayhub::MotionAction::DOWN) {
                pop_fx0 = fx; pop_fy0 = fy; pop_t0 = now_ms();
            } else if (action == rplayhub::MotionAction::UP) {
                float dist = std::hypot((fx - pop_fx0) * dw, (fy - pop_fy0) * dh);
                int dur = (int)std::clamp<uint64_t>(now_ms() - pop_t0, 80, 1000);
                if (dist < 12.0f) api_async("tap", {{"fx", pop_fx0}, {"fy", pop_fy0}});
                else api_async("swipe", {{"fx0", pop_fx0}, {"fy0", pop_fy0}, {"fx1", fx}, {"fy1", fy}, {"duration_ms", dur}});
            }
        };
        g_popout_session.on_scroll = [](int x, int y, float, float wy, int32_t) {
            if (std::fabs(wy) < 0.05f) return;
            float fx = std::clamp((float)x / (float)std::max(1, g_vtex_w), 0.0f, 1.0f);
            float fy = std::clamp((float)y / (float)std::max(1, g_vtex_h), 0.0f, 1.0f);
            float fy1 = std::clamp(fy + (wy > 0.0f ? 0.22f : -0.22f), 0.05f, 0.95f);
            api_async("swipe", {{"fx0", fx}, {"fy0", fy}, {"fx1", fx}, {"fy1", fy1}, {"duration_ms", 180}});
        };
        g_popout_session.on_key = [](int key) {
            if (key == rplayhub::AndroidKey::HOME || key == rplayhub::AndroidKey::BACK) {
                api_async("press_button", {{"button", "home"}});
            }
        };
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    ImGui_ImplSDL2_InitForSDLRenderer(g_win, g_ren);
    ImGui_ImplSDLRenderer2_Init(g_ren);

    rplayhub::Theme::applyMacStyle(g_scale);
    discover_fonts();
    rebuild_fonts();

    std::thread vthr(video_thread_main);
    if (audio) audio_start(g_host, g_api_port + 2);
    refresh_devices();

    while (!g_quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (g_popout_win && g_popout_win->handleEvent(ev, &g_popout_session)) {
                continue;
            }
            ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type == SDL_QUIT) {
                g_quit = true;
            } else if (ev.type == SDL_KEYDOWN && !ImGui::GetIO().WantTextInput) {
                SDL_Keymod mod = SDL_GetModState();
                bool ctrl = (mod & KMOD_CTRL) != 0;
                bool shift = (mod & KMOD_SHIFT) != 0;
                if (ctrl && !shift && ev.key.keysym.sym == SDLK_r) {
                    refresh_devices();
                } else if (ctrl && shift && ev.key.keysym.sym == SDLK_h) {
                    api_async("press_button", {{"button", "home"}});
                } else if (ctrl && !shift && ev.key.keysym.sym == SDLK_l) {
                    g_rotation = (g_rotation + 1) & 3;
                } else if (ctrl && !shift && ev.key.keysym.sym == SDLK_s) {
                    take_screenshot_action();
                } else if (ctrl && !shift && ev.key.keysym.sym == SDLK_t) {
                    open_screen_tab();
                } else if (ctrl && !shift && ev.key.keysym.sym == SDLK_w) {
                    close_screen_tab(g_current_tab);
                } else if (ctrl && !shift && ev.key.keysym.sym == SDLK_n) {
                    toggle_popout_window();
                } else if (ctrl && !shift && ev.key.keysym.sym == SDLK_p) {
                    g_pinned = !g_pinned;
                    SDL_SetWindowAlwaysOnTop(g_win, g_pinned ? SDL_TRUE : SDL_FALSE);
                }
            } else if (ev.type == SDL_WINDOWEVENT &&
                       ev.window.windowID == SDL_GetWindowID(g_win) &&
                       ev.window.event == SDL_WINDOWEVENT_CLOSE) {
                g_quit = true;
            } else if (ev.type == SDL_DROPFILE && ev.drop.file) {
                std::string dropped = ev.drop.file;
                SDL_free(ev.drop.file);
                handle_dropped_file(dropped);
            }
        }

        drain_ui_callbacks();
        if (g_fonts_dirty) rebuild_fonts();

        /* Periodic background polls: device list (every 3s), stream_info (every 2s when Info tab open) */
        uint64_t now = now_ms();
        if (now - g_last_dev_poll_ms >= 3000) {
            g_last_dev_poll_ms = now;
            refresh_devices();
        }
        if (g_inspector_visible && g_inspector_group == 2 && g_info_subtab == 0 &&
            now - g_last_stream_poll_ms >= 2000) {
            g_last_stream_poll_ms = now;
            api_async("stream_info", {}, [](const json &r) {
                if (r.value("ok", false)) g_stream_info = r["result"];
            }, 3);
        }

        upload_latest_video_frame();
        if (g_popout_win) {
            if (g_popout_win->closeRequested()) {
                g_popout_win.reset();
            } else {
                g_popout_win->render(g_popout_frame);
            }
        }

        update_resize_cursor();

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        int win_w = 0, win_h = 0;
        SDL_GetWindowSize(g_win, &win_w, &win_h);

        render_titlebar((float)win_w);

        const float body_h      = std::max(100.0f, (float)win_h - g_menu_h);
        const float sidebar_w   = g_sidebar_hidden ? 0.0f : 260.0f * g_scale;
        const float inspector_w = g_inspector_visible ? 320.0f * g_scale : 0.0f;
        const float stage_w     = std::max(120.0f, (float)win_w - sidebar_w - inspector_w);

        render_sidebar(body_h);
        render_center_stage(sidebar_w, stage_w, body_h);
        render_inspector_pane((float)win_w - inspector_w, inspector_w, body_h);
        render_modals_and_toast(win_w, win_h);

        /* GNOME-style 1px dark window outline on top of all panes, inside the corner cut */
        const float radius = (!g_system_titlebar && !(SDL_GetWindowFlags(g_win) & SDL_WINDOW_MAXIMIZED))
                           ? 11.0f * g_scale : 0.0f;
        if (radius > 0.0f) {
            ImGui::GetForegroundDrawList()->AddRect(
                ImVec2(0.5f, 0.5f), ImVec2((float)win_w - 0.5f, (float)win_h - 0.5f),
                IM_COL32(0, 0, 0, 100), radius, 0, 1.0f);
        }

        ImGui::Render();
        if (g_argb) SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 0);
        else SDL_SetRenderDrawColor(g_ren, 247, 247, 250, 255);
        SDL_RenderClear(g_ren);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), g_ren);

        if (g_argb && radius > 0.0f) {
            int out_w = 0, out_h = 0;
            SDL_GetRendererOutputSize(g_ren, &out_w, &out_h);
            float px = win_w > 0 ? (float)out_w / (float)win_w : 1.0f;
            rplayhub::cutCorners(g_ren, out_w, out_h, radius * px);
        }
        SDL_RenderPresent(g_ren);
        if (!has_vsync) {
            uint64_t frame_ms = now_ms() - now;
            if (frame_ms < 16) SDL_Delay((Uint32)(16 - frame_ms));
        }
    }

    stop_syslog();
    vthr.join();

    g_popout_win.reset();
    for (auto &kv : g_app_icon_cache) {
        if (kv.second) SDL_DestroyTexture(kv.second);
    }
    if (g_back_texture) SDL_DestroyTexture(g_back_texture);
    if (g_vtex) SDL_DestroyTexture(g_vtex);

    if (g_cursor_arrow) SDL_FreeCursor(g_cursor_arrow);
    if (g_cursor_resize_ew) SDL_FreeCursor(g_cursor_resize_ew);
    if (g_cursor_resize_ns) SDL_FreeCursor(g_cursor_resize_ns);
    if (g_cursor_resize_nwse) SDL_FreeCursor(g_cursor_resize_nwse);
    if (g_cursor_resize_nesw) SDL_FreeCursor(g_cursor_resize_nesw);

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(g_ren);
    SDL_DestroyWindow(g_win);
#ifdef _WIN32
    timeEndPeriod(1);
#endif
    SDL_Quit();
    return 0;
}
