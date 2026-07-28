#!/usr/bin/env python3
"""Capture exactly what a viewer receives from a running engine, then say whether it is sound.

    python3 scripts/capture-viewer.py [seconds] [output]

Connects to the live stream port as an ordinary viewer — the same fan-out path the app uses — and
records the bytes verbatim. That is the point: the engine's own Recorder writes to a file from the
NALs it has already depacketized, so a clean recording proves only that depacketization worked. It
says nothing about what actually reached the socket. This does.

Use it while provoking the fault (swipe, scroll, play video). Then:

    ./build/decodecheck <output> 4096

which runs the app's real parser and decoder over the capture and writes PNGs, so the frames can
be looked at rather than reasoned about.
"""
import collections
import socket
import sys
import time

HOST, PORT = "127.0.0.1", 9877


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 12.0
    out = sys.argv[2] if len(sys.argv) > 2 else "build/viewer-capture.h265"

    # Wait for the engine rather than demanding it already be up. Coordinating "start the engine,
    # now tell me, now I start capturing" across two people costs a round trip every attempt and
    # loses the run when the timing slips — which it did.
    s = None
    deadline = time.time() + 180
    announced = False
    while time.time() < deadline:
        try:
            s = socket.create_connection((HOST, PORT), timeout=5)
            break
        except OSError:
            if not announced:
                print(f"waiting for the engine on {HOST}:{PORT} — start it with sudo ./scripts/live.sh")
                announced = True
            time.sleep(1)
    if s is None:
        raise SystemExit("engine never appeared")
    s.settimeout(5)

    print(f"capturing {seconds:g}s from {HOST}:{PORT} — PROVOKE THE FAULT NOW (swipe, scroll)")
    chunks, t0, last = [], time.time(), 0
    while time.time() - t0 < seconds:
        try:
            chunk = s.recv(1 << 16)
        except socket.timeout:
            break
        if not chunk:
            break
        chunks.append(chunk)
        elapsed = int(time.time() - t0)
        if elapsed != last:
            last = elapsed
            print(f"\r  {elapsed}s  {sum(len(c) for c in chunks) / 1e6:.1f} MB", end="", flush=True)
    s.close()
    print()

    data = b"".join(chunks)
    with open(out, "wb") as f:
        f.write(data)

    nals = [n for n in data.split(b"\x00\x00\x00\x01")[1:] if n]
    types = collections.Counter((n[0] >> 1) & 0x3F for n in nals)
    names = {32: "VPS", 33: "SPS", 34: "PPS", 19: "IDR_W_RADL", 20: "IDR_N_LP", 21: "CRA",
             1: "TRAIL_R", 0: "TRAIL_N", 39: "SEI", 48: "AP", 49: "FU"}
    print(f"wrote {out}: {len(data)} bytes, {len(nals)} NALs")
    for t, n in sorted(types.items()):
        print(f"    type {t:>2} x{n:<6} {names.get(t, '?')}")

    # Sizes matter here: the fault only appears once frames get big, so a capture that contains
    # nothing large did not reproduce it and any verdict from it is worthless.
    vcl = [len(n) for n in nals if ((n[0] >> 1) & 0x3F) < 32]
    if vcl:
        vcl.sort()
        print(f"    frame NAL sizes: min {vcl[0]}  median {vcl[len(vcl) // 2]}  max {vcl[-1]}")
        if vcl[-1] < 20000:
            print("    !! nothing large in this capture — the swipe probably was not recorded;"
                  " run again and swipe throughout")
    if any(t in types for t in (48, 49)):
        print("    !! AP/FU packetization headers reached the stream — depacketization is broken")
    print(f"    parameter sets: {'yes' if all(t in types for t in (32, 33, 34)) else 'NO'}"
          f"   keyframe: {'yes' if any(16 <= t <= 23 for t in types) else 'NO'}")


if __name__ == "__main__":
    main()
