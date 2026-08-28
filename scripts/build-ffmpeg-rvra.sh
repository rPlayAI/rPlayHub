#!/bin/bash
# Build the RVRA-patched ffmpeg in deps/ffmpeg (deps/ is gitignored; the patch is the artifact).
#
# RVRA is Apple's proprietary in-sequence resolution adaptation: under motion the device's
# encoder codes a smaller picture into the top-left of the same SPS-sized frame, and correct
# decode requires resampling every reference when that size changes -- which standard HEVC (and
# so stock ffmpeg) does not do, producing a garbled mosaic from the first downshift with zero
# warnings. patches/ffmpeg-rvra.patch adds that resampling to ffmpeg's hevc decoder, gated
# behind RPLAY_RVRA=1 and single-threaded decode. See doc/RVRA-AND-PORTABILITY.md.
#
# Usage:   scripts/build-ffmpeg-rvra.sh
# Verify:  RPLAY_RVRA=1 deps/ffmpeg/ffmpeg -threads 1 -r 60 -i reference/captures/apple_video_REFERENCE.h265 \
#              -fps_mode passthrough -strict -1 -f yuv4mpegpipe cand.y4m
#          python3 scripts/compare-decodes.py gt.y4m cand.y4m reference/captures/apple_video_REFERENCE.h265
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION=8.1.2
TARBALL="deps/ffmpeg-$VERSION.tar.xz"

if [[ ! -d deps/ffmpeg ]]; then
    if [[ ! -f "$TARBALL" ]]; then
        echo "fetching ffmpeg $VERSION"
        mkdir -p deps    # gitignored; a fresh clone has no deps/ at all
        curl -fsL -o "$TARBALL" "https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz"
    fi
    tar xf "$TARBALL" -C deps
    mv "deps/ffmpeg-$VERSION" deps/ffmpeg
fi

cd deps/ffmpeg
if ! patch -p1 -N --dry-run -s < ../../patches/ffmpeg-rvra.patch >/dev/null 2>&1; then
    # Already applied (or conflicts). Reverse-check tells them apart.
    if patch -p1 -R --dry-run -s < ../../patches/ffmpeg-rvra.patch >/dev/null 2>&1; then
        echo "patch already applied"
    else
        echo "patch does not apply cleanly to deps/ffmpeg -- wrong version?" >&2
        exit 1
    fi
else
    patch -p1 < ../../patches/ffmpeg-rvra.patch
fi

if [[ ! -f ffbuild/config.mak ]]; then
    ./configure --disable-doc --disable-autodetect --disable-network \
                --disable-ffplay --disable-ffprobe
fi
make -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
echo "built deps/ffmpeg/ffmpeg (use with RPLAY_RVRA=1 -threads 1)"
