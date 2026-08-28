#!/bin/bash
# Build the modern libimobiledevice stack from source into a prefix (default: deps/imd).
#
# The engine needs the MODERN stack: libplist >= 2.3 (the 4-arg plist_from_memory), and
# libimobiledevice 1.4 pulls in libimobiledevice-glue + libtatsu. Distro packages predate all of
# that (Ubuntu 22.04 ships libplist 2.2 / libimobiledevice 1.3, no glue, no tatsu), so on Linux
# the stack is built from pinned release tags -- the same versions homebrew ships on the Mac side.
# deps/ is gitignored; like ffmpeg and lwIP this is regenerated per-host.
#
# Needs: autoconf automake libtool pkg-config libssl-dev libcurl4-openssl-dev
# Usage:  scripts/build-imd-stack.sh [prefix]
# Then:   make -C host-c IMD_PREFIX=<prefix>
set -euo pipefail
cd "$(dirname "$0")/.."
PREFIX="${1:-$PWD/deps/imd}"
mkdir -p deps/imd-src
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

build() {   # build <repo> <tag> [configure args...]
    local name=$1 tag=$2; shift 2
    if [ ! -d "deps/imd-src/$name" ]; then
        git clone --branch "$tag" --depth 1 "https://github.com/libimobiledevice/$name.git" \
            "deps/imd-src/$name"
    fi
    (cd "deps/imd-src/$name" && ./autogen.sh --prefix="$PREFIX" "$@" \
        && make -j"$(nproc)" && make install)
}

build libplist              2.7.0 --without-cython
build libimobiledevice-glue 1.3.2
build libusbmuxd            2.1.1
build libtatsu              1.0.5
build libimobiledevice      1.4.0 --without-cython

echo "installed to $PREFIX"
echo "build the engine with: make -C host-c IMD_PREFIX=$PREFIX"
