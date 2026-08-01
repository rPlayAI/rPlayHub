#!/usr/bin/env bash
#
# Capture what avconferenced feeds its video decoder.
#
# avconferenced is the receiver, not Device Hub. With mirroring live, Device Hub did 225 kB of I/O
# and made zero VideoToolbox calls, while avconferenced held both RTP sockets on the tunnel and
# read 4.59 MB in 5,722 recvmsg calls. It has AVConference loaded 41 times, VideoToolbox 17, and
# AppleVideoDecoder.bundle resident. Device Hub only configures the session over XPC.
#
# Every previous attempt to observe "avconference's decoder" targeted Device Hub and therefore
# measured nothing -- both the vtcapture dylib and the first dtrace probe. This targets the process
# that actually does the work.
#
# It is an on-demand LaunchAgent (com.apple.videoconference.camera) in the USER domain, so this
# needs no sudo and never touches the sealed system volume. The environment is set on the user's
# launchd domain, the daemon is torn down, and launchd respawns it with the interposer when the
# next session starts.
#
#   ./scripts/hook-avconferenced.sh arm      then open View Screen and swipe
#   ./scripts/hook-avconferenced.sh disarm   ALWAYS run this afterwards
#   ./scripts/hook-avconferenced.sh decode
#
# avconferenced also serves FaceTime. Disarm when finished. If anything goes wrong, `disarm`
# restores it, and a reboot clears the launchd environment regardless.
#
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

LABEL="gui/$(id -u)/com.apple.videoconference.camera"
DYLIB="$ROOT/tools/vtcapture/vtcapture.dylib"
OUT="/tmp/avconferenced.vtc"

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }

MODE="${1:-arm}"
if [ "$MODE" = "arm-output" ]; then WANT_OUTPUT=1; MODE=arm; fi

case "$MODE" in
arm)
    [ -f "$DYLIB" ] || { say "building the interposer"; ./tools/vtcapture/build.sh >/dev/null; }
    rm -f "$OUT"
    # VTCAP_PROC gates the constructor, so every other process that inherits the variable loads
    # the dylib and immediately does nothing.
    launchctl setenv DYLD_INSERT_LIBRARIES "$DYLIB"
    launchctl setenv VTCAP_OUT "$OUT"
    launchctl setenv VTCAP_PROC avconferenced
    # Capture the decoder's OUTPUT as well as its input. This one replaces a function pointer
    # rather than merely observing, so it is opt-in: ./hook-avconferenced.sh arm-output
    if [ "${WANT_OUTPUT:-0}" = "1" ]; then
        rm -rf /tmp/avc-output
        launchctl setenv VTCAP_HOOK_OUTPUT 1
        launchctl setenv VTCAP_OUT_FRAMES /tmp/avc-output
    else
        launchctl unsetenv VTCAP_HOOK_OUTPUT
        launchctl unsetenv VTCAP_OUT_FRAMES
    fi
    say "environment armed on the user launchd domain"
    # kickstart -k restarts the job in place. `bootout` UNLOADS it, and an on-demand agent that
    # has been unloaded never respawns -- Device Hub then hangs on "Connecting to display" with no
    # error anywhere, which cost a round here.
    launchctl kickstart -k "$LABEL" 2>/dev/null \
        && say "avconferenced restarted with the interposer" \
        || say "avconferenced idle; it will spawn with the interposer on the next session"
    cat <<EOF

  NOW: in Device Hub, open View Screen and swipe for ~20 s.
       launchd respawns avconferenced with the interposer loaded.

  THEN: ./scripts/hook-avconferenced.sh disarm
        ./scripts/hook-avconferenced.sh decode
EOF
    ;;
disarm)
    launchctl unsetenv DYLD_INSERT_LIBRARIES
    launchctl unsetenv VTCAP_OUT
    launchctl unsetenv VTCAP_PROC
    launchctl unsetenv VTCAP_HOOK_OUTPUT
    launchctl unsetenv VTCAP_OUT_FRAMES
    launchctl kickstart -k "$LABEL" 2>/dev/null
    # If a previous run left the agent unloaded, put it back.
    launchctl print "$LABEL" >/dev/null 2>&1 || \
        launchctl bootstrap "gui/$(id -u)" /System/Library/LaunchAgents/com.apple.avconferenced.plist 2>/dev/null
    say "environment cleared and avconferenced restarted clean"
    ;;
decode)
    [ -s "$OUT" ] || { say "no capture at $OUT -- was the mirror live while armed?"; exit 1; }
    ls -la "$OUT"
    python3 scripts/decode-vtcap.py "$OUT" --frames 25
    say "to compare against our own depacketization of the same session:"
    echo "    python3 scripts/decode-vtcap.py $OUT --annexb /tmp/avc-theirs.h265"
    echo "    # then diff /tmp/avc-theirs.h265 against build/validate-ours-*/video.h265"
    ;;
*)
    echo "usage: $0 {arm|arm-output|disarm|decode}" >&2; exit 2 ;;
esac
