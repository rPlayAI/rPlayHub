#!/bin/bash
set -euo pipefail

export PATH="/ucrt64/bin:$PATH"
source /c/Users/huisi/tools/portable-msvc/msvc-shell.sh

cd /c/devhub/rPlayHub-dev/rPlayHub/deps/ffmpeg

./configure \
    --toolchain=msvc \
    --enable-static \
    --disable-shared \
    --disable-doc \
    --disable-autodetect \
    --disable-network \
    --disable-ffplay \
    --disable-ffprobe \
    --disable-programs \
    --prefix=/c/devhub/rPlayHub-dev/rPlayHub/deps/ffmpeg-dist

make -j"$(nproc)"
make install
echo "Static FFmpeg with RVRA patch built and installed to deps/ffmpeg-dist"
