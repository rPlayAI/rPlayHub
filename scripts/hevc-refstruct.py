#!/usr/bin/env python3
"""Show which pictures each picture predicts from.

hevc-refs.py established that this encoder never signals long-term reference
pictures, so LTR-ACK cannot be doing what doc/DEVICEHUB-CAPTURE-FINDINGS.md
says it does. That leaves a question with teeth: does the encoder still adapt
its choice of reference to the receiver's feedback, using short-term references
that reach further back than the previous picture?

If every picture predicts from the one before it, the stream is a plain IPPP
chain with no error resilience and feedback cannot influence it. If some
pictures reach further back, the encoder IS choosing references based on
something -- and what we acknowledge then decides whether those references are
pictures we actually hold.

    python3 scripts/hevc-refstruct.py build/devicehub-recording.h265 ours.h265
"""
import sys
from collections import Counter

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import importlib
refs = importlib.import_module("hevc-refs".replace("-", "_")) if False else None

# Import the parser from the sibling script, whose name has a hyphen.
import importlib.util
_spec = importlib.util.spec_from_file_location(
    "hevc_refs", __file__.rsplit("/", 1)[0] + "/hevc-refs.py")
H = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(H)


def structure(path):
    with open(path, "rb") as f:
        data = f.read()

    sps_map, pps_map = {}, {}
    prev_lsb = prev_msb = 0
    pictures = 0
    deltas = Counter()          # reference delta -> count
    per_pic = []
    nrefs = Counter()

    for nal_type, tid, payload in H.annexb_nals(data):
        if nal_type == H.SPS_NUT:
            try:
                s = H.parse_sps(H.unescape(payload))
                sps_map[s["id"]] = s
            except Exception:
                pass
            continue
        if nal_type == H.PPS_NUT:
            try:
                p = H.parse_pps(H.unescape(payload))
                pps_map[p["id"]] = p
            except Exception:
                pass
            continue
        if not H.is_vcl(nal_type):
            continue
        try:
            sh = H.parse_slice(H.unescape(payload), nal_type, sps_map, pps_map)
        except Exception:
            continue
        if not sh or sh.get("dependent") or not sh["first"]:
            continue

        sps = sh["sps"]
        max_lsb = 1 << sps["log2_max_poc_lsb"]
        if H.is_idr(nal_type):
            poc = 0
            prev_lsb = prev_msb = 0
        else:
            lsb = sh["poc_lsb"] or 0
            if lsb < prev_lsb and (prev_lsb - lsb) >= max_lsb // 2:
                msb = prev_msb + max_lsb
            elif lsb > prev_lsb and (lsb - prev_lsb) > max_lsb // 2:
                msb = prev_msb - max_lsb
            else:
                msb = prev_msb
            poc = msb + lsb

        used = []
        if sh["st"]:
            for d, u in sh["st"]["neg"] + sh["st"]["pos"]:
                if u:
                    used.append(d)
                    deltas[d] += 1
        nrefs[len(used)] += 1
        per_pic.append((pictures, poc, H.NAL_NAMES.get(nal_type, nal_type), sorted(used)))

        if tid == 0 and not H.is_sub_layer_non_ref(nal_type):
            prev_lsb = sh["poc_lsb"] or 0
            prev_msb = poc - prev_lsb
        pictures += 1

    return {"path": path, "pictures": pictures, "deltas": deltas,
            "per_pic": per_pic, "nrefs": nrefs, "sps": sps_map}


def main():
    for path in sys.argv[1:]:
        r = structure(path)
        print(f"\n=== {r['path']} ===")
        print(f"coded pictures: {r['pictures']}")
        for sid, s in sorted(r["sps"].items()):
            print(f"SPS #{sid}: {s['num_short_term_ref_pic_sets']} short-term "
                  f"ref pic sets declared:")
            for i, st in enumerate(s["st_sets"]):
                neg = ", ".join(f"{d}{'*' if u else ''}" for d, u in st["neg"])
                pos = ", ".join(f"+{d}{'*' if u else ''}" for d, u in st["pos"])
                print(f"    [{i:>2}] neg=[{neg}] pos=[{pos}]")
            print("         (* = used_by_curr_pic)")

        print("\nreference deltas actually used, over the whole stream:")
        total = sum(r["deltas"].values())
        for d, n in sorted(r["deltas"].items(), reverse=True):
            bar = "#" * max(1, int(60 * n / total))
            print(f"  delta {d:>5}: {n:>6}  {bar}")
        print(f"\nreferences per picture: {dict(sorted(r['nrefs'].items()))}")

        odd = [p for p in r["per_pic"] if p[3] and p[3] != [-1]]
        print(f"pictures predicting from something other than the previous "
              f"picture: {len(odd)} of {r['pictures']}")
        for pic, poc, name, used in odd[:20]:
            print(f"    picture {pic:>4} POC {poc:>6} {name:<10} refs {used}")


if __name__ == "__main__":
    main()
