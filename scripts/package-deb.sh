#!/usr/bin/env bash
# Build the Linux client and engine as a Debian package: scripts/package-deb.sh [version]
#
# What goes in: /usr/bin/rplayhub (the GUI, client-c/rplay-gui), /usr/bin/rplayhub-engine (a
# wrapper that runs the engine unprivileged over its userspace TCP/IP stack), the engine itself
# in /usr/libexec/rplayhub/cdhost, /usr/bin/rplayhub-fetch-ddi (scripts/fetch-ddi.sh), the Inter
# fonts, a launcher entry and icons. Output: build/deb/rplayhub_<version>-1_<arch>.deb
#
# The GUI links the RVRA-patched FFmpeg from deps/ffmpeg STATICALLY -- the distribution's FFmpeg
# cannot decode this stream correctly (doc/RVRA-AND-PORTABILITY.md), and that tree is a plain LGPL
# build (no --enable-gpl, no nonfree). The engine links the libimobiledevice stack statically --
# distribution packages are too old (Ubuntu 22.04 ships libplist 2.2) -- from a copy this script
# builds into build/deb/imd against the distribution's OpenSSL and libcurl (a developer's deps/imd
# may have been configured against a self-built OpenSSL in /usr/local, which must not leak into the
# package). Everything else -- SDL2, X11, GL, OpenSSL, libcurl, zlib -- is the distribution's
# shared library, so the package installs on a stock system. Depends: is derived from the
# binaries' DT_NEEDED entries.
#
# Needs: deps/ffmpeg built (scripts/build-ffmpeg-rvra.sh), deps/lwip, deps/imgui + deps/json
# (scripts/fetch-gui-deps.sh), the libimobiledevice build tools (autoconf automake libtool
# libssl-dev libcurl4-openssl-dev), and dpkg-dev fakeroot lintian. Rebuilds the libimobiledevice
# sources in deps/imd-src from clean, so a deps/imd install made from them stays as it is but the
# trees are reconfigured.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
version="${1:-$(sed -n 's/^RPLAYHUB_VERSION := \([0-9.]*\).*/\1/p' "$root/client-c/Makefile")}"
arch="$(dpkg --print-architecture)"
multiarch="$(dpkg-architecture -qDEB_HOST_MULTIARCH)"
stage="$root/build/deb/stage"
out="$root/build/deb"
die() { echo "package-deb: $*" >&2; exit 1; }
[ -n "$version" ] || die "no version (RPLAYHUB_VERSION in client-c/Makefile, or pass one)"
[ -f "$root/deps/ffmpeg/libavcodec/libavcodec.a" ] || die "deps/ffmpeg not built -- run scripts/build-ffmpeg-rvra.sh"
[ -f "$root/deps/ffmpeg/libswscale/libswscale.a" ] || die "deps/ffmpeg has no libswscale -- rebuild with scripts/build-ffmpeg-rvra.sh"
[ -d "$root/deps/lwip/src" ] || die "deps/lwip missing -- git clone --branch STABLE-2_2_1_RELEASE https://github.com/lwip-tcpip/lwip.git deps/lwip"
[ -f "$root/deps/imgui/imgui.cpp" ] || die "deps/imgui missing -- run scripts/fetch-gui-deps.sh"
for t in dpkg-deb fakeroot lintian objdump strip autoconf automake libtoolize; do command -v "$t" >/dev/null || die "missing: $t (apt install dpkg-dev fakeroot lintian binutils autoconf automake libtool)"; done

# GCC ignores -I/-isystem for directories it already searches by default, so a self-built SDL2 in
# /usr/local/include would shadow the distribution's headers and the binary would reference
# symbols the distribution's libSDL2 does not have. A directory of symlinks to the distribution's
# headers, given as a plain -I, wins the search. pkg-config is likewise confined to the
# distribution's .pc files.
shadow="$root/build/deb/shadow-include"
rm -rf "$shadow"; mkdir -p "$shadow"
[ -d /usr/include/SDL2 ] && ln -s /usr/include/SDL2 "$shadow/SDL2"
# OpenSSL's headers are split between /usr/include/openssl and the multiarch directory
# (opensslconf.h, configuration.h), so the shadow is a merged directory of links, not one link.
mkdir -p "$shadow/openssl"
for d in /usr/include/openssl "/usr/include/$multiarch/openssl"; do
    [ -d "$d" ] && for h in "$d"/*; do ln -sf "$h" "$shadow/openssl/"; done
done
export PKG_CONFIG_LIBDIR="/usr/lib/$multiarch/pkgconfig:/usr/share/pkgconfig"

echo "package-deb: building the libimobiledevice stack against the distribution's OpenSSL and libcurl"
imd_prefix="$root/build/deb/imd"
rm -rf "$imd_prefix"
for d in "$root"/deps/imd-src/*/; do
    [ -f "$d/Makefile" ] && make -C "$d" distclean >/dev/null 2>&1 || true
done
CFLAGS="-I$shadow" CXXFLAGS="-I$shadow" "$root/scripts/build-imd-stack.sh" "$imd_prefix" >"$root/build/deb/imd-build.log" 2>&1 \
    || die "libimobiledevice stack failed to build; see build/deb/imd-build.log"

echo "package-deb: building the GUI against the distribution's SDL2 and the RVRA FFmpeg"
make -C "$root/client-c" clean >/dev/null
CFLAGS="-I$shadow" CXXFLAGS="-L/usr/lib/$multiarch" make -C "$root/client-c" rplay-gui
gui="$root/client-c/rplay-gui"

echo "package-deb: building the engine against the static libimobiledevice stack"
imd="$imd_prefix/lib"
# IMD_PREFIX=/usr keeps the Makefile from baking an rpath to the prefix; the headers come in via
# CFLAGS (the shadow first, so OpenSSL's are the distribution's), and IMD_LIBS on the command line
# replaces the shared-library link with the archives (dependents first). libtatsu needs libcurl,
# libplist needs libm; OpenSSL and zlib follow from the Makefile.
make -C "$root/host-c" clean >/dev/null 2>&1 || true
CFLAGS="-I$shadow -I$imd_prefix/include" make -C "$root/host-c" IMD_PREFIX=/usr \
    IMD_LIBS="$imd/libimobiledevice-1.0.a $imd/libusbmuxd-2.0.a $imd/libimobiledevice-glue-1.0.a $imd/libtatsu.a $imd/libplist-2.0.a -lcurl -lm"
engine="$root/host-c/cdhost"

# ldd as on a user's machine: the distribution's library directories only (this host's ldconfig
# may prefer /usr/local/lib for the same sonames).
distro_ldd() { LD_LIBRARY_PATH="/lib/$multiarch:/usr/lib/$multiarch" ldd "$1"; }
for bin in "$gui" "$engine"; do
    distro_ldd "$bin" | grep -qE "not found|/usr/local/" && die "$bin needs a library outside the distribution:
$(distro_ldd "$bin" | grep -E 'not found|/usr/local/')"
    objdump -p "$bin" | grep -qE 'RPATH|RUNPATH' && die "$bin carries an rpath: $(objdump -p "$bin" | grep -E 'RPATH|RUNPATH')"
done

rm -rf "$stage"
mkdir -p "$stage/DEBIAN" "$stage/usr/bin" "$stage/usr/libexec/rplayhub" "$stage/usr/share/rplayhub/fonts" \
         "$stage/usr/share/applications" "$stage/usr/share/icons/hicolor/256x256/apps" \
         "$stage/usr/share/icons/hicolor/128x128/apps" "$stage/usr/share/doc/rplayhub" \
         "$stage/usr/share/lintian/overrides"
install -m 0755 "$gui" "$stage/usr/bin/rplayhub"
install -m 0755 "$engine" "$stage/usr/libexec/rplayhub/cdhost"
strip --strip-unneeded "$stage/usr/bin/rplayhub" "$stage/usr/libexec/rplayhub/cdhost"
install -m 0755 "$root/scripts/fetch-ddi.sh" "$stage/usr/bin/rplayhub-fetch-ddi"
cat > "$stage/usr/bin/rplayhub-engine" <<'SH'
#!/bin/sh
# rPlayHub engine: talks to the iPhone and serves the GUI on 127.0.0.1:9876 (control) and :9877
# (video). Runs as an ordinary user over its own userspace TCP/IP stack; no root, no TUN.
# Arguments and RPLAY_* variables go through to cdhost (see rplayhub-engine --help).
export RPLAY_USERSPACE_NET="${RPLAY_USERSPACE_NET:-1}"
exec /usr/libexec/rplayhub/cdhost "$@"
SH
chmod 0755 "$stage/usr/bin/rplayhub-engine"
cp "$root/client-c/fonts/"*.ttf "$stage/usr/share/rplayhub/fonts/"
cp "$root/app/rPlayHub/Assets.xcassets/AppIcon.appiconset/icon_256x256.png" "$stage/usr/share/icons/hicolor/256x256/apps/rplayhub.png"
cp "$root/app/rPlayHub/Assets.xcassets/AppIcon.appiconset/icon_128x128.png" "$stage/usr/share/icons/hicolor/128x128/apps/rplayhub.png"
cp "$root/client-c/packaging/rplayhub.desktop" "$stage/usr/share/applications/"
cp "$root/client-c/packaging/copyright" "$stage/usr/share/doc/rplayhub/copyright"
{ echo "rplayhub ($version-1) stable; urgency=medium"; echo; echo "  * See https://github.com/rPlayAI/rPlayHub/releases"; echo; echo " -- rPlayAI <code@rplay.ai>  $(date -R)"; } | gzip -9n > "$stage/usr/share/doc/rplayhub/changelog.Debian.gz"
cat > "$stage/usr/share/lintian/overrides/rplayhub" <<'OVR'
# FFmpeg is linked in on purpose: the GUI needs the RVRA-patched decoder (patches/ffmpeg-rvra.patch),
# which no distribution FFmpeg has. LGPL build, source in the repository's release.
rplayhub: embedded-library libavutil usr/bin/rplayhub
rplayhub: embedded-library libswscale usr/bin/rplayhub
rplayhub: no-manual-page usr/bin/rplayhub
rplayhub: no-manual-page usr/bin/rplayhub-engine
rplayhub: no-manual-page usr/bin/rplayhub-fetch-ddi
OVR
find "$stage/usr" -type d -exec chmod 0755 {} + ; find "$stage/usr" -type f -exec chmod 0644 {} +
chmod 0755 "$stage/usr/bin/"* "$stage/usr/libexec/rplayhub/cdhost"

# Depends: the packages owning the libraries the binaries need (DT_NEEDED, not ldd's transitive
# closure), plus usbmuxd (the USB daemon the engine talks to) and curl (rplayhub-fetch-ddi).
deps="usbmuxd, curl"
for bin in "$stage/usr/bin/rplayhub" "$stage/usr/libexec/rplayhub/cdhost"; do
    for so in $(objdump -p "$bin" | awk '/NEEDED/{print $2}'); do
        # dpkg records libc's files under /lib and most others under /usr/lib (merged /usr).
        pkg="$(dpkg -S "/usr/lib/$multiarch/$so" "/lib/$multiarch/$so" 2>/dev/null | head -1 | cut -d: -f1)" || true
        [ -n "$pkg" ] && deps="$deps, $pkg" || die "no package owns $so"
    done
done
deps="$(echo "$deps" | tr ',' '\n' | sed 's/^ *//' | sort -u | grep -v '^$' | paste -sd, | sed 's/,/, /g')"
size="$(du -sk "$stage/usr" | cut -f1)"
cat > "$stage/DEBIAN/control" <<CTL
Package: rplayhub
Version: $version-1
Section: utils
Priority: optional
Architecture: $arch
Installed-Size: $size
Depends: $deps
Recommends: python3
Maintainer: rPlayAI <code@rplay.ai>
Homepage: https://github.com/rPlayAI/rPlayHub
Description: iPhone screen mirroring and control, a Device Hub for Linux
 Mirrors an iPhone (iOS 27 or later) over USB with the CoreDevice protocol,
 no Xcode and no Apple daemon in the path: live HEVC screen and sound,
 click-to-tap and drag-to-swipe, a pop-out phone window, a 3D device twin,
 and the rest of Device Hub -- apps, profiles, files, crash reports, the
 device's settings and its console. rplayhub is the GUI; rplayhub-engine
 talks to the phone and runs as an ordinary user.
CTL
( cd "$stage" && find usr -type f -exec md5sum {} + > DEBIAN/md5sums )
mkdir -p "$out"
deb="$out/rplayhub_${version}-1_${arch}.deb"
fakeroot dpkg-deb --build --root-owner-group "$stage" "$deb" >/dev/null
echo "package-deb: $deb"
echo "package-deb: Depends: $deps"
lintian --no-tag-display-limit "$deb" 2>&1 | grep -vE "^N:|^$" | head -20 || true
