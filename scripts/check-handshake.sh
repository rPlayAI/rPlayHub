#!/usr/bin/env bash
#
# Byte-compare the C RemoteXPC opening exchange against the Python.
#
# Against the ACTUAL module, not a reimplementation of it. An earlier version of this check built
# the expected bytes from a hand-written copy of the Python's framing, including its own copy of
# INITIAL_WINDOW -- so it compared the C against my assumptions and passed while the two really
# differed by 15 MB of flow-control window. Importing the module is the whole point.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
make -C core test_rp_remotexpc >/dev/null || exit 1

c=$(./core/test_rp_remotexpc | sed -n '2p' | tr -d ' ')
p=$(cd host && python3 -c "
import sys, struct; sys.path.insert(0,'.')
from rplayhub.wire import remotexpc as R      # the real module, not a copy
from rplayhub.wire import xpc
out  = R.HTTP2_MAGIC
out += R._frame(R.SETTINGS, 0, 0, struct.pack('>HI',0x3,100)+struct.pack('>HI',0x4,R.INITIAL_WINDOW))
out += R._frame(R.WINDOW_UPDATE, 0, 0, struct.pack('>I', R.INITIAL_WINDOW-65535))
out += R._frame(R.HEADERS, R.FLAG_END_HEADERS, R.ROOT)
out += R._frame(R.DATA, 0, R.ROOT, xpc.build_wrapper({}, message_id=0))
out += R._frame(R.HEADERS, R.FLAG_END_HEADERS, R.REPLY)
out += R._frame(R.DATA, 0, R.ROOT, xpc.build_wrapper(None, flags=0x0201))
out += R._frame(R.DATA, 0, R.REPLY, xpc.build_wrapper(None, flags=xpc.F_ALWAYS_SET|xpc.F_INIT_HANDSHAKE))
print(out.hex())
")
if [[ "$c" == "$p" ]]; then
    echo "  handshake identical to the Python module ($(( ${#c} / 2 )) bytes)"
else
    echo "  handshake DIFFERS (C $(( ${#c} / 2 ))B, python $(( ${#p} / 2 ))B)"
    python3 -c "
c='$c'; p='$p'
for i in range(0, min(len(c), len(p)), 2):
    if c[i:i+2] != p[i:i+2]:
        print(f'    first difference at byte {i//2}: C={c[i:i+2]} python={p[i:i+2]}')
        break
"
    exit 1
fi
