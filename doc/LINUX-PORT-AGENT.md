# Linux port — agent onboarding

You are an agent (e.g. Claude Code) starting fresh on a **Linux** machine to port rplay-hub. This
is your setup + first-tasks guide. Read `doc/LINUX-PORT-HANDOFF.md` next for the deeper state, and
`doc/RVRA-AND-PORTABILITY.md` before touching the video path.

**Head start:** the portable client is already implemented — `client-c/rplay-view.c` is a working
**SDL2 + ffmpeg** viewer with no macOS dependencies. Most of the "Linux client" is a recompile of
existing code (§3), not new work. The engine is likewise already portable; Linux is mostly Makefile
friction (§4).

## The one rule: git is the single source of truth

This project is developed on macOS **and** Linux (and later Windows) from **one GitHub repo**.
Every host is a clone; you `commit` + `push`; the others `git pull`. **Never rsync the tree
between machines** — that is how they diverge. Commit your Linux fixes and push them back so the
Mac side stays in sync.

## 1. Clone from GitHub

Repo: **`github.com/rPlayAI/rplay-hub`** (private). Working branch: **`rendering-resolution-switch`**
(88+ commits ahead of `main`; `main` is stale — do not use it).

Authenticate first (the repo is private), any one of:
```bash
gh auth login                      # GitHub CLI, easiest
# or use an SSH key added to the account, or a Personal Access Token in the clone URL
```
Then:
```bash
git clone -b rendering-resolution-switch https://github.com/rPlayAI/rplay-hub.git
cd rplay-hub
```

## 2. Fetch the vendored deps (gitignored — NOT in the repo)

`deps/` is entirely gitignored and is **regenerated per-host** (a macOS-built `libavcodec.a` is
useless on Linux — each OS builds its own natively). Two things to fetch:

```bash
# lwIP (userspace TCP/IP for the engine) — pinned release
git clone --branch STABLE-2_2_1_RELEASE https://github.com/lwip-tcpip/lwip.git deps/lwip

# ffmpeg + the RVRA patch (REQUIRED for correct video — see §5)
apt install -y build-essential pkg-config nasm    # ffmpeg build deps
./scripts/build-ffmpeg-rvra.sh                    # fetches ffmpeg 8.1.2, applies patches/ffmpeg-rvra.patch, builds deps/ffmpeg
```

System packages:
```bash
# client (rplay-view): SDL2 (+ system ffmpeg only for the no-RVRA fallback)
apt install -y libsdl2-dev

# engine (cdhost): TLS + build tools for the libimobiledevice stack, and usbmuxd for a USB phone
apt install -y libssl-dev zlib1g-dev libcurl4-openssl-dev autoconf automake libtool usbmuxd
```

**Do NOT use the distro's libimobiledevice packages for the engine** — the code needs the modern
stack (libplist >= 2.3 for the 4-arg `plist_from_memory`; libimobiledevice 1.4 also pulls in
libimobiledevice-glue and libtatsu), and Ubuntu 22.04 ships libplist 2.2 / libimobiledevice 1.3.
Build it from pinned source tags instead (the same versions homebrew ships on the Mac side):
```bash
./scripts/build-imd-stack.sh          # installs to deps/imd; gitignored, per-host like ffmpeg
```

## 3. Build the CLIENT first — it already exists; you're recompiling it

**The SDL2 + ffmpeg client is already written: `client-c/rplay-view.c`.** It's a single file —
POSIX sockets, `libavcodec` software decode, SDL2 render — with no macOS-specific code, authored
and validated on the Mac against the reference captures. On Linux this is a **recompile, not a
rewrite**: `apt install libsdl2-dev`, build the patched ffmpeg (§2), `make`. Treat `rplay-view.c`
as the reference implementation for the whole portable video path — it already does Annex-B split,
access-unit assembly, keyframe gating, the RVRA active-rect crop, single-thread decode, and SDL
display. It's also **fully verifiable with captures, no iPhone needed**. Do this before the engine.

```bash
make -C client-c            # links deps/ffmpeg automatically when built (else warns + uses system ffmpeg)
```
Verify against the committed reference captures (`reference/captures/` is gitignored as a
directory, but `gop-reproducer.h265` and `apple_video_REFERENCE.h265` are force-added and travel
with the repo; CI checks both):
```bash
# RVRA-correct decode: 601/601 frames, RVRA engaged, zero decoder errors
RPLAY_RVRA=1 ./client-c/rplay-view --check -f reference/captures/apple_video_REFERENCE.h265
# expect: "... decoded 601, decode errors 0 ..." and "RVRA reference resampling enabled"
```
The pixel-correctness of RVRA was proven on macOS against a VideoToolbox ground truth
(`scripts/compare-decodes.py`, worst frame 39 dB). ffmpeg's decoder is deterministic C, so the
**same patched build on Linux produces the same pixels** — you mainly need to confirm it builds
and decodes clean. (If you want the pixel diff on Linux too, the ground-truth `gt.y4m` must be
generated on a Mac — `build/groundtruth`, VideoToolbox-only — and copied over; it is ~2.6 GB and
not committed.)

Windowed smoke test (needs a display / X or Wayland):
```bash
./client-c/rplay-view -f reference/captures/gop-reproducer.h265 -r 60
```

## 3b. GUI: the mirror view ports; the app chrome does not

Be clear about scope: **`rplay-view.c` is only the mirror window** (live video + click→tap /
drag→swipe). It is *not* the macOS app's full interface. The macOS app (`app/rPlayHub/`,
Swift/AppKit) adds a device **sidebar** and an **inspector with tab views** — Info, Apps, Console,
Files, Profiles — plus a control strip and power actions. **All of that is AppKit: macOS-only, and
it does not port.** A Linux build will not have those tab views for free.

But none of it is business logic. Every panel is just a front-end over the engine's **JSON API on
`127.0.0.1:9876`** (`app/api/PROTOCOL.md`): `list_apps` / `launch_app`, `syslog`, `list_dir` /
`read_file`, `device_info`, `list_profiles`, `device_action`, `take_screenshot`, etc. The macOS
GUI is one client of that API; the Python SDK (`rplayhub-sdk`) is another. So on Linux the "app" is
a **design choice, not a translation of the Swift**:

- **Minimal (recommended first):** ship `rplay-view` (mirror + input) and reach device management
  through the JSON API / the SDK / a small CLI. Fastest, and it exercises the whole engine.
- **Full GUI later:** build a native front-end in a Linux toolkit — Qt, GTK, Dear ImGui, or a local
  web UI — that re-implements the sidebar + inspector tabs against the **same** 9876 API. This is
  new UI code in whatever toolkit you pick; the tab views are redrawn, not ported. Because the API
  already returns everything the tabs display, any toolkit works and none of the wire logic changes.

Get the mirror + engine + API working first; decide the GUI framework after, as its own task.

## 4. Build the ENGINE

`host-c/cdhost` is portable (libimobiledevice/libplist everywhere, lwIP userspace net, no
CoreFoundation) and **builds on Linux as of 2026-08-27**:
- On Linux the Makefile defaults `OPENSSL=/usr` and bakes an rpath for a non-`/usr` `IMD_PREFIX`,
  so the binary runs without `LD_LIBRARY_PATH`. Point `IMD_PREFIX` at the from-source stack (§2).
- The `STATIC=1` path uses homebrew `.a` archives and macOS `ld` flags (`-dead_strip`, `-Wl,-S`);
  it is macOS-only. Build the **dynamic** target on Linux.
- The kernel-utun tunnel path is `#ifdef __APPLE__` — on Linux the engine requires
  `RPLAY_USERSPACE_NET=1` (lwIP) and says so if started without it.

```bash
make -C host-c IMD_PREFIX=$PWD/deps/imd  # dynamic build against scripts/build-imd-stack.sh
```
Testing the engine needs `usbmuxd` running and **a phone plugged into this Linux box** (or the
remote path). The client needs neither. `RPLAY_USERSPACE_NET=1` runs the engine non-root over lwIP.

## 5. Why RVRA matters (do not skip)

The device's screen stream uses Apple's proprietary in-sequence resolution adaptation (RVRA).
**Stock ffmpeg decodes it conformantly and WRONG** — garbage from the first motion downshift, with
no error. `patches/ffmpeg-rvra.patch` (built into `deps/ffmpeg` by `build-ffmpeg-rvra.sh`, gated by
`RPLAY_RVRA=1` + single-thread) adds the reference resampling that fixes it. The client links the
patched libs and sets `RPLAY_RVRA=1` itself. If you ever see garbled motion, you are on stock
ffmpeg — rebuild `deps/ffmpeg` from the patch. Full story: `doc/RVRA-AND-PORTABILITY.md`.

## 6. Suggested first-session order

1. Clone + fetch deps (§1–2).
2. Build + verify the **client** against the captures (§3) — proves the hardest part (RVRA decode)
   works on Linux with no device.
3. Linux CI exists: `.github/workflows/linux.yml` builds the RVRA ffmpeg + client and checks the
   capture decode, and builds the engine against the from-source stack, on every push. Keep it
   green so the port can't silently regress.
4. Build the **engine** dynamically (§4); fix Makefile/`#ifdef` friction; commit the fixes.
5. Only then, with a phone on the Linux box + usbmuxd, exercise the live path.

## 7. Do / don't

- **Do** commit and push every Linux fix (Makefile guards, `#ifdef`s) to
  `rendering-resolution-switch` so macOS stays in sync.
- **Do** keep OS-specific build logic behind `ifeq ($(shell uname),...)` or feature guards, not
  forks of the file.
- **Don't** commit anything under `deps/` (gitignored; regenerated by the scripts).
- **Don't** rsync from the Mac — clone/pull from GitHub.
- **Don't** branch off `main` (stale).

## Key docs

- `doc/LINUX-PORT-HANDOFF.md` — full state of the port (engine seams, ffmpeg evaluation).
- `doc/RVRA-AND-PORTABILITY.md` — the RVRA decode problem and the patch (authoritative for video).
- `app/PORTING.md`, `README.md` — architecture overview.
- `doc/REMOTE-OVER-TAILSCALE.md` — remote-device research (separate track; not needed for the port).
