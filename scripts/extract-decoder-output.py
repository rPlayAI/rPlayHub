#!/usr/bin/env python3
"""Turn captured decoder OUTPUT into PNGs.

The point of this is narrow and decisive. avconferenced's decoder INPUT reconstructs to a mosaic in
both ffmpeg and VideoToolbox, and Device Hub's window is clean from the same session. Both cannot
be true unless the difference is at or after the decoder's output -- so this shows what Apple's
decoder actually produced.

The pictures are carried in the same ring as everything else, because CoreImage and ImageIO fail
silently inside a launchd daemon (the first attempt wrote nothing and said nothing). Each picture is
a REC_DECODE_OUTPUT header followed by one REC_NOTE per plane.

    python3 scripts/extract-decoder-output.py /tmp/avconferenced.vtc /tmp/apple-out
"""
import os
import struct
import subprocess
import sys

REC_NOTE, REC_DECODE_OUTPUT = 4, 5


def records(path):
    data = open(path, "rb").read()
    if data[:4] != b"VTC1":
        sys.exit("not a vtcapture file")
    off = 4
    while off + 8 <= len(data):
        t, n = struct.unpack_from("<II", data, off)
        off += 8
        if off + n > len(data):
            break
        yield t, data[off:off + n]
        off += n


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "/tmp/avconferenced.vtc"
    dst = sys.argv[2] if len(sys.argv) > 2 else "/tmp/apple-out"
    os.makedirs(dst, exist_ok=True)

    pending = None
    planes = []
    made = 0
    for t, p in records(src):
        if t == REC_DECODE_OUTPUT:
            r = struct.unpack_from("<dIIIII", p, 0)
            hdr = {"t": r[0], "seq": r[1], "w": r[2], "h": r[3], "fmt": r[4], "n": r[5]}
            off = struct.calcsize("<dIIIII")
            hdr["planes"] = []
            for _ in range(hdr["n"]):
                stride, rows = struct.unpack_from("<II", p, off)
                off += 8
                hdr["planes"].append((stride, rows))
            pending, planes = hdr, []
        elif t == REC_NOTE and pending is not None:
            planes.append(p)
            if len(planes) == pending["n"]:
                fourcc = struct.pack(">I", pending["fmt"]).decode("ascii", "replace")
                w, h = pending["w"], pending["h"]
                raw = os.path.join(dst, f"out-{pending['seq']:05d}.raw")
                with open(raw, "wb") as f:
                    for (stride, rows), data in zip(pending["planes"], planes):
                        # Strip row padding so ffmpeg can read a tightly packed plane.
                        bpr = w if len(pending["planes"]) > 1 and planes.index(data) == 0 else \
                              (w if len(pending["planes"]) == 1 else w)
                        for y in range(rows):
                            row = data[y * stride:(y + 1) * stride]
                            f.write(row[:bpr * (2 if (len(pending['planes']) > 1
                                                      and planes.index(data) == 1) else
                                                (4 if len(pending['planes']) == 1 else 1))])
                png = raw.replace(".raw", ".png")
                pix = {"420f": "nv12", "420v": "nv12", "BGRA": "bgra"}.get(fourcc, "nv12")
                subprocess.run(["ffmpeg", "-loglevel", "error", "-f", "rawvideo",
                                "-pix_fmt", pix, "-s", f"{w}x{h}", "-i", raw,
                                "-y", png], check=False)
                if os.path.exists(png) and os.path.getsize(png) > 0:
                    made += 1
                os.remove(raw)
                pending, planes = None, []

    print(f"wrote {made} PNGs to {dst}")
    if not made:
        print("No decoder-output records in this capture -- was it armed with `arm-output`?")


if __name__ == "__main__":
    main()
