#!/usr/bin/env bash
#
# The bitrate experiment, end to end.
#
#   sudo ./scripts/rvra-bitrate-all.sh
#
# Answers the one question doc/RVRA-AND-PORTABILITY.md leaves open: is the encoder's resolution
# adaptation driven by the rate budget? If it is, asking for more bits keeps the stream at full
# resolution, every frame decodes on a conforming decoder, and the Linux/Windows port needs
# neither a two-tier product nor a reimplementation of Apple's reference resampler. If it is not,
# option 2 in that document -- hide frames below full tier -- is the plan.
#
# Before running: unlock the phone and leave it on the home screen. The swipes page across it, and
# a locked screen produces no motion, which would make every run look adaptation-free.
#
# HOW TO READ THE RESULT. rctl-low is the positive control and must be read first:
#
#   * If rctl-low shows MORE downshifting than baseline, the device is listening to RCTL. Only
#     then does rctl-high showing less mean anything, and only then is bitrate the lever.
#   * If rctl-low looks like baseline, the device ignores RCTL entirely. Every other RCTL row is
#     then uninterpretable -- not evidence that bitrate is irrelevant, evidence that this knob
#     never reached the encoder. ceiling-high, which travels in streamConfig rather than RTCP, is
#     the independent second attempt for exactly that case.
#
# That ordering is the whole point of the design. A bare "we asked for more bits and nothing
# changed" cannot tell a device that refuses from a device that never heard.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

[ "$(id -u)" = "0" ] || { echo "needs sudo: sudo $0" >&2; exit 1; }

OUT="build/rvra-bitrate"
mkdir -p "$OUT"
: > "$OUT/results.jsonl"

run() { ./scripts/rvra-bitrate-run.sh "$@" || echo "   (run '$1' did not complete)"; }

run baseline
run rctl-off      --rctl 0
run rctl-low      --rctl 800000
run rctl-high     --rctl 20000000
run ceiling-high  --max-bitrate 25000000 --min-bitrate 12000000 --rctl 20000000

echo
echo "================================================================"
python3 - "$OUT/results.jsonl" <<'PY'
import json, sys
rows = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
if not rows:
    sys.exit("no runs completed -- see build/rvra-bitrate/*.log")
print(f"{'run':<14} {'phase':<13} {'frames':>6} {'fps':>6} {'Mbps':>6} "
      f"{'changes/s':>9} {'% below full':>12}")
for r in rows:
    for p in r["phases"]:
        if p["phase"] == "still-after":
            continue
        print(f"{r['label']:<14} {p['phase']:<13} {p['frames']:>6} {p['fps']:>6.1f} "
              f"{p['mbps']:>6.2f} {p['changes_per_s']:>9.2f} {p['below_full_pct']:>11.1f}%")
by = {r["label"]: next(p for p in r["phases"] if p["phase"] == "swiping") for r in rows}
base, low = by.get("baseline"), by.get("rctl-low")
print()
if base and low:
    if low["below_full_pct"] > base["below_full_pct"] * 1.5:
        print("POSITIVE CONTROL PASSED: starving the target increased downshifting, so the device")
        print("does act on RCTL. rctl-high and ceiling-high are now readable as real answers.")
    else:
        print("POSITIVE CONTROL FAILED: starving the target to 0.8 Mbps changed nothing, so the")
        print("device is not acting on RCTL at all. Read no conclusion into rctl-high; the only")
        print("row that carries information is ceiling-high, which travels in streamConfig.")
PY
echo "================================================================"
echo "raw: $OUT/results.jsonl   daemon logs: $OUT/*.log"
