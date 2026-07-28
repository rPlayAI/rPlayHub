#!/bin/sh
# Dump the EXACT HEVC bytes DeviceHub feeds VideoToolbox (ground truth for a correct decode).
# Requires SIP off + DeviceHub running with View Screen open.
#
#   sudo ./run-vt-dump.sh            # then swipe in View Screen a few seconds, Ctrl-C
#   -> writes ~/dh_decoder_input.h265  (Annex-B of exactly what DH decodes)
#
# The per-frame lldb calls are heavy, so DeviceHub will run slowly during the dump — swipe slowly,
# grab ~30-50 frames, stop. Then decode/diff against our network reconstruction:
#   ffmpeg -i ~/dh_decoder_input.h265 -f null -
set -e
PID=$(pgrep -x DeviceHub | head -1)
[ -z "$PID" ] && { echo "DeviceHub not running — open it and View Screen first"; exit 1; }
DIR=$(cd "$(dirname "$0")" && pwd)
export VTDUMP_OUT="${VTDUMP_OUT:-$HOME/dh_decoder_input.h265}"
: > "$VTDUMP_OUT"
LLDB=$(command -v lldb || echo /usr/bin/lldb)

cat > /tmp/vt.lldb <<EOF
process attach --pid $PID
command script import $DIR/dump_vt.py
breakpoint set -n VTDecompressionSessionDecodeFrame
breakpoint command add -F dump_vt.on_decode
continue
EOF

echo "attaching to DeviceHub pid=$PID via $LLDB"
echo "-> swipe/scroll in View Screen (slowly), watch the [vtdump] frame counter, Ctrl-C to stop"
echo "-> output: $VTDUMP_OUT"
exec "$LLDB" -b -s /tmp/vt.lldb
