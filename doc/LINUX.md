# rPlayHub on Linux

The Linux client is the same product as the macOS app: the engine (`cdhost`) talks to the
iPhone over the CoreDevice protocol, and a native GUI (`rplayhub`, Dear ImGui over SDL2) shows
the screen and drives the rest of Device Hub. This page is how to install it, run it, build it
from source, and package it.

x86_64, Ubuntu 22.04 or later, Debian 12 or later. The GUI runs on X11 and on Wayland (through
XWayland); the borderless window's rounded corners are real on X11 and square on Wayland.

## Install (Debian package)

Download the `.deb` from the [latest Linux release](https://github.com/rPlayAI/rPlayHub/releases)
and install it:

```sh
sudo apt install ./rplayhub_0.1.2-1_amd64.deb
```

It installs:

| | |
|---|---|
| `rplayhub` | the GUI, also in the launcher as "rPlayHub" |
| `rplayhub-engine` | the engine; runs as you, no root |
| `rplayhub-fetch-ddi` | downloads Apple's Developer Disk Image once (below) |
| `/usr/share/rplayhub/fonts` | the Inter fonts the GUI uses |

and pulls in `usbmuxd` (the USB daemon the engine talks to), SDL2, and the distribution's
OpenSSL, libcurl and X11 libraries. FFmpeg and libimobiledevice are built in, for the reasons
under "Why the package builds its own" below.

## Run

1. **Once:** fetch the Developer Disk Image. The phone forgets it on every reboot and the engine
   mounts it again for you, but the files are Apple's and cannot ship in the package:

   ```sh
   rplayhub-fetch-ddi        # -> ~/.local/share/rplayhub/iOS_DDI
   ```

   Details and other sources: [DDI.md](DDI.md).

2. Plug the phone in over USB, unlock it, and tap **Trust** if it asks. iOS 27 or later, with
   Developer Mode on (Settings ▸ Privacy & Security ▸ Developer Mode).

3. Start the engine, then the GUI, in two terminals or with the GUI from the launcher:

   ```sh
   rplayhub-engine           # binds the USB phone; :9876 control, :9877 video
   rplayhub
   ```

   Either order works: the GUI shows "Plug in USB & start cdhost" until the engine answers, and
   retries by itself. With more than one phone, `rplayhub-engine --udid <prefix>` picks one.

The engine runs unprivileged: it carries the CoreDevice tunnel over its own userspace TCP/IP
stack (lwIP) instead of a kernel TUN device, which is what `rplayhub-engine` turns on
(`RPLAY_USERSPACE_NET=1`). `rplayhub-engine --help` lists the bitrate and codec knobs.

GUI options: `rplayhub [-h host] [-p video_port] [-A api_port] [--no-audio] [-r 0|1]
[-scale F] [--system-titlebar]`. `--system-titlebar` keeps your window manager's title bar
instead of the macOS-style chrome; `-scale` overrides the HiDPI factor.

## Build from source

Debian / Ubuntu:

```sh
sudo apt install build-essential pkg-config git curl nasm \
     libsdl2-dev libssl-dev libcurl4-openssl-dev zlib1g-dev \
     autoconf automake libtool \
     libx11-dev libxext-dev libgl-dev
```

The last three are optional: they give the main window and the pop-out phone window their
translucent rounded corners on X11 (an ARGB visual chosen through Xlib and GLX). Without them the
build still works and the corners are square.

Then, from a clone of this repository:

```sh
./scripts/build-ffmpeg-rvra.sh      # FFmpeg 8.1.2 with the RVRA patch -> deps/ffmpeg (once, ~5 min)
./scripts/build-imd-stack.sh        # modern libimobiledevice stack -> deps/imd (once, ~3 min)
git clone --branch STABLE-2_2_1_RELEASE https://github.com/lwip-tcpip/lwip.git deps/lwip
./scripts/fetch-gui-deps.sh         # Dear ImGui + nlohmann/json -> deps/imgui, deps/json

make -C host-c IMD_PREFIX=$PWD/deps/imd    # -> host-c/cdhost
make -C client-c rplay-gui                 # -> client-c/rplay-gui
make -C client-c                           # -> client-c/rplay-view, the minimal reference viewer
```

Run what you built:

```sh
./scripts/fetch-ddi.sh                        # once
RPLAY_USERSPACE_NET=1 ./host-c/cdhost         # the engine, as you
./client-c/rplay-gui                          # the GUI
```

`deps/` is gitignored; everything in it is regenerated per machine from pinned versions. CI
(`.github/workflows/linux.yml`) does exactly these steps on a clean Ubuntu runner on every push.

### Why the package builds its own FFmpeg and libimobiledevice

- **FFmpeg.** The iPhone's encoder uses RVRA, an in-sequence resolution adaptation standard HEVC
  has no mechanism for. Stock FFmpeg decodes the stream without a warning and produces a garbled
  picture from the first downshift under motion. `patches/ffmpeg-rvra.patch` adds the reference
  resampling to FFmpeg's HEVC decoder; `scripts/build-ffmpeg-rvra.sh` applies it to the 8.1.2
  release and builds it as a plain LGPL tree (`--disable-autodetect`, zlib on for PNG app icons).
  `client-c/Makefile` links that tree statically when it is built, and falls back to the system
  FFmpeg with a warning when it is not. [RVRA-AND-PORTABILITY.md](RVRA-AND-PORTABILITY.md) has
  the full story.
- **libimobiledevice.** The engine needs libplist 2.3 or later and libimobiledevice 1.4 (with
  libimobiledevice-glue and libtatsu). Ubuntu 22.04 and Debian 12 ship older releases, so
  `scripts/build-imd-stack.sh` builds the pinned tags from source. The distribution's `usbmuxd`
  daemon is fine and is what the package depends on.

## Package

```sh
./scripts/package-deb.sh            # -> build/deb/rplayhub_<version>-1_<arch>.deb
```

The version is `RPLAYHUB_VERSION` in `client-c/Makefile` (or the script's first argument). The
script builds the GUI against the distribution's SDL2 headers and the RVRA FFmpeg, builds a copy
of the libimobiledevice stack into `build/deb/imd` against the distribution's OpenSSL and libcurl
and links the engine to it statically, checks that neither binary needs anything outside the
distribution (a self-built SDL2 or OpenSSL in `/usr/local` is bypassed, not shipped), stages the
files, derives `Depends:` from the binaries' `DT_NEEDED` entries, and runs `dpkg-deb` and
`lintian`. It needs `dpkg-dev fakeroot lintian` on top of the build packages above.

Releases of the Linux client are tagged `linux-v<version>` and carry the `.deb` and its SHA-256.
The macOS app has its own version and DMG.

## Troubleshooting

- **"No devices found"** in the sidebar: the engine is not running or `usbmuxd` does not see the
  phone. `rplayhub-engine` prints what it finds; `usbmuxd` starts on demand through udev when a
  phone is plugged in, and `systemctl status usbmuxd` shows it.
- **The engine says it cannot mount the DDI**: run `rplayhub-fetch-ddi` and check
  `~/.local/share/rplayhub/iOS_DDI/Restore/` has `BuildManifest.plist`, `Image.dmg` and
  `Image.dmg.trustcache`. `RPLAY_DDI=/some/dir` points the engine elsewhere.
- **The picture garbles under motion** in a from-source build: `make -C client-c` warned that
  `deps/ffmpeg` was not built, so the GUI linked the system FFmpeg. Run
  `scripts/build-ffmpeg-rvra.sh` and rebuild.
- **App icons are blank** in a from-source build: `deps/ffmpeg` was built before zlib was enabled
  in the script. `rm -rf deps/ffmpeg` and run `scripts/build-ffmpeg-rvra.sh` again.
