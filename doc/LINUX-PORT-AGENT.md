# Linux port — agent onboarding

You are an agent (e.g. Claude Code) starting fresh on a **Linux** machine to port rplay-hub. This
is your setup + first-tasks guide. Read `doc/LINUX-PORT-HANDOFF.md` next for the deeper state, and
`doc/RVRA-AND-PORTABILITY.md` before touching the video path.

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

# engine (cdhost): the libimobiledevice stack + TLS, and usbmuxd to reach a USB phone
apt install -y libimobiledevice-dev libplist-dev libusbmuxd-dev libssl-dev usbmuxd
```

## 3. Build the CLIENT first — it needs no iPhone

The portable client (`client-c/rplay-view`) is the novel, risky piece (RVRA software decode) and
is **fully verifiable with captures, no device**. Do this before the engine.

```bash
make -C client-c            # links deps/ffmpeg automatically when built (else warns + uses system ffmpeg)
```
Verify against the committed reference captures (`reference/captures/`):
```bash
# RVRA-correct decode: 601/601 frames, RVRA engaged, zero decoder errors
RPLAY_RVRA=1 ./client-c/rplay-view --check -f reference/captures/apple_video_REFERENCE.h265
# expect: "... decoded 601 ... 0 decode errors ..." and "RVRA reference resampling enabled"
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

## 4. Build the ENGINE

`host-c/cdhost` is portable (libimobiledevice/libplist everywhere, lwIP userspace net, no
CoreFoundation). Expect **Makefile friction, not architectural problems** — the `host-c/Makefile`
is macOS-tuned:
- `IMD_PREFIX` defaults to `/opt/homebrew`; on Linux set `IMD_PREFIX=/usr` (or wherever apt put
  the libs). The non-STATIC path already links `-limobiledevice-1.0 -lplist-2.0` dynamically.
- The `STATIC=1` path uses homebrew `.a` archives and macOS `ld` flags (`-dead_strip`, `-Wl,-S`);
  it is macOS-only. Build the **dynamic** target on Linux.
- A few socket options are already `#ifdef`'d for Darwin (`SO_NOSIGPIPE`, `SO_TRAFFIC_CLASS`,
  `SO_NET_SERVICE_TYPE`); add Linux guards if the compiler complains.

```bash
make -C host-c IMD_PREFIX=/usr           # dynamic build
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
3. Add Linux CI (a GitHub Actions job that does §2–3 on `ubuntu-latest`) so the port can't silently
   regress. This is the highest-value non-device task.
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
