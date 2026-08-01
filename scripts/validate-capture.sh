#!/usr/bin/env bash
#
# Run every independent check we have against a captured session, whoever the
# receiver was.
#
# The point is comparison. Every capture analysed so far has been Device Hub's,
# and all of them decode to garbled pictures -- which was never the complaint.
# Our own app's stream has never been captured or checked. This runs the same
# battery over either, so "ours differs from theirs" can be established or
# dismissed with numbers rather than by watching two windows on different days.
#
# The strongest check here is the RTP header extension: the device states, in
# every packet, how many packets the access unit contains, plus a global frame
# index. That is the sender's own account of completeness, independent of our
# depacketizer, of the marker bit, and of sequence arithmetic.
#
#   ./scripts/validate-capture.sh <capture.pcap> [label]
#
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

PCAP="${1:?usage: $0 <capture.pcap> [label]}"
LABEL="${2:-$(basename "$PCAP")}"
WORK="build/validate-$(basename "${PCAP%.pcap}")"
mkdir -p "$WORK"

hdr() { printf '\n\033[1m===== %s =====\033[0m\n' "$*"; }

hdr "1. everything in the capture (is anything being ignored?)"
python3 scripts/pcap-census.py "$PCAP" 2>&1 | sed -n '2,40p'

hdr "2. the sender's own frame-completeness signal (RTP header extension)"
python3 scripts/rtp-extension.py "$PCAP" 2>&1 | sed -n '/frame index runs/,/differs between/p'

hdr "3. depacketize with the shipped C"
[ -x build/pcapreplay ] || clang -O2 -o build/pcapreplay tools/pcapreplay/pcapreplay.c \
    core/rp_rtp.c core/rp_rtp_assembler.c
./build/pcapreplay "$PCAP" "$WORK/video.h265" --stats 2>&1 | tail -3

hdr "4. cross-check with an independent depacketizer"
python3 scripts/rtp-depacketize.py "$PCAP" "$WORK/video-py.h265" 2>&1 | tail -3
if cmp -s "$WORK/video.h265" "$WORK/video-py.h265"; then
    echo "  -> byte-identical to the shipped C"
else
    echo "  -> DIFFERS from the shipped C  <<< that would be the bug"
    cmp "$WORK/video.h265" "$WORK/video-py.h265" | head -3
fi

hdr "5. bitstream structure (POC continuity, dangling references, LTR)"
python3 scripts/hevc-refs.py "$WORK/video.h265" 2>&1 | sed -n '2,12p'

hdr "6. decoder's own verdict"
ERR=$(ffmpeg -loglevel error -i "$WORK/video.h265" -f null - 2>&1 | head -5)
[ -z "$ERR" ] && echo "  ffmpeg: zero decode errors" || { echo "  ffmpeg reported:"; echo "$ERR" | sed 's/^/    /'; }

hdr "7. how the encoder spent its bits"
python3 - "$WORK/video.h265" "$LABEL" <<'PY'
import importlib.util, statistics as st, sys
spec = importlib.util.spec_from_file_location("h","scripts/hevc-refs.py")
H = importlib.util.module_from_spec(spec); spec.loader.exec_module(H)
data = open(sys.argv[1],"rb").read()
sizes, cur = [], 0
for t,_,pl in H.annexb_nals(data):
    if H.is_vcl(t):
        if cur: sizes.append(cur)
        cur = len(pl)+2
    elif cur: cur += len(pl)+2
if cur: sizes.append(cur)
p = sizes[1:]
if p:
    print(f"  {sys.argv[2]}: {len(sizes)} pictures, IDR {sizes[0]:,} B")
    print(f"  P-frames: mean {st.mean(p):,.0f} B  median {st.median(p):,.0f} B  "
          f"p95 {sorted(p)[int(len(p)*.95)]:,} B  peak {max(p):,} B")
    tiny = sum(1 for x in p if x < 2000)
    print(f"  P-frames under 2 kB (encoder emitting almost nothing): {tiny} "
          f"({100*tiny/len(p):.0f}%)")
PY

hdr "8. frames, to look at"
mkdir -p "$WORK/decoded"
ffmpeg -loglevel error -i "$WORK/video.h265" "$WORK/decoded/f-%04d.png" 2>/dev/null
echo "  $(ls "$WORK/decoded" | wc -l | tr -d ' ') PNGs in $WORK/decoded/"
echo
echo "Compare this against the other side's capture. The rows that matter:"
echo "  * extension disagreements  -- non-zero means frames arrive incomplete"
echo "  * depacketizer cross-check -- a difference means our assembly is wrong"
echo "  * P-frames under 2 kB      -- a high share means the encoder is starved"
