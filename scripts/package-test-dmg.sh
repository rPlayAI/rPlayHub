#!/usr/bin/env bash
#
# Build a Developer ID signed DMG of rPlayHub for testing on ANOTHER Mac. The engine (cdhost) is
# EMBEDDED in the app bundle as an SMAppService launchd daemon, so the user drags one app, approves
# the background engine once in System Settings, and never runs sudo. TestFlight is still not an
# option (App Store forbids the root daemon); this is the Developer ID path. See app/DISTRIBUTION.md.
#
#   SIGN_ID="Developer ID Application: VMLite Corporation (NL28FE3UZ7)" ./scripts/package-test-dmg.sh
#
# Then notarize (needs notarytool credentials stored once):
#   xcrun notarytool submit build/rPlayHub-test-<ver>.dmg --keychain-profile <profile> --wait
#   xcrun stapler staple build/rPlayHub-test-<ver>.dmg
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build"; DD="$BUILD/DerivedData"; STAGE="$BUILD/test-dmg-stage"
SIGN_ID="${SIGN_ID:-}"
VER="$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' "$ROOT/app/rPlayHub/Info.plist")"
DMG="$BUILD/rPlayHub-test-$VER.dmg"
APP="$STAGE/rPlayHub.app"
say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
[ -n "$SIGN_ID" ] || { echo "set SIGN_ID to your Developer ID Application identity"; exit 1; }

say "1/6  building rPlayHub.app $VER (Release, hardened runtime)"
rm -rf "$STAGE"; mkdir -p "$STAGE"
xcodebuild -project "$ROOT/app/rPlayHub.xcodeproj" -scheme rPlayHub -configuration Release \
    -derivedDataPath "$DD" CODE_SIGN_STYLE=Manual CODE_SIGN_IDENTITY="$SIGN_ID" \
    OTHER_CODE_SIGN_FLAGS="--timestamp --options=runtime" -quiet
cp -R "$DD/Build/Products/Release/rPlayHub.app" "$APP"

say "2/6  building cdhost (OpenSSL) and embedding it in the bundle"
make -C "$ROOT/host-c" clean >/dev/null; make -C "$ROOT/host-c" STATIC=1 >/dev/null
cp "$ROOT/host-c/cdhost" "$APP/Contents/MacOS/cdhost"
mkdir -p "$APP/Contents/Library/LaunchDaemons"
cp "$ROOT/app/rPlayHub/daemon/com.rplay.rplayhub.engine.plist" "$APP/Contents/Library/LaunchDaemons/"

say "    bundling the iOS DDI so the engine can self-activate after a reboot (no Xcode needed)"
DDI_SRC="${RPLAY_DDI:-/Library/Developer/DeveloperDiskImages/iOS_DDI}"
if [ -f "$DDI_SRC/Restore/BuildManifest.plist" ]; then
    mkdir -p "$APP/Contents/Resources/iOS_DDI"
    cp -R "$DDI_SRC/Restore" "$APP/Contents/Resources/iOS_DDI/"
    # The daemon plist tells cdhost where to find it, since it runs from Contents/MacOS.
    /usr/libexec/PlistBuddy -c "Add :EnvironmentVariables:RPLAY_DDI string Contents/Resources/iOS_DDI"         "$APP/Contents/Library/LaunchDaemons/com.rplay.rplayhub.engine.plist" 2>/dev/null || true
else
    echo "    (no DDI at $DDI_SRC -- skipping; the colleague will need Xcode/Device Hub after a reboot)"
fi

# With STATIC=1 the engine links libimobiledevice + libplist + OpenSSL from .a archives, so it has
# NO non-system dylib dependency (only libc++, libz, libSystem). This loop bundles any Homebrew
# dylib that somehow remains; with the static build it finds none and is a no-op.
say "3/6  bundling any non-system dylibs beside cdhost (none expected -- static build)"
for lib in $(otool -L "$APP/Contents/MacOS/cdhost" | awk '/opt\/homebrew/{print $1}'); do
    base="$(basename "$lib")"
    cp "$lib" "$APP/Contents/MacOS/$base"; chmod 644 "$APP/Contents/MacOS/$base"
    install_name_tool -change "$lib" "@loader_path/$base" "$APP/Contents/MacOS/cdhost"
    for inner in $(otool -L "$APP/Contents/MacOS/$base" | awk '/opt\/homebrew.*crypto/{print $1}'); do
        install_name_tool -change "$inner" "@loader_path/$(basename "$inner")" "$APP/Contents/MacOS/$base"
    done
    install_name_tool -id "@loader_path/$base" "$APP/Contents/MacOS/$base"
done

say "4/6  signing inside-out (dylibs if any, cdhost, then the app bundle)"
for f in "$APP"/Contents/MacOS/*.dylib; do
    [ -e "$f" ] || continue          # static build has no dylibs -- the glob stays literal
    codesign --force --timestamp --options=runtime -s "$SIGN_ID" "$f"
done
codesign --force --timestamp --options=runtime -s "$SIGN_ID" "$APP/Contents/MacOS/cdhost"
codesign --force --timestamp --options=runtime -s "$SIGN_ID" "$APP"
codesign --verify --strict --deep "$APP" && say "    app bundle signature verifies (engine included)"

say "5/6  staging the note"
cp "$ROOT/doc/TEST-ON-ANOTHER-MAC.md" "$STAGE/SETUP.md"

say "6/6  building the DMG"
rm -f "$DMG"
hdiutil create -volname "rPlayHub $VER" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null
codesign --force --timestamp -s "$SIGN_ID" "$DMG"
say "done: $DMG"
echo
echo "Notarize before sharing (Gatekeeper blocks a downloaded un-notarized app):"
echo "  xcrun notarytool submit \"$DMG\" --keychain-profile <profile> --wait"
echo "  xcrun stapler staple \"$DMG\""
