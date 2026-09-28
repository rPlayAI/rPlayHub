#!/usr/bin/env bash
# Download Apple's Developer Disk Image, which the engine mounts on the phone after every reboot.
#
# iOS 17+ drops the DDI on reboot, and the CoreDevice services rPlayHub uses stay quiet until one
# is mounted again. The engine mounts it by itself (host-c/ddi.c): it asks Apple's signing server
# for a ticket bound to your phone, then uploads the image. What it needs from you is the image
# files -- and those are Apple's software, which this repository cannot redistribute.
#
# On a Mac with Xcode (or Device Hub) installed there is nothing to do: the engine finds Xcode's
# copy in /Library/Developer/DeveloperDiskImages/iOS_DDI. Everywhere else -- Linux, a Mac without
# Xcode -- this fetches the same files from doronz88/DeveloperDiskImage, the public mirror that
# pymobiledevice3 downloads from, into the folder the engine looks in:
#
#   ./scripts/fetch-ddi.sh                 # -> ~/.local/share/rplayhub/iOS_DDI
#   ./scripts/fetch-ddi.sh /some/dir       # -> /some/dir, then run the engine with RPLAY_DDI=/some/dir
#
# DDI_REF picks a branch, tag or commit of that repository (default: main).
set -euo pipefail

DEST="${1:-${XDG_DATA_HOME:-$HOME/.local/share}/rplayhub/iOS_DDI}"
REF="${DDI_REF:-main}"
BASE="https://github.com/doronz88/DeveloperDiskImage/raw/$REF/PersonalizedImages/Xcode_iOS_DDI_Personalized"

# The engine reads <dir>/Restore/BuildManifest.plist and takes the image and trust-cache paths
# from the manifest, which in this copy are Image.dmg and Image.dmg.trustcache.
mkdir -p "$DEST/Restore"
for f in BuildManifest.plist Image.dmg Image.dmg.trustcache; do
    echo "fetching $f"
    curl -fL --retry 3 -o "$DEST/Restore/$f.part" "$BASE/$f"
    mv "$DEST/Restore/$f.part" "$DEST/Restore/$f"
done

build=""
if command -v python3 >/dev/null; then
    build=$(python3 -c 'import plistlib,sys; print(plistlib.load(open(sys.argv[1],"rb")).get("ProductBuildVersion",""))' \
            "$DEST/Restore/BuildManifest.plist" 2>/dev/null || true)
fi
echo
echo "DDI ${build:+build $build }in $DEST"
case "$DEST" in
    "${XDG_DATA_HOME:-$HOME/.local/share}/rplayhub/iOS_DDI") echo "the engine finds it there on its own." ;;
    *) echo "run the engine with RPLAY_DDI=$DEST" ;;
esac
echo "keep the phone unlocked when the engine starts: the phone refuses the mount while locked."
