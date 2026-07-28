#!/usr/bin/env bash
#
# Byte-compare the C offer builder against the Python one.
#
# The offer is protobuf inside zlib inside a binary plist, and the device rejects anything it does
# not like without saying why -- so "produces a valid plist" is not the bar. These must be the
# same bytes.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

SSRC=725448759
CALL=1E312779-5E86-4742-9215-7522E1EB1610

make -C core test_rp_media_offer >/dev/null || exit 1

fail=0
for what in blob offer; do
    if [[ $what == blob ]]; then
        c=$(./core/test_rp_media_offer b)
        p=$(cd host && python3 -c "import sys;sys.path.insert(0,'.');import screen
print(screen._media_blob_video($SSRC,'auto').hex())")
    else
        c=$(./core/test_rp_media_offer)
        p=$(cd host && python3 -c "import sys;sys.path.insert(0,'.');import screen
print(screen.build_offer('$CALL',$SSRC).hex())")
    fi
    if [[ "$c" == "$p" ]]; then
        printf '  %-6s identical (%s bytes)\n' "$what" "$(( ${#c} / 2 ))"
    else
        printf '  %-6s DIFFERS (C %s bytes, python %s bytes)\n' "$what" "$(( ${#c} / 2 ))" "$(( ${#p} / 2 ))"
        fail=1
    fi
done
exit $fail
