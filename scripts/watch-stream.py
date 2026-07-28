#!/usr/bin/env python3
"""Poll the running engine and print what changes, second by second.

    python3 scripts/watch-stream.py [seconds]

Cumulative counters hide the thing we care about. The fault is provoked by one action — opening an
app, a swipe — and what matters is whether packet loss appears at that instant. Deltas make that
visible; a total does not.

Run it, then provoke the fault. A row with `lost` non-zero at the moment the picture breaks means
the corruption is on the receive path and the app never had a chance. All-zero loss through a break
means the bytes arrived intact and the fault is downstream of the engine.
"""
import json
import socket
import sys
import time

HOST, PORT = "127.0.0.1", 9876
FIELDS = ["rtp_packets", "nals", "keyframes", "rtp_lost", "rtp_late", "rtp_duplicates",
          "queue_drops", "ltr_acked"]


def query():
    try:
        s = socket.create_connection((HOST, PORT), timeout=3)
    except OSError:
        return None
    try:
        s.sendall(b'{"id":1,"method":"stream_info","params":{}}\n')
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
        return json.loads(buf.decode()).get("result", {})
    except Exception:
        return None
    finally:
        s.close()


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 40.0
    # Same reasoning as capture-viewer: wait for the engine instead of racing it.
    waited = False
    deadline = time.time() + 180
    while query() is None and time.time() < deadline:
        if not waited:
            print(f"waiting for the engine on {HOST}:{PORT} — start it with sudo ./scripts/live.sh")
            waited = True
        time.sleep(1)
    print("watching — PROVOKE THE FAULT NOW (open the Clock app, swipe)")
    print(f"{'time':>5}  " + "  ".join(f"{f.replace('rtp_', ''):>10}" for f in FIELDS))

    prev = None
    t0 = time.time()
    worst = 0
    while time.time() - t0 < seconds:
        r = query()
        if r is None:
            print("  engine not reachable")
            time.sleep(1)
            continue
        cur = {f: r.get(f, 0) for f in FIELDS}
        if prev is not None:
            d = {f: cur[f] - prev[f] for f in FIELDS}
            flag = ""
            if d["rtp_lost"] or d["queue_drops"] or d["rtp_late"]:
                flag = "   <<< LOSS"
                worst = max(worst, d["rtp_lost"])
            print(f"{time.time() - t0:5.0f}  "
                  + "  ".join(f"{d[f]:>10}" for f in FIELDS) + flag)
        prev = cur
        time.sleep(1.0)

    print()
    if worst:
        print(f"VERDICT: packets were lost on the receive path (worst second: {worst}).")
        print("         The app was handed a stream with holes in it — fix the receive path.")
    else:
        print("VERDICT: no loss at any point. Every packet the device sent was received in order,")
        print("         so garbling is downstream of the engine: assembly, decode or display.")


if __name__ == "__main__":
    main()
