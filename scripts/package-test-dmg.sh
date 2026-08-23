#!/usr/bin/env bash
#
# Build a Developer ID signed DMG for testing rPlayHub on ANOTHER Mac, bundling both halves:
# the app (GUI client) and cdhost (the root engine), plus the OpenSSL dylibs cdhost needs so the
# target Mac needs no Homebrew. TestFlight is not possible yet (the engine needs root, which the
# App Store sandbox forbids -- see app/DISTRIBUTION.md); this is the Developer ID path.
#
#   SIGN_ID="Developer ID Application: VMLite Corporation (NL28FE3UZ7)" ./scripts/package-test-dmg.sh
#
# Notarize afterwards (needs your credentials once, stored with notarytool store-credentials):
#   xcrun notarytool submit build/rPlayHub-test-<ver>.dmg --keychain-profile <profile> --wait
#   xcrun stapler staple build/rPlayHub-test-<ver>.dmg
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build"; DD="$BUILD/DerivedData"; STAGE="$BUILD/test-dmg-stage"
SIGN_ID="${SIGN_ID:-}"
VER="$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' "$ROOT/app/rPlayHub/Info.plist")"
DMG="$BUILD/rPlayHub-test-$VER.dmg"
say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
[ -n "$SIGN_ID" ] || { echo "set SIGN_ID to your Developer ID Application identity"; exit 1; }

say "1/6  building rPlayHub.app $VER (Release, hardened runtime)"
rm -rf "$STAGE"; mkdir -p "$STAGE"
xcodebuild -project "$ROOT/app/rPlayHub.xcodeproj" -scheme rPlayHub -configuration Release \
    -derivedDataPath "$DD" CODE_SIGN_STYLE=Manual CODE_SIGN_IDENTITY="$SIGN_ID" \
    OTHER_CODE_SIGN_FLAGS="--timestamp --options=runtime" -quiet
cp -R "$DD/Build/Products/Release/rPlayHub.app" "$STAGE/"

say "2/6  building cdhost (OpenSSL)"
make -C "$ROOT/host-c" clean >/dev/null; make -C "$ROOT/host-c" >/dev/null
mkdir -p "$STAGE/engine"
cp "$ROOT/host-c/cdhost" "$STAGE/engine/"

say "3/6  bundling OpenSSL dylibs beside cdhost (so no Homebrew is needed on the target Mac)"
# Copy each linked dylib and rewrite cdhost to load it from its own directory.
for lib in $(otool -L "$STAGE/engine/cdhost" | awk '/opt\/homebrew.*(ssl|crypto)/{print $1}'); do
    base="$(basename "$lib")"
    cp "$lib" "$STAGE/engine/$base"; chmod 644 "$STAGE/engine/$base"
    install_name_tool -change "$lib" "@loader_path/$base" "$STAGE/engine/cdhost"
    # libssl depends on libcrypto by its absolute path too; rewrite that as well.
    for inner in $(otool -L "$STAGE/engine/$base" | awk '/opt\/homebrew.*crypto/{print $1}'); do
        install_name_tool -change "$inner" "@loader_path/$(basename "$inner")" "$STAGE/engine/$base"
    done
    install_name_tool -id "@loader_path/$base" "$STAGE/engine/$base"
done

say "4/6  signing engine (Developer ID, hardened runtime, timestamp)"
for f in "$STAGE"/engine/*.dylib "$STAGE/engine/cdhost"; do
    codesign --force --timestamp --options=runtime -s "$SIGN_ID" "$f"
done
codesign --verify --strict "$STAGE/engine/cdhost"
otool -L "$STAGE/engine/cdhost" | grep -q "@loader_path" && say "    cdhost is self-contained (loads OpenSSL from its own folder)"

say "5/6  writing SETUP.md"
cp "$ROOT/doc/TEST-ON-ANOTHER-MAC.md" "$STAGE/SETUP.md"

say "6/6  building the DMG"
rm -f "$DMG"
hdiutil create -volname "rPlayHub $VER" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null
codesign --force --timestamp -s "$SIGN_ID" "$DMG"
say "done: $DMG"
echo
echo "Notarize it (once you have notarytool credentials stored):"
echo "  xcrun notarytool submit \"$DMG\" --keychain-profile <profile> --wait"
echo "  xcrun stapler staple \"$DMG\""
