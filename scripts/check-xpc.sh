#!/usr/bin/env bash
#
# Verify the C XPC encoder byte-for-byte against the Python one.
#
# The Python implementation is verified against a real device, so equality here is the strongest
# check available without a phone: the device silently ignores a dictionary whose padding, key
# alignment or container lengths are wrong, giving no error to debug from.
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

cc -std=c11 -Wall -Wextra -O2 -o build/test_rp_xpc core/test_rp_xpc.c core/rp_xpc.c
C_HEX="$(build/test_rp_xpc 2>/dev/null)"

PY_HEX="$(cd host && python3 -c "
import uuid as U
from rplayhub.wire import xpc
print(xpc.build_wrapper({
  'MessageType': 'Handshake',
  'MessagingProtocolVersion': xpc.U64(7),
  'UUID': U.UUID('12345678-1234-5678-1234-567812345678'),
  'Properties': {'RemoteXPCVersionFlags': xpc.U64(0x0100000000000006),
                 'SensitivePropertiesVisible': True},
  'Services': {},
}, message_id=1).hex())
")"

if [[ "$C_HEX" == "$PY_HEX" ]]; then
    echo "✅ C and Python encoders agree byte-for-byte (${#C_HEX} hex chars)"
    exit 0
fi
echo "❌ encoders differ"
python3 - "$C_HEX" "$PY_HEX" <<'PY'
import sys
c, p = sys.argv[1], sys.argv[2]
print(f"  lengths: C={len(c)//2} PY={len(p)//2} bytes")
for i in range(0, min(len(c), len(p)), 2):
    if c[i:i+2] != p[i:i+2]:
        print(f"  first difference at byte {i//2}: C={c[i:i+2]} PY={p[i:i+2]}")
        print("  C  …", c[max(0,i-20):i+20])
        print("  PY …", p[max(0,i-20):i+20])
        break
PY
exit 1
