#!/usr/bin/env bash
#
# Build rPlayHub.app and wrap it in a DMG for download from our own site.
#
#   ./scripts/package-dmg.sh                      # ad-hoc signed, for local testing only
#   SIGN_ID="Developer ID Application: Acme (TEAMID)" ./scripts/package-dmg.sh
#   SIGN_ID="…" NOTARY_PROFILE=rplayhub ./scripts/package-dmg.sh   # sign + notarize + staple
#
# Gatekeeper will refuse an unsigned or un-notarized app that a user downloaded, so the
# ad-hoc output is for testing on this machine only. See app/DISTRIBUTION.md.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT="$ROOT/app/rPlayHub.xcodeproj"
BUILD="$ROOT/build"
DD="$BUILD/DerivedData"
STAGE="$BUILD/dmg-stage"
APP_NAME="rPlayHub"

VERSION="$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' \
    "$ROOT/app/$APP_NAME/Info.plist")"
DMG="$BUILD/$APP_NAME-$VERSION.dmg"

SIGN_ID="${SIGN_ID:-}"
NOTARY_PROFILE="${NOTARY_PROFILE:-}"

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }

# ---------------------------------------------------------------- build
say "building $APP_NAME $VERSION (Release)"
rm -rf "$STAGE" "$DMG"
mkdir -p "$STAGE"

if [[ -n "$SIGN_ID" ]]; then
    # Hardened runtime is required for notarization and is already on in the project.
    # CODE_SIGN_INJECT_BASE_ENTITLEMENTS=NO is not optional here. Left at its default, Xcode
    # adds com.apple.security.get-task-allow -- the entitlement that lets a debugger attach --
    # even to a Release build, and the notary service rejects the upload outright for it:
    #   "The executable requests the com.apple.security.get-task-allow entitlement."
    # The .entitlements file never mentions it, so it looks like it comes from nowhere.
    xcodebuild -project "$PROJECT" -scheme "$APP_NAME" -configuration Release \
        -derivedDataPath "$DD" \
        CODE_SIGN_STYLE=Manual \
        CODE_SIGN_IDENTITY="$SIGN_ID" \
        CODE_SIGN_INJECT_BASE_ENTITLEMENTS=NO \
        OTHER_CODE_SIGN_FLAGS="--timestamp --options=runtime" \
        build
else
    say "no SIGN_ID set — building ad-hoc signed (LOCAL TESTING ONLY)"
    # The project uses automatic signing so that opening it in Xcode picks up a real team.
    # A headless build has no team, so override back to manual ad-hoc here.
    xcodebuild -project "$PROJECT" -scheme "$APP_NAME" -configuration Release \
        -derivedDataPath "$DD" \
        CODE_SIGN_STYLE=Manual \
        CODE_SIGN_IDENTITY="-" \
        DEVELOPMENT_TEAM="" \
        PROVISIONING_PROFILE_SPECIFIER="" \
        build
fi

APP="$DD/Build/Products/Release/$APP_NAME.app"
[[ -d "$APP" ]] || { echo "build did not produce $APP" >&2; exit 1; }

# ---------------------------------------------------------------- verify signature
say "verifying signature"
codesign --verify --deep --strict --verbose=2 "$APP" 2>&1 | sed 's/^/    /'
if [[ -n "$SIGN_ID" ]]; then
    # This is the check that actually predicts whether a downloaded copy will launch.
    if spctl --assess --type execute --verbose "$APP" 2>&1 | sed 's/^/    /'; then
        say "Gatekeeper assessment passed"
    else
        say "Gatekeeper assessment failed — notarization is still needed"
    fi
fi

# ---------------------------------------------------------------- stage + dmg
say "staging"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"

say "creating $DMG"
hdiutil create -volname "$APP_NAME $VERSION" \
    -srcfolder "$STAGE" \
    -ov -format UDZO \
    -fs HFS+ \
    "$DMG" >/dev/null

# ---------------------------------------------------------------- notarize
if [[ -n "$NOTARY_PROFILE" ]]; then
    if [[ -z "$SIGN_ID" ]]; then
        echo "refusing to notarize an ad-hoc signed build — set SIGN_ID too" >&2
        exit 1
    fi
    say "signing the DMG"
    codesign --sign "$SIGN_ID" --timestamp "$DMG"

    say "submitting for notarization (this waits for Apple)"
    xcrun notarytool submit "$DMG" --keychain-profile "$NOTARY_PROFILE" --wait

    say "stapling the ticket"
    xcrun stapler staple "$DMG"
    xcrun stapler validate "$DMG"
else
    say "NOTARY_PROFILE not set — skipping notarization"
    say "a user who downloads this DMG will be blocked by Gatekeeper"
fi

rm -rf "$STAGE"
say "done: $DMG"
ls -lh "$DMG"
