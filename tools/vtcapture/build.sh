#!/bin/sh
# Build the vtcapture interposer.
#
# Device Hub is arm64e, so build arm64e as well as arm64 — an arm64e process
# will load either, but matching the slice keeps pointer authentication out of
# the picture entirely.
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$DIR/vtcapture.dylib}"

clang -dynamiclib -O2 -Wall -Wextra -Wno-unused-parameter \
    -arch arm64 -arch arm64e \
    -o "$OUT" "$DIR/vtcapture.m" \
    -framework CoreFoundation -framework CoreMedia -framework VideoToolbox \
    -framework CoreVideo -framework CoreImage -framework ImageIO -framework CoreGraphics -fobjc-arc

codesign -f -s - "$OUT"
echo "built $OUT"
lipo -archs "$OUT"
