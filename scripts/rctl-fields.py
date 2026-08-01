#!/usr/bin/env python3
"""Pin down the meaning of each varying RCTL field, with measured evidence.

Builds on decode-rctl.py (census), analyze-rctl.py (correlation) and
rctl-layout.py (byte census + fits). Two things those scripts missed:

  1. The capture holds TWO RTP streams: pt=100 video (24 kHz clock, 400
     ticks/frame) and pt=101 audio (48 kHz clock, 480 ticks/packet, 100 pkt/s).
     Earlier "cumulative RTP" references merged them, blurring every fit.
  2. Several fields only make sense against media-clock quantities (RTP
     timestamps, LTR-ACK values), not wall time.

For each unexplained field this script fits a battery of concrete candidate
quantities (wall clock, media clocks, cumulative packet/byte/frame counts,
windowed rates, jitter, gaps, LTR-ACK series), unwrapping u16 counters first,
and prints slope / r / residual so a fit is distinguishable from a guess.

    python3 scripts/rctl-fields.py logs/devicehub.pcap
"""
import bisect
import importlib.util
import math
import struct
import sys
from collections import Counter

_dir = __file__.rsplit("/", 1)[0]
_spec = importlib.util.spec_from_file_location("decode_rctl", _dir + "/decode-rctl.py")
dr = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(dr)

VIDEO_PT, AUDIO_PT = 100, 101
VIDEO_CLOCK, AUDIO_CLOCK = 24000.0, 48000.0   # verified below from the data


# --- collection ---------------------------------------------------------------

def collect(path):
    video, audio, rctl, ltr = [], [], [], []
    shapes = Counter()          # RTCP compound shapes per direction
    rctl_shapes = Counter()
    for ts, src, dst, sport, dport, p in dr.udp_datagrams(path):
        if dr.is_rtcp(p):
            names = []
            for pt, st, sub in dr.rtcp_subpackets(p):
                if pt == 204 and len(sub) >= 12:
                    nm = sub[8:12]
                    if nm == b"RCTL":
                        names.append("APP:RCTL")
                        rctl.append((ts, sub))
                    elif nm == b"\x00\x00\x00\x05":
                        names.append("APP:LTR-ACK")
                        ltr.append((ts, struct.unpack(">I", sub[12:16])[0]))
                    else:
                        names.append("APP:" + nm.hex())
                else:
                    names.append({200: "SR", 201: "RR", 202: "SDES"}.get(pt, str(pt)))
            key = "+".join(names)
            shapes[key] += 1
            if "APP:RCTL" in names:
                rctl_shapes[key] += 1
        elif len(p) >= 12 and (p[0] >> 6) == 2:
            pt = p[1] & 0x7F
            rec = (ts, struct.unpack(">H", p[2:4])[0],
                   struct.unpack(">I", p[4:8])[0], bool(p[1] & 0x80), len(p))
            if pt == VIDEO_PT:
                video.append(rec)
            elif pt == AUDIO_PT:
                audio.append(rec)
    return video, audio, rctl, ltr, shapes, rctl_shapes


def unwrap16(vals, thresh=32768):
    out, acc, prev = [], 0, vals[0]
    for v in vals:
        if v < prev - thresh:
            acc += 65536
        out.append(v + acc)
        prev = v
    return out


def unwrap32(vals):
    out, acc, prev = [], 0, vals[0]
    for v in vals:
        if v < prev - (1 << 31):
            acc += 1 << 32
        out.append(v + acc)
        prev = v
    return out


def fit(x, y):
    """Least squares y = a*x + b -> (a, b, r, rms_resid, max_resid)."""
    n = len(x)
    mx, my = sum(x) / n, sum(y) / n
    vx = sum((v - mx) ** 2 for v in x)
    vy = sum((v - my) ** 2 for v in y)
    if vx == 0 or vy == 0:
        return 0.0, my, 0.0, math.sqrt(vy / n) if n else 0.0, 0.0
    cov = sum((u - mx) * (v - my) for u, v in zip(x, y))
    a = cov / vx
    b = my - a * mx
    res = [v - (a * u + b) for u, v in zip(x, y)]
    rms = math.sqrt(sum(e * e for e in res) / n)
    return a, b, cov / math.sqrt(vx * vy), rms, max(abs(e) for e in res)


class Sampler:
    """Step-function view of a cumulative/latest-value series."""

    def __init__(self, times, vals, before=0):
        self.t, self.v, self.before = times, vals, before

    def at(self, t):
        i = bisect.bisect_right(self.t, t)
        return self.v[i - 1] if i else self.before


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "logs/devicehub.pcap"
    video, audio, rctl, ltr, shapes, rctl_shapes = collect(path)
    video.sort()
    audio.sort()
    n = len(rctl)
    ts_r = [t for t, _ in rctl]
    bodies = [b for _, b in rctl]
    t0 = ts_r[0]

    print(f"=== {path} ===")
    print(f"RCTL {n}  LTR-ACK {len(ltr)}  video RTP {len(video)}  "
          f"audio RTP {len(audio)}")

    # --- stream sanity: verify the two RTP clocks ----------------------------
    vspan = video[-1][0] - video[0][0]
    vts = unwrap32([r[2] for r in video])
    aspan = audio[-1][0] - audio[0][0]
    ats = unwrap32([r[2] for r in audio])
    vbytes = sum(r[4] for r in video)
    vframes = sum(1 for r in video if r[3])
    frame_ts = sorted({r[2] for r in video})
    fdel = Counter(b - a for a, b in zip(frame_ts, frame_ts[1:]))
    print(f"\nvideo: {len(video)} pkts, {vbytes} B, {vframes} markers, "
          f"{vspan:.2f}s, {vbytes*8/vspan/1e6:.2f} Mbit/s, "
          f"rtpts rate {(vts[-1]-vts[0])/vspan:.1f}/s "
          f"(24 kHz nominal); per-frame rtpts deltas {fdel.most_common(4)}")
    adel = Counter(b - a for a, b in zip(ats, ats[1:]))
    print(f"audio: {len(audio)} pkts, {aspan:.2f}s, "
          f"{len(audio)/aspan:.2f} pkt/s, rtpts rate {(ats[-1]-ats[0])/aspan:.1f}/s "
          f"(48 kHz nominal); per-pkt rtpts deltas {adel.most_common(3)}")

    # video loss check
    vseq = unwrap16([r[1] for r in video])
    print(f"video seq: {vseq[-1]-vseq[0]+1} expected, {len(video)} received, "
          f"loss {vseq[-1]-vseq[0]+1-len(video)}; "
          f"audio loss {ats and unwrap16([r[1] for r in audio])[-1]-unwrap16([r[1] for r in audio])[0]+1-len(audio)}")

    # --- reference series ----------------------------------------------------
    vt = [r[0] for r in video]
    at_ = [r[0] for r in audio]
    cum_vb, cum_vp, cum_vf = [], [], []
    b = p = f = 0
    for r in video:
        b += r[4]; p += 1; f += r[3]
        cum_vb.append(b); cum_vp.append(p); cum_vf.append(f)
    cum_ap = list(range(1, len(audio) + 1))
    cum_ab, s = [], 0
    for r in audio:
        s += r[4]; cum_ab.append(s)

    frame_times = [r[0] for r in video if r[3]]

    # RFC3550 interarrival jitter, per stream, in clock ticks
    def jitter_series(recs, clock):
        out, J, prev = [], 0.0, None
        for t, _, tsr, _, _ in recs:
            if prev is not None:
                D = (t - prev[0]) * clock - (tsr - prev[1])
                J += (abs(D) - J) / 16.0
            out.append(J)
            prev = (t, tsr)
        return out

    vjit = jitter_series([(t, q, u, m, l) for (t, q, u, m, l), u2 in zip(video, vts)], VIDEO_CLOCK)
    # recompute with unwrapped ts
    vjit, J, prev = [], 0.0, None
    for (t, _, _, _, _), u in zip(video, vts):
        if prev is not None:
            D = (t - prev[0]) * VIDEO_CLOCK - (u - prev[1])
            J += (abs(D) - J) / 16.0
        vjit.append(J)
        prev = (t, u)
    ajit, J, prev = [], 0.0, None
    for (t, _, _, _, _), u in zip(audio, ats):
        if prev is not None:
            D = (t - prev[0]) * AUDIO_CLOCK - (u - prev[1])
            J += (abs(D) - J) / 16.0
        ajit.append(J)
        prev = (t, u)

    ltr_t = [t for t, _ in ltr]
    ltr_v = unwrap32([v for _, v in ltr])

    S = {
        "cum_video_bytes": Sampler(vt, cum_vb),
        "cum_video_pkts": Sampler(vt, cum_vp),
        "cum_video_frames": Sampler(vt, cum_vf),
        "cum_audio_pkts": Sampler(at_, cum_ap),
        "cum_audio_bytes": Sampler(at_, cum_ab),
        "video_rtpts": Sampler(vt, vts, before=vts[0]),
        "audio_rtpts": Sampler(at_, ats, before=ats[0]),
        "video_seq": Sampler(vt, vseq, before=vseq[0]),
        "video_jitter_ticks": Sampler(vt, vjit),
        "audio_jitter_ticks": Sampler(at_, ajit),
        "ltr_val": Sampler(ltr_t, ltr_v, before=ltr_v[0]),
    }

    def ref_at(name, t):
        return S[name].at(t)

    refs = {}
    refs["wall_s"] = [t - t0 for t in ts_r]
    refs["wall_ms"] = [(t - t0) * 1000 for t in ts_r]
    for name in ("cum_video_bytes", "cum_video_pkts", "cum_video_frames",
                 "cum_audio_pkts", "cum_audio_bytes",
                 "video_rtpts", "audio_rtpts", "video_seq", "ltr_val"):
        refs[name] = [ref_at(name, t) for t in ts_r]
    refs["cum_all_bytes"] = [a + b for a, b in zip(refs["cum_video_bytes"], refs["cum_audio_bytes"])]
    refs["cum_all_pkts"] = [a + b for a, b in zip(refs["cum_video_pkts"], refs["cum_audio_pkts"])]
    refs["video_jitter_ms"] = [ref_at("video_jitter_ticks", t) / VIDEO_CLOCK * 1000 for t in ts_r]
    refs["audio_jitter_ms"] = [ref_at("audio_jitter_ticks", t) / AUDIO_CLOCK * 1000 for t in ts_r]

    def last_before(times, t):
        i = bisect.bisect_right(times, t)
        return times[i - 1] if i else None

    refs["ms_since_video_pkt"] = [
        (t - (last_before(vt, t) or t)) * 1000 for t in ts_r]
    refs["ms_since_video_frame"] = [
        (t - (last_before(frame_times, t) or t)) * 1000 for t in ts_r]
    refs["ms_since_audio_pkt"] = [
        (t - (last_before(at_, t) or t)) * 1000 for t in ts_r]
    # last inter-frame gap (ms)
    gaps = []
    for t in ts_r:
        i = bisect.bisect_right(frame_times, t)
        gaps.append((frame_times[i - 1] - frame_times[i - 2]) * 1000 if i >= 2 else 0.0)
    refs["last_frame_gap_ms"] = gaps

    # windowed rates
    for win in (0.05, 0.1, 0.25, 0.5, 1.0):
        tag = f"{int(win*1000)}ms"
        refs[f"vid_kbps_{tag}"] = [
            (ref_at("cum_video_bytes", t) - ref_at("cum_video_bytes", t - win)) * 8 / win / 1000
            for t in ts_r]
        refs[f"vid_pkts_{tag}"] = [
            ref_at("cum_video_pkts", t) - ref_at("cum_video_pkts", t - win) for t in ts_r]
        refs[f"vid_frames_{tag}"] = [
            ref_at("cum_video_frames", t) - ref_at("cum_video_frames", t - win) for t in ts_r]

    # --- extract fields -------------------------------------------------------
    def u16(off):
        return [struct.unpack(">H", b[off:off + 2])[0] for b in bodies]

    F16, F22, F24, F26, F28, F30 = (u16(o) for o in (16, 22, 24, 26, 28, 30))
    F24u = unwrap16(F24)

    def battery(name, vals, wrapped=False, top=6):
        print(f"\n--- {name}: range {min(vals)}..{max(vals)}, "
              f"{len(set(vals))} distinct{' (unwrapped for fitting)' if wrapped else ''} ---")
        rows = []
        for rname, rv in refs.items():
            if len(set(rv)) < 2:
                continue
            a, b_, r, rms, mx = fit(rv, vals)
            spread = max(vals) - min(vals) or 1
            rows.append((rms / spread, rname, a, b_, r, rms, mx))
        rows.sort()
        print(f"  {'candidate':<22} {'slope':>12} {'intercept':>12} {'r':>8} "
              f"{'rms':>8} {'max':>8}")
        for nr, rname, a, b_, r, rms, mx in rows[:top]:
            print(f"  {rname:<22} {a:>12.6g} {b_:>12.6g} {r:>8.4f} "
                  f"{rms:>8.2f} {mx:>8.1f}")
        return rows

    print("\n================ field batteries (best fits by rms/spread) ================")

    # ---- +16 --------------------------------------------------------------
    battery("+16 u16be", F16)
    # zero-free-parameter test: latest LTR-ACK value >> 8
    pred = [ref_at("ltr_val", t) >> 8 for t in ts_r]
    diffs = [v - p for v, p in zip(F16, pred)]
    print(f"  H(+16 == latest LTR-ACK acked ts >> 8): "
          f"diff min {min(diffs)} max {max(diffs)} "
          f"exact {sum(d == 0 for d in diffs)}/{n} within±1 {sum(abs(d) <= 1 for d in diffs)}/{n}")
    # media-clock version with a fitted base
    bases = [ref_at("video_rtpts", t) - 256 * v for t, v in zip(ts_r, F16)]
    bases_s = sorted(bases)
    base = bases_s[len(bases) // 2]
    pred2 = [(ref_at("video_rtpts", t) - base) // 256 for t in ts_r]
    d2 = [v - p for v, p in zip(F16, pred2)]
    print(f"  H(+16 == (latest video rtpts - base)>>8), base={base} (median-fit): "
          f"diff min {min(d2)} max {max(d2)}, "
          f"base spread p5..p95 = {bases_s[n//20]}..{bases_s[-n//20]} ticks")
    # what does the LTR base look like vs video rtpts?
    off_lv = [ref_at("video_rtpts", t) - ref_at("ltr_val", t) for t in ts_r]
    print(f"  (video_rtpts - ltr_val) at RCTL times: min {min(off_lv)} "
          f"median {sorted(off_lv)[n//2]} max {max(off_lv)} ticks "
          f"({sorted(off_lv)[n//2]/VIDEO_CLOCK*1000:.0f} ms median)")

    # ---- +22 --------------------------------------------------------------
    battery("+22 u16be", F22)
    # inspect a few explicit small-int hypotheses
    for rname in ("vid_frames_500ms", "vid_frames_1000ms", "vid_pkts_100ms",
                  "ms_since_video_frame", "last_frame_gap_ms", "video_jitter_ms",
                  "audio_jitter_ms"):
        a, b_, r, rms, mx = fit(refs[rname], F22)
        print(f"  vs {rname:<22} slope {a:8.4f} icept {b_:8.2f} r {r:7.4f} rms {rms:6.2f}")
    # is +22 the count of RTP-timestamp-distinct frames in the last second?

    # ---- +24 --------------------------------------------------------------
    wraps = sum(1 for a, b in zip(F24, F24[1:]) if b < a - 32768)
    rows24 = battery("+24 u16be", F24u, wrapped=True)
    print(f"  wraps observed: {wraps}")
    a, b_, r, rms, mx = fit(refs["wall_s"], F24u)
    print(f"  vs wall clock: slope {a:.3f}/s (1000=ms, 1024=binary-ms) "
          f"r {r:.6f} rms {rms:.2f} max {mx:.1f}")
    for rname, scale, label in (
            ("video_rtpts", VIDEO_CLOCK, "video media clock"),
            ("audio_rtpts", AUDIO_CLOCK, "audio media clock"),
            ("ltr_val", VIDEO_CLOCK, "LTR-acked media clock")):
        a, b_, r, rms, mx = fit(refs[rname], F24u)
        print(f"  vs {rname:<12} slope {a:.6f} (x{scale:.0f} = {a*scale:.2f}/media-s) "
              f"r {r:.6f} rms {rms:.2f} max {mx:.1f}")

    # ---- +28 refinement (computed first: +26 hypotheses depend on it) ------
    # cumulative video pkts *up to and including the last marker packet*
    cum_pkts_at_marker, last_mark_cum = [], 0
    marker_cum = []          # (time, cum_pkts up to that marker)
    b = 0
    for r, cp in zip(video, cum_vp):
        if r[3]:
            marker_cum.append((r[0], cp))
    mtimes = [t for t, _ in marker_cum]
    mcum = [c for _, c in marker_cum]
    Smark = Sampler(mtimes, mcum)

    # ---- +26 --------------------------------------------------------------
    battery("+26 u16be", F26)
    for rname in ("ms_since_video_frame", "ms_since_video_pkt", "last_frame_gap_ms",
                  "video_jitter_ms", "audio_jitter_ms", "vid_pkts_100ms"):
        a, b_, r, rms, mx = fit(refs[rname], F26)
        print(f"  vs {rname:<22} slope {a:8.4f} icept {b_:8.2f} r {r:7.4f} rms {rms:6.2f}")
    # cross-field: +26 vs +22, and vs delta(+24)
    a, b_, r, rms, mx = fit(F22, F26)
    print(f"  vs field +22            slope {a:8.4f} icept {b_:8.2f} r {r:7.4f} rms {rms:6.2f}")
    d24 = [0] + [y - x for x, y in zip(F24u, F24u[1:])]
    a, b_, r, rms, mx = fit(d24, F26)
    print(f"  vs delta(+24)           slope {a:8.4f} icept {b_:8.2f} r {r:7.4f} rms {rms:6.2f}")
    # backlog hypothesis: +26 == pkts received but not yet counted by +28
    backlog = [ref_at("cum_video_pkts", t) - v for t, v in zip(ts_r, F28)]
    a, b_, r, rms, mx = fit(backlog, F26)
    print(f"  vs (cum_rcvd - field28) slope {a:8.4f} icept {b_:8.2f} r {r:7.4f} rms {rms:6.2f}")
    d26b = [x - y for x, y in zip(F26, backlog)]
    print(f"  H(+26 == cum_rcvd_pkts - (+28)): diff min {min(d26b)} max {max(d26b)} "
          f"exact {sum(x == 0 for x in d26b)}/{n} within±2 {sum(abs(x) <= 2 for x in d26b)}/{n}")
    # pkts of the current in-progress frame (since last marker)
    prog = [ref_at("cum_video_pkts", t) - Smark.at(t) for t in ts_r]
    a, b_, r, rms, mx = fit(prog, F26)
    print(f"  vs pkts_since_marker    slope {a:8.4f} icept {b_:8.2f} r {r:7.4f} rms {rms:6.2f}")
    # size/packet-count of the last complete frame
    fr_pkts, fr_bytes, acc_p, acc_b = [], [], 0, 0
    per_frame = []           # (marker_time, pkts_in_frame, bytes_in_frame)
    ap = ab = 0
    for r in video:
        ap += 1
        ab += r[4]
        if r[3]:
            per_frame.append((r[0], ap, ab))
            ap = ab = 0
    Sfp = Sampler([x[0] for x in per_frame], [x[1] for x in per_frame])
    Sfb = Sampler([x[0] for x in per_frame], [x[2] for x in per_frame])
    lf_p = [Sfp.at(t) for t in ts_r]
    lf_b = [Sfb.at(t) for t in ts_r]
    for nm, series in (("last_frame_pkts", lf_p), ("last_frame_bytes", lf_b)):
        a, b_, r, rms, mx = fit(series, F26)
        print(f"  vs {nm:<20} slope {a:8.4f} icept {b_:8.2f} r {r:7.4f} rms {rms:6.2f}")
    # media-lead hypothesis: rtpts of newest packet (in ms of media time)
    # minus elapsed receiver wall time -- explains drift AND sawtooth
    lead = [(ref_at("video_rtpts", t) - vts[0]) / (VIDEO_CLOCK / 1000)
            - (t - vt[0]) * 1000 for t in ts_r]
    a, b_, r, rms, mx = fit(lead, F26)
    print(f"  vs media_lead_ms        slope {a:8.4f} icept {b_:8.2f} r {r:7.4f} rms {rms:6.2f} max {mx:6.1f}")
    lead_ltr = [(ref_at("ltr_val", t) - vts[0]) / (VIDEO_CLOCK / 1000)
                - (t - vt[0]) * 1000 for t in ts_r]
    a, b_, r, rms, mx = fit(lead_ltr, F26)
    print(f"  vs media_lead_ltr_ms    slope {a:8.4f} icept {b_:8.2f} r {r:7.4f} rms {rms:6.2f} max {mx:6.1f}")
    # eyeball sample of the series
    print("  sample (t, +22, +26, media_lead, ms_since_pkt, backlog):")
    for i in range(0, n, 29):
        print(f"    t={ts_r[i]-t0:7.3f}  +22={F22[i]:>3} +26={F26[i]:>3} "
              f"lead={lead[i]:7.1f}  since_pkt={refs['ms_since_video_pkt'][i]:6.1f} "
              f"backlog={backlog[i]:>4}")

    # ---- +28 --------------------------------------------------------------
    battery("+28 u16be", F28)
    pred = [ref_at("cum_video_pkts", t) for t in ts_r]
    d = [v - p for v, p in zip(F28, pred)]
    print(f"  H(+28 == cumulative video RTP packets received): diff min {min(d)} "
          f"max {max(d)} exact {sum(x == 0 for x in d)}/{n} "
          f"within±2 {sum(abs(x) <= 2 for x in d)}/{n}")
    predv = [ref_at("video_seq", t) - vseq[0] + 1 for t in ts_r]
    d = [v - p for v, p in zip(F28, predv)]
    print(f"  H(+28 == highest video seq - first + 1): diff min {min(d)} max {max(d)} "
          f"exact {sum(x == 0 for x in d)}/{n}")
    predm = [Smark.at(t) for t in ts_r]
    d = [v - p for v, p in zip(F28, predm)]
    print(f"  H(+28 == cum video pkts up to last completed frame): diff min {min(d)} "
          f"max {max(d)} exact {sum(x == 0 for x in d)}/{n} "
          f"within±2 {sum(abs(x) <= 2 for x in d)}/{n}")
    for lag_ms in (5, 10, 20, 30):
        predl = [ref_at("cum_video_pkts", t - lag_ms / 1000) for t in ts_r]
        d = [v - p for v, p in zip(F28, predl)]
        print(f"  H(+28 == cum video pkts as of {lag_ms}ms earlier): "
              f"diff min {min(d)} max {max(d)} exact {sum(x == 0 for x in d)}/{n} "
              f"within±2 {sum(abs(x) <= 2 for x in d)}/{n}")

    # ---- +30 --------------------------------------------------------------
    vals30 = Counter(F30)
    print(f"\n--- +30 u16be: values {dict(vals30)} ---")
    # segments + windowed bitrate around transitions
    cur = None
    for i, v in enumerate(F30):
        if v != cur:
            t = ts_r[i]
            kb500 = refs["vid_kbps_500ms"][i]
            kb1000 = refs["vid_kbps_1000ms"][i]
            print(f"  t={t-t0:7.3f}s  ->{v:>6}  (0x{v:04x})  "
                  f"vid kbps(500ms)={kb500:7.1f} (1s)={kb1000:7.1f}  pkt #{i}")
            cur = v
    print(f"  negotiated TX/RXMaxBitrate 6 Mbps = 60000 x 100bps; "
          f"TX/RXMinBitrate 333 kbps = 3330 x 100bps")

    # --- cadence --------------------------------------------------------------
    print("\n================ cadence ================")
    iv = [(b - a) * 1000 for a, b in zip(ts_r, ts_r[1:])]
    iv_s = sorted(iv)
    mean = sum(iv) / len(iv)
    std = math.sqrt(sum((x - mean) ** 2 for x in iv) / len(iv))
    print(f"intervals ms: mean {mean:.2f} std {std:.2f} min {iv_s[0]:.2f} "
          f"p5 {iv_s[len(iv)//20]:.2f} median {iv_s[len(iv)//2]:.2f} "
          f"p95 {iv_s[-len(iv)//20]:.2f} max {iv_s[-1]:.2f}")
    hist = Counter(int(x // 10) * 10 for x in iv)
    print("interval histogram (10ms bins):",
          " ".join(f"{k}-{k+10}ms:{c}" for k, c in sorted(hist.items())))
    # timer vs frame-driven: phase of RCTL relative to video frame arrivals
    phases = [x for x in refs["ms_since_video_frame"]]
    ph_s = sorted(phases)
    print(f"RCTL delay after most recent video frame (ms): "
          f"min {ph_s[0]:.1f} median {ph_s[len(ph_s)//2]:.1f} p95 {ph_s[-len(ph_s)//20]:.1f}")
    # if frame-driven the delay would concentrate near a small constant
    near = sum(1 for x in phases if x < 5)
    print(f"RCTL within 5ms after a frame: {near}/{n} "
          f"(frame-driven would be ~all; timer would be ~{5/ (1000/ (len(frame_times)/vspan)) * 100:.0f}%)")
    # audio-driven?
    nearA = sum(1 for x in refs["ms_since_audio_pkt"] if x < 2)
    print(f"RCTL within 2ms after an audio pkt: {nearA}/{n}")

    # drift test: is the send time a strict timer (regression of ts vs index)?
    a, b_, r, rms, mx = fit(list(range(n)), [t - t0 for t in ts_r])
    print(f"send time vs index: slope {a*1000:.2f} ms/pkt, rms {rms*1000:.1f} ms, "
          f"max {mx*1000:.1f} ms  (small rms => free-running timer)")

    # --- first packet ---------------------------------------------------------
    print("\n================ first packets ================")
    print(f"first video pkt t={vt[0]-t0:+.3f}s  first audio t={at_[0]-t0:+.3f}s "
          f"first LTR-ACK t={ltr_t[0]-t0:+.3f}s (relative to first RCTL)")
    for i in (0, 1, 2, n - 1):
        b = bodies[i]
        print(f"RCTL[{i}] t={ts_r[i]-t0:7.3f}  {b.hex()}")
        print(f"        +16={struct.unpack('>H', b[16:18])[0]:>5} "
              f"+22={struct.unpack('>H', b[22:24])[0]:>3} "
              f"+24={struct.unpack('>H', b[24:26])[0]:>5} "
              f"+26={struct.unpack('>H', b[26:28])[0]:>3} "
              f"+28={struct.unpack('>H', b[28:30])[0]:>5} "
              f"+30={struct.unpack('>H', b[30:32])[0]:>5}")

    # --- compounding ----------------------------------------------------------
    print("\n================ RTCP datagram shapes ================")
    for k, c in shapes.most_common():
        print(f"  {c:>4}  {k}")
    print("datagrams containing RCTL:")
    for k, c in rctl_shapes.most_common():
        print(f"  {c:>4}  {k}")


if __name__ == "__main__":
    main()
