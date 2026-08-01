#!/usr/bin/env python3
"""Parse HEVC reference-picture structure, and find references to pictures that
are not there.

Why this exists. The stream this project receives is coded with long-term
reference pictures (`ltrpEnabled=1` in the offer), and the receiver tells the
encoder which frames it may use as long-term references by acknowledging them
over RTCP. If the encoder then predicts from a picture the decoder does not
hold, the result is exactly the symptom in doc/RENDERING-HANDOFF.md: the
bitstream arrives complete, every packet accounted for, the decoder reports no
failure -- and the picture is wrong until the next IDR. A decoder is not
required to complain about a missing reference. It just predicts from whatever
is in that slot.

So counters cannot find this class of fault, and neither can a second
depacketizer: both agree the bytes arrived. The only thing that can find it is
reading what the slices actually reference.

    python3 scripts/hevc-refs.py build/devicehub-recording.h265
    python3 scripts/hevc-refs.py ours.h265 --frames 40

Parses SPS, PPS and slice segment headers as far as the reference picture sets
(ITU-T H.265 7.3.2.2, 7.3.6.1, 7.4.8), derives POC (8.3.1), tracks the set of
pictures a decoder would hold, and reports any short-term or long-term
reference that is absent from it.
"""
import argparse
import sys
from collections import Counter

# NAL unit types we care about by name.
TRAIL_N, TRAIL_R = 0, 1
TSA_N, TSA_R, STSA_N, STSA_R = 2, 3, 4, 5
RADL_N, RADL_R, RASL_N, RASL_R = 6, 7, 8, 9
BLA_W_LP, BLA_W_RADL, BLA_N_LP = 16, 17, 18
IDR_W_RADL, IDR_N_LP, CRA_NUT = 19, 20, 21
VPS_NUT, SPS_NUT, PPS_NUT, AUD_NUT = 32, 33, 34, 35
EOS_NUT, EOB_NUT, FD_NUT = 36, 37, 38
PREFIX_SEI, SUFFIX_SEI = 39, 40

NAL_NAMES = {
    0: "TRAIL_N", 1: "TRAIL_R", 2: "TSA_N", 3: "TSA_R", 4: "STSA_N", 5: "STSA_R",
    6: "RADL_N", 7: "RADL_R", 8: "RASL_N", 9: "RASL_R",
    16: "BLA_W_LP", 17: "BLA_W_RADL", 18: "BLA_N_LP",
    19: "IDR_W_RADL", 20: "IDR_N_LP", 21: "CRA_NUT",
    32: "VPS", 33: "SPS", 34: "PPS", 35: "AUD", 36: "EOS", 37: "EOB", 38: "FD",
    39: "PREFIX_SEI", 40: "SUFFIX_SEI",
}

SLICE_TYPES = {0: "B", 1: "P", 2: "I"}


def is_irap(t):
    return BLA_W_LP <= t <= 23


def is_idr(t):
    return t in (IDR_W_RADL, IDR_N_LP)


def is_rasl(t):
    return t in (RASL_N, RASL_R)


def is_radl(t):
    return t in (RADL_N, RADL_R)


def is_sub_layer_non_ref(t):
    return t in (TRAIL_N, TSA_N, STSA_N, RADL_N, RASL_N, 10, 12, 14)


def is_vcl(t):
    return t <= 31


class Bits:
    """RBSP bit reader. The caller supplies bytes with emulation prevention
    already removed."""

    def __init__(self, data):
        self.d = data
        self.pos = 0  # in bits

    def u(self, n):
        v = 0
        for _ in range(n):
            byte = self.pos >> 3
            if byte >= len(self.d):
                raise EOFError("past end of RBSP")
            bit = (self.d[byte] >> (7 - (self.pos & 7))) & 1
            v = (v << 1) | bit
            self.pos += 1
        return v

    def ue(self):
        # Exp-Golomb: count leading zeros, then read that many bits.
        zeros = 0
        while self.u(1) == 0:
            zeros += 1
            if zeros > 32:
                raise ValueError("bad exp-golomb")
        return (1 << zeros) - 1 + (self.u(zeros) if zeros else 0)

    def se(self):
        k = self.ue()
        return (k + 1) // 2 if k % 2 else -(k // 2)


def unescape(data):
    """Strip emulation prevention bytes (00 00 03 -> 00 00)."""
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        if i + 2 < n and data[i] == 0 and data[i + 1] == 0 and data[i + 2] == 3:
            out += data[i:i + 2]
            i += 3
        else:
            out.append(data[i])
            i += 1
    return bytes(out)


def annexb_nals(data):
    """Yield (nal_type, temporal_id, payload_without_header) for each NAL."""
    i, n = 0, len(data)
    starts = []
    while i + 3 <= n:
        if data[i] == 0 and data[i + 1] == 0:
            if i + 2 < n and data[i + 2] == 1:
                starts.append((i + 3, 3))
                i += 3
                continue
            if i + 3 < n and data[i + 2] == 0 and data[i + 3] == 1:
                starts.append((i + 4, 4))
                i += 4
                continue
        i += 1
    for idx, (s, _) in enumerate(starts):
        e = starts[idx + 1][0] - starts[idx + 1][1] if idx + 1 < len(starts) else n
        nal = data[s:e]
        if len(nal) < 2:
            continue
        t = (nal[0] >> 1) & 0x3F
        tid = (nal[1] & 0x07) - 1
        yield t, tid, nal[2:]


# ---------------------------------------------------------------- parameter sets

def parse_ptl(b, max_sub_layers_minus1):
    b.u(2); b.u(1); b.u(5)          # general_profile_space/tier/idc
    b.u(32)                          # compatibility flags
    b.u(48)                          # progressive/interlaced/... + reserved
    b.u(8)                           # general_level_idc
    sub_profile, sub_level = [], []
    for _ in range(max_sub_layers_minus1):
        sub_profile.append(b.u(1))
        sub_level.append(b.u(1))
    if max_sub_layers_minus1 > 0:
        for _ in range(max_sub_layers_minus1, 8):
            b.u(2)
    for i in range(max_sub_layers_minus1):
        if sub_profile[i]:
            b.u(2); b.u(1); b.u(5); b.u(32); b.u(48)
        if sub_level[i]:
            b.u(8)


def parse_st_ref_pic_set(b, idx, num_sets, sets):
    """7.3.7. Returns a dict with the derived delta POC lists."""
    inter_pred = b.u(1) if idx != 0 else 0
    if inter_pred:
        delta_idx_minus1 = b.ue() if idx == num_sets else 0
        delta_rps_sign = b.u(1)
        abs_delta_rps_minus1 = b.ue()
        ref_idx = idx - (delta_idx_minus1 + 1)
        ref = sets[ref_idx]
        delta_rps = (1 - 2 * delta_rps_sign) * (abs_delta_rps_minus1 + 1)
        num_delta = len(ref["neg"]) + len(ref["pos"])
        used, use_delta = [], []
        for _ in range(num_delta + 1):
            u = b.u(1)
            used.append(u)
            use_delta.append(1 if u else b.u(1))
        # Derivation of the new set from the reference set (7.4.8).
        neg, pos = [], []
        ref_neg = [p for p, _ in ref["neg"]]
        ref_pos = [p for p, _ in ref["pos"]]
        n_neg, n_pos = len(ref_neg), len(ref_pos)
        for j in range(n_pos - 1, -1, -1):
            d = ref_pos[j] + delta_rps
            if d < 0 and use_delta[n_neg + j]:
                neg.append((d, used[n_neg + j]))
        if delta_rps < 0 and use_delta[n_neg + n_pos]:
            neg.append((delta_rps, used[n_neg + n_pos]))
        for j in range(n_neg):
            d = ref_neg[j] + delta_rps
            if d < 0 and use_delta[j]:
                neg.append((d, used[j]))
        for j in range(n_neg - 1, -1, -1):
            d = ref_neg[j] + delta_rps
            if d > 0 and use_delta[j]:
                pos.append((d, used[j]))
        if delta_rps > 0 and use_delta[n_neg + n_pos]:
            pos.append((delta_rps, used[n_neg + n_pos]))
        for j in range(n_pos):
            d = ref_pos[j] + delta_rps
            if d > 0 and use_delta[n_neg + j]:
                pos.append((d, used[n_neg + j]))
        return {"neg": neg, "pos": pos}

    num_negative = b.ue()
    num_positive = b.ue()
    neg, pos, acc = [], [], 0
    for _ in range(num_negative):
        acc -= b.ue() + 1
        neg.append((acc, b.u(1)))
    acc = 0
    for _ in range(num_positive):
        acc += b.ue() + 1
        pos.append((acc, b.u(1)))
    return {"neg": neg, "pos": pos}


def parse_sps(rbsp):
    b = Bits(rbsp)
    sps = {}
    b.u(4)                                        # sps_video_parameter_set_id
    max_sub = b.u(3)
    b.u(1)                                        # sps_temporal_id_nesting_flag
    parse_ptl(b, max_sub)
    sps["id"] = b.ue()
    chroma = b.ue()
    sps["separate_colour_plane_flag"] = b.u(1) if chroma == 3 else 0
    width = b.ue()
    height = b.ue()
    sps["width"], sps["height"] = width, height
    if b.u(1):                                    # conformance_window_flag
        b.ue(); b.ue(); b.ue(); b.ue()
    b.ue(); b.ue()                                # bit_depth_luma/chroma_minus8
    sps["log2_max_poc_lsb"] = b.ue() + 4
    sub_layer_ordering = b.u(1)
    for _ in range(0 if sub_layer_ordering else max_sub, max_sub + 1):
        b.ue(); b.ue(); b.ue()
    log2_min_cb = b.ue() + 3
    log2_diff_max_min_cb = b.ue()
    sps["ctb_log2"] = log2_min_cb + log2_diff_max_min_cb
    b.ue(); b.ue(); b.ue()                        # min/max transform, max_transform_hierarchy_depth_inter
    b.ue()                                        # ..._intra
    if b.u(1):                                    # scaling_list_enabled_flag
        if b.u(1):                                # sps_scaling_list_data_present_flag
            skip_scaling_list(b)
    b.u(1)                                        # amp_enabled_flag
    b.u(1)                                        # sample_adaptive_offset_enabled_flag
    if b.u(1):                                    # pcm_enabled_flag
        b.u(4); b.u(4); b.ue(); b.ue(); b.u(1)
    num_st = b.ue()
    sets = []
    for i in range(num_st):
        sets.append(parse_st_ref_pic_set(b, i, num_st, sets))
    sps["num_short_term_ref_pic_sets"] = num_st
    sps["st_sets"] = sets
    sps["long_term_ref_pics_present_flag"] = b.u(1)
    sps["num_long_term_ref_pics_sps"] = 0
    sps["lt_ref_poc_lsb_sps"] = []
    sps["used_by_curr_pic_lt_sps_flag"] = []
    if sps["long_term_ref_pics_present_flag"]:
        n = b.ue()
        sps["num_long_term_ref_pics_sps"] = n
        for _ in range(n):
            sps["lt_ref_poc_lsb_sps"].append(b.u(sps["log2_max_poc_lsb"]))
            sps["used_by_curr_pic_lt_sps_flag"].append(b.u(1))
    sps["temporal_mvp_enabled_flag"] = b.u(1)
    return sps


def skip_scaling_list(b):
    for size_id in range(4):
        step = 3 if size_id == 3 else 1
        for _ in range(0, 6, step):
            if not b.u(1):
                b.ue()
            else:
                coef = min(64, 1 << (4 + (size_id << 1)))
                if size_id > 1:
                    b.se()
                for _ in range(coef):
                    b.se()


def parse_pps(rbsp):
    b = Bits(rbsp)
    pps = {}
    pps["id"] = b.ue()
    pps["sps_id"] = b.ue()
    pps["dependent_slice_segments_enabled_flag"] = b.u(1)
    pps["output_flag_present_flag"] = b.u(1)
    pps["num_extra_slice_header_bits"] = b.u(3)
    return pps


# ---------------------------------------------------------------- slice header

def parse_slice(rbsp, nal_type, sps_map, pps_map):
    b = Bits(rbsp)
    sh = {}
    first = b.u(1)
    if is_irap(nal_type):
        b.u(1)                                    # no_output_of_prior_pics_flag
    pps_id = b.ue()
    pps = pps_map.get(pps_id)
    if pps is None:
        return None
    sps = sps_map.get(pps["sps_id"])
    if sps is None:
        return None
    sh["first"] = first
    dependent = 0
    if not first:
        if pps["dependent_slice_segments_enabled_flag"]:
            dependent = b.u(1)
        ctb_log2 = sps["ctb_log2"]
        w = (sps["width"] + (1 << ctb_log2) - 1) >> ctb_log2
        h = (sps["height"] + (1 << ctb_log2) - 1) >> ctb_log2
        size = max(1, w * h)
        bits = (size - 1).bit_length()
        if bits:
            b.u(bits)
    sh["dependent"] = dependent
    if dependent:
        return sh                                  # carries no reference info
    for _ in range(pps["num_extra_slice_header_bits"]):
        b.u(1)
    sh["slice_type"] = b.ue()
    if pps["output_flag_present_flag"]:
        b.u(1)
    if sps["separate_colour_plane_flag"]:
        b.u(2)

    sh["poc_lsb"] = None
    sh["st"] = None
    sh["lt"] = []
    if not is_idr(nal_type):
        sh["poc_lsb"] = b.u(sps["log2_max_poc_lsb"])
        if b.u(1):                                 # short_term_ref_pic_set_sps_flag
            n = sps["num_short_term_ref_pic_sets"]
            idx = 0
            if n > 1:
                idx = b.u(max(1, (n - 1).bit_length()))
            sh["st"] = sps["st_sets"][idx] if idx < len(sps["st_sets"]) else None
        else:
            n = sps["num_short_term_ref_pic_sets"]
            sh["st"] = parse_st_ref_pic_set(b, n, n, sps["st_sets"])
        if sps["long_term_ref_pics_present_flag"]:
            num_lt_sps = b.ue() if sps["num_long_term_ref_pics_sps"] > 0 else 0
            num_lt_pics = b.ue()
            for i in range(num_lt_sps + num_lt_pics):
                if i < num_lt_sps:
                    idx = 0
                    if sps["num_long_term_ref_pics_sps"] > 1:
                        bits = max(1, (sps["num_long_term_ref_pics_sps"] - 1).bit_length())
                        idx = b.u(bits)
                    poc_lsb = sps["lt_ref_poc_lsb_sps"][idx]
                    used = sps["used_by_curr_pic_lt_sps_flag"][idx]
                else:
                    poc_lsb = b.u(sps["log2_max_poc_lsb"])
                    used = b.u(1)
                msb_present = b.u(1)
                delta_msb = b.ue() if msb_present else None
                sh["lt"].append({"poc_lsb": poc_lsb, "used": used,
                                 "msb_present": msb_present, "delta_msb": delta_msb,
                                 "from_sps": i < num_lt_sps})
    sh["sps"] = sps
    return sh


# ---------------------------------------------------------------- analysis

def analyse(path, max_report):
    with open(path, "rb") as f:
        data = f.read()

    sps_map, pps_map = {}, {}
    prev_poc_lsb = prev_poc_msb = 0
    dpb = {}                    # POC -> {"lt": bool}
    pictures = 0
    counts = Counter()
    lt_used_frames = 0
    lt_entries_total = 0
    dangling = []
    st_dangling = []
    poc_seen_order = []

    cur_pic_first_slice = True
    for nal_type, tid, payload in annexb_nals(data):
        counts[NAL_NAMES.get(nal_type, str(nal_type))] += 1
        if nal_type == SPS_NUT:
            try:
                s = parse_sps(unescape(payload))
                sps_map[s["id"]] = s
            except Exception as e:
                print(f"  ! SPS parse failed: {e}", file=sys.stderr)
            continue
        if nal_type == PPS_NUT:
            try:
                p = parse_pps(unescape(payload))
                pps_map[p["id"]] = p
            except Exception as e:
                print(f"  ! PPS parse failed: {e}", file=sys.stderr)
            continue
        if not is_vcl(nal_type):
            continue

        try:
            sh = parse_slice(unescape(payload), nal_type, sps_map, pps_map)
        except Exception as e:
            print(f"  ! slice parse failed at picture {pictures}: {e}", file=sys.stderr)
            continue
        if sh is None or sh.get("dependent"):
            continue
        if not sh["first"]:
            continue                     # one report per picture

        sps = sh["sps"]
        max_poc_lsb = 1 << sps["log2_max_poc_lsb"]

        # POC derivation (8.3.1).
        if is_idr(nal_type) or (is_irap(nal_type) and nal_type in (BLA_W_LP, BLA_W_RADL, BLA_N_LP)):
            poc = 0
            prev_poc_lsb = prev_poc_msb = 0
            dpb.clear()
        elif sh["poc_lsb"] is None:
            poc = 0
        else:
            lsb = sh["poc_lsb"]
            if lsb < prev_poc_lsb and (prev_poc_lsb - lsb) >= max_poc_lsb // 2:
                msb = prev_poc_msb + max_poc_lsb
            elif lsb > prev_poc_lsb and (lsb - prev_poc_lsb) > max_poc_lsb // 2:
                msb = prev_poc_msb - max_poc_lsb
            else:
                msb = prev_poc_msb
            poc = msb + lsb

        # Check every reference this slice declares.
        if sh["st"]:
            for delta, used in sh["st"]["neg"] + sh["st"]["pos"]:
                if not used:
                    continue
                want = poc + delta
                if want not in dpb:
                    st_dangling.append((pictures, poc, want, "short-term"))
        if sh["lt"]:
            lt_used = [e for e in sh["lt"] if e["used"]]
            if lt_used:
                lt_used_frames += 1
            lt_entries_total += len(lt_used)
            for e in lt_used:
                # Without an explicit MSB the decoder matches on POC LSBs alone.
                if e["msb_present"]:
                    want = poc - e["delta_msb"] * max_poc_lsb - (poc & (max_poc_lsb - 1)) + e["poc_lsb"]
                    present = want in dpb
                else:
                    present = any((p & (max_poc_lsb - 1)) == e["poc_lsb"] for p in dpb)
                    want = e["poc_lsb"]
                if not present:
                    dangling.append((pictures, poc, want, e["poc_lsb"]))

        # Update the picture store the way a decoder would: keep what the RPS
        # says to keep, drop everything else.
        if sh["st"] is not None or sh["lt"]:
            keep = set()
            if sh["st"]:
                for delta, _ in sh["st"]["neg"] + sh["st"]["pos"]:
                    keep.add(poc + delta)
            for e in sh["lt"]:
                for p in dpb:
                    if (p & (max_poc_lsb - 1)) == e["poc_lsb"]:
                        keep.add(p)
            dpb = {p: v for p, v in dpb.items() if p in keep}
        dpb[poc] = {"lt": bool(sh["lt"])}

        if not is_rasl(nal_type) and not is_radl(nal_type) and not is_sub_layer_non_ref(nal_type) and tid == 0:
            prev_poc_lsb = sh["poc_lsb"] if sh["poc_lsb"] is not None else 0
            prev_poc_msb = poc - prev_poc_lsb

        poc_seen_order.append((pictures, poc, NAL_NAMES.get(nal_type, nal_type),
                               SLICE_TYPES.get(sh.get("slice_type"), "?"),
                               len([e for e in sh["lt"] if e["used"]])))
        pictures += 1

    return {
        "path": path, "pictures": pictures, "counts": counts,
        "lt_used_frames": lt_used_frames, "lt_entries_total": lt_entries_total,
        "dangling": dangling, "st_dangling": st_dangling,
        "order": poc_seen_order, "sps": sps_map,
    }


def report(r, frames):
    print(f"\n=== {r['path']} ===")
    print("NAL census: " + "  ".join(f"{k}:{v}" for k, v in sorted(r["counts"].items())))
    print(f"coded pictures: {r['pictures']}")
    for sid, s in sorted(r["sps"].items()):
        print(f"SPS #{sid}: {s['width']}x{s['height']}  "
              f"log2_max_poc_lsb={s['log2_max_poc_lsb']}  "
              f"short_term_sets={s['num_short_term_ref_pic_sets']}  "
              f"long_term_ref_pics_present={s['long_term_ref_pics_present_flag']}  "
              f"num_long_term_ref_pics_sps={s['num_long_term_ref_pics_sps']}")

    print(f"\nlong-term references actually used:")
    print(f"  pictures that reference an LTR: {r['lt_used_frames']} "
          f"of {r['pictures']}")
    print(f"  total LTR entries used:         {r['lt_entries_total']}")

    if r["st_dangling"]:
        print(f"\n  !! {len(r['st_dangling'])} SHORT-TERM references to absent pictures")
        for pic, poc, want, kind in r["st_dangling"][:12]:
            print(f"     picture {pic} (POC {poc}) wants POC {want}")
    if r["dangling"]:
        print(f"\n  !! {len(r['dangling'])} LONG-TERM references to absent pictures")
        for pic, poc, want, lsb in r["dangling"][:12]:
            print(f"     picture {pic} (POC {poc}) wants LTR with POC lsb {lsb}")
    if not r["dangling"] and not r["st_dangling"]:
        print("\n  every reference resolves to a picture the decoder would hold.")

    if frames:
        print(f"\n  {'#':>5} {'POC':>7} {'type':<12} {'slice':<5} {'LTRs used':>9}")
        for pic, poc, name, st, nlt in r["order"][:frames]:
            print(f"  {pic:>5} {poc:>7} {str(name):<12} {st:<5} {nlt:>9}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--frames", type=int, default=0)
    args = ap.parse_args()
    for p in args.paths:
        report(analyse(p, 12), args.frames)


if __name__ == "__main__":
    main()
