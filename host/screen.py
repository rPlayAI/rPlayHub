#!/usr/bin/env python3
"""Screen video recorder — our own, on top of coredevice.py/remotexpc.py.

Negotiates an RTP/HEVC screen stream via displayservice (feature startmediastream), receives
the device's RTP over the tunnel, depacketizes (RFC 7798) and writes a playable Annex-B .h265.
Offer blob (protobuf-in-bplist, zlib-9) ported from the RE (media_stream_offer). Decode the
result with:  ffmpeg -i out.h265 out.mp4   (or VideoToolbox on macOS).
"""
import os, plistlib, random, socket, threading, time, uuid, zlib

import xpc
from coredevice import CoreDeviceService

DISPLAY_SVC = "com.apple.coredevice.displayservice"

# ---- offer constants (from RE / captured Xcode offer) ----
DEFAULT_DECODER_NAME = "Viceroy 1.7.0"
NEGOTIATOR_MODE_VIDEO = 5
_RES_ENTRY_CODEC_CAP_ID = 50115
_CAPTURED_VIDEO_TIMESTAMP = 17137042128614416384
_HEVC_FEATURES = "FLS;SW:1;"
_AVC_FEATURES = "FLS;VRAE:0;SW:1;"
_CLIENT_SUPPORTED_FEATURES = 140
_ACCESS_NETWORK_TYPE = int(os.environ.get("RPLAY_ACCESS_NETWORK_TYPE", 1))
_TRANSPORT_PROTOCOL_TYPE = 2
# Scale every tier's bitrate ceiling. Apple's own offer uses these exact numbers, and a capture
# of Device Hub proved its stream runs at the same 2.4 Mbps average / 4.0 Mbps peak we get — so
# matching Apple exactly is parity, not quality. Raising the ceiling is the one lever we have that
# does not require imitating a mechanism we have not reverse-engineered. Set RPLAY_TIER_SCALE=4 to
# ask for four times the ceiling and watch stream_info's mbps to see whether the device obliges.
_TIER_SCALE = float(os.environ.get("RPLAY_TIER_SCALE", 1))

_VIDEO_BITRATE_TIERS = (
    (4074, 0, 16384), (0, 75_000_000, 524288), (0, 40_000_000, 12288), (16, 4100, None),
    (0, 20_000_000, 98304), (4, 6500, None), (0, 6_000_000, 131072), (0, 100_000_000, 1048576),
    (0, 60_000_000, 262144), (1, 299, None),
)

# ---- protobuf helpers ----
def _varint(v):
    out = bytearray()
    while True:
        b = v & 0x7F; v >>= 7
        if v: out.append(b | 0x80)
        else: out.append(b); return bytes(out)

def _varint_padded(v, width):
    raw = bytearray(_varint(v))
    if len(raw) > width:
        return _varint_padded(v & ((1 << (7 * width)) - 1), width)
    while len(raw) < width:
        raw[-1] |= 0x80; raw.append(0x00)
    return bytes(raw)

def _tag(f, w): return _varint((f << 3) | w)
def _fv(f, v): return _tag(f, 0) + _varint(v)
def _fb(f, v): return _tag(f, 2) + _varint(len(v)) + v
def _fs(f, v): return _fb(f, v.encode())

def _res_entry(pair_index):
    return _fv(1, 1) + _fv(2, pair_index) + _fv(3, _RES_ENTRY_CODEC_CAP_ID) + _fv(4, 0)

# One codec entry in the offer. This is Apple's VCMediaNegotiatorStreamGroupCodecConfiguration,
# and it is where the RTCP feedback capabilities are negotiated:
#
#   rtcpPSFB_PLIEnabled / rtcpPSFB_FIREnabled / ltrpEnabled / ltrAckFeedbackType
#
# are per-codec-config properties, NOT global settings. That is why 8 PLIs and 8 FIRs produced no
# keyframe: the device never agreed to honour them, because our offer never asked. Apple's own
# recovery path is LTR ACK (`_VideoReceiver_SendLTRACK`, `RTCP_PSFB_LTRACK`), which is why the
# stream carries exactly one IDR and never another.
#
# Field 4 is the suspect for those flags: it is 1 for HEVC and 14 for AVC in the captured offer,
# and 0b0001 vs 0b1110 looks far more like a bitmask than a version or a count. Unconfirmed —
# `flags` and `extra` exist so the mapping can be tested by experiment as soon as the field
# numbers are known, without rebuilding the blob logic.
def _codec_bank(payload_type, features, flags, res_pair_count, extra=None):
    body = _fv(1, payload_type)
    for i in range(res_pair_count):
        body += _fb(2, _res_entry(1 + (i % 2)))
    body += _fs(3, features) + _fv(4, flags)
    for field_no, value in (extra or ()):
        body += _fv(field_no, value)
    return body

HEVC_PAYLOAD_TYPE = 123
AVC_PAYLOAD_TYPE = 100


# Experiment hooks for the codec configuration, settable without editing code:
#   RPLAY_HEVC_FLAGS=<int>     override field 4 for the HEVC bank (default 1)
#   RPLAY_AVC_FLAGS=<int>      override field 4 for the AVC bank  (default 14)
#   RPLAY_CODEC_EXTRA=5:1,6:1  append extra varint fields to every codec bank
# Wrong values are expected to make the device reject the offer outright, which is a clear signal
# rather than a silent degradation — so this is safe to sweep.
def _codec_extra_from_env():
    raw = os.environ.get("RPLAY_CODEC_EXTRA", "").strip()
    out = []
    for part in raw.split(","):
        if ":" in part:
            f, v = part.split(":", 1)
            try:
                out.append((int(f), int(v)))
            except ValueError:
                pass
    return out


def _media_blob_video(session_id, codec="auto"):
    """Build the video offer. `codec` decides which codec banks we advertise.

    The device chooses from what we offer, and offering both (the default) gets HEVC. Offering
    only the AVC bank is how to ask for H.264 — useful because H.264 is far more forgiving to
    depacketize and decode, and because the display pipeline this project adopted from ~/rplay is
    already proven on H.264.
    """
    hevc_flags = int(os.environ.get("RPLAY_HEVC_FLAGS", 1))
    avc_flags = int(os.environ.get("RPLAY_AVC_FLAGS", 14))
    extra = _codec_extra_from_env()
    if hevc_flags != 1 or avc_flags != 14 or extra:
        print(f"  codec config override: hevc_flags={hevc_flags} avc_flags={avc_flags} "
              f"extra={extra}")
    hevc = _fb(3, _codec_bank(123, _HEVC_FEATURES, hevc_flags, 4, extra))
    avc = _fb(3, _codec_bank(100, _AVC_FEATURES, avc_flags, 2, extra))
    banks = {"auto": hevc + avc, "hevc": hevc, "h264": avc}
    if codec not in banks:
        raise ValueError(f"codec must be one of {sorted(banks)}, not {codec!r}")
    vs = (_tag(1, 0) + _varint_padded(session_id, 5)
          + _fv(2, 0)
          + banks[codec]
          # Field 7 is ltrpEnabled and field 10 was our invented fecEnabled. Both were guesses
          # until a tcpdump of Device Hub's own tunnel settled them: Apple sends 7=1 and omits 10
          # entirely, and every other field in this blob — codec banks, resolution entries,
          # feature strings, all ten bitrate tiers — is already byte-identical to ours.
          #
          # ltrpEnabled is the important one. It turns on long-term reference pictures, which is
          # how this device recovers from loss: it never sends a second IDR and ignores PLI and
          # FIR, so instead the receiver acknowledges frames it decoded (RTCP APP 'LTR ACK') and
          # the encoder predicts from the newest acknowledged frame rather than from a frame the
          # receiver may have missed. Without it a single lost packet smears until the session
          # ends. This must be kept in step with RTCPSession.send_ltr_ack().
          #
          # Both halves are switchable, because "LTR without acks" and "acks without LTR" are each
          # worse than either consistent choice, and only an experiment can say which consistent
          # choice this device likes:
          #     RPLAY_LTRP=0      offer field 7 off — P-frames chain off the previous frame, so a
          #                       bad frame diffuses and washes out instead of being anchored
          #     RPLAY_LTR_ACK=0   stop sending the acks (see RTCPSession.send_ltr_ack)
          + _fv(7, int(os.environ.get("RPLAY_LTRP", 1))) + _fv(8, 63) + _fv(12, 1))
    f9s = b""
    for f1, f2, f3 in _VIDEO_BITRATE_TIERS:
        # Only the large ceilings scale; the small values (299, 4100, 6500) are not bitrates.
        f2s = int(f2 * _TIER_SCALE) if f2 >= 1_000_000 else f2
        body = _fv(1, f1) + _fv(2, f2s) + (_fv(3, f3) if f3 is not None else b"")
        f9s += _fb(9, body)
    return (_fv(1, 1) + _fv(2, 1) + _fb(5, vs) + _fs(6, DEFAULT_DECODER_NAME) + _fv(8, 0)
            + f9s + _fv(13, _CAPTURED_VIDEO_TIMESTAMP) + _fv(14, 2) + _fv(16, 0) + _fv(18, 1))

def _endpoint_info(model, os_ver, build):
    return _fv(1, 0) + _fv(2, 1) + _fs(3, model) + _fs(4, os_ver) + _fs(5, build)

def build_offer(call_id, session_id, model="Mac15,9", os_ver="2205.3.1", build="25F80",
                codec="auto"):
    blob = zlib.compress(_media_blob_video(session_id, codec), level=9)  # only level 9 is accepted
    return plistlib.dumps({
        "avcMediaStreamOptionRemoteEndpointInfo": _endpoint_info(model, os_ver, build),
        "avcMediaStreamNegotiatorMode": NEGOTIATOR_MODE_VIDEO,
        "avcMediaStreamNegotiatorMediaBlob": blob,
        "avcMediaStreamOptionCallID": call_id,
    }, fmt=plistlib.FMT_BINARY)

# ---- RTP/HEVC depacketize (RFC 7798) ----
_HEVC_NAL_AP, _HEVC_NAL_FU = 48, 49
_DS_TRAILER = bytes.fromhex("04f00ac0000003000004ec0ab003")   # displayservice 14-byte NAL footer
_ANNEXB = b"\x00\x00\x00\x01"

def _strip_trailer(nal):
    return nal[:-len(_DS_TRAILER)] if nal.endswith(_DS_TRAILER) else nal

def _depacketize(payload, fu, out):
    if len(payload) < 2:
        return
    nt = (payload[0] >> 1) & 0x3F
    if nt == _HEVC_NAL_AP:
        i = 2
        while i + 2 <= len(payload):
            size = int.from_bytes(payload[i:i + 2], "big"); i += 2
            out.append(payload[i:i + size]); i += size
    elif nt == _HEVC_NAL_FU:
        fuh = payload[2]
        if fuh & 0x80:  # start
            fu[:] = bytes([(payload[0] & 0x81) | ((fuh & 0x3F) << 1), payload[1]]) + payload[3:]
        else:
            fu.extend(payload[3:])
        if (fuh & 0x40) and fu:  # end
            out.append(_strip_trailer(bytes(fu))); fu.clear()
    else:
        out.append(_strip_trailer(payload))

def parse_rtp(pkt):
    """Strip the RTP header; return (payload, seq, is_rtcp, payload_type).

    The payload type tells us which codec the device actually chose — 123 for HEVC, 100 for AVC.
    That is worth surfacing rather than assuming, because the device picks from what we offered.
    """
    if len(pkt) < 12:
        return b"", 0, True, -1
    pt = pkt[1] & 0x7F
    # RTCP, and the masking matters: RTCP's packet-type byte is 200-204 (SR/RR/SDES/BYE/APP), but
    # the marker bit has already been stripped above, so they arrive here as 72-76. Comparing
    # against 200-204 after masking can never match — which is exactly the bug that fed every
    # RTCP packet into the video depacketizer as if it were a frame. RFC 5761 reserves 64-95 for
    # RTCP when RTP and RTCP share a port, so that is the range to reject.
    if 64 <= pt <= 95:
        return b"", 0, True, pt
    seq = int.from_bytes(pkt[2:4], "big")
    cc = pkt[0] & 0x0F
    off = 12 + 4 * cc
    if pkt[0] & 0x10 and off + 4 <= len(pkt):   # extension header
        extlen = int.from_bytes(pkt[off + 2:off + 4], "big")
        off += 4 + 4 * extlen
    return pkt[off:], seq, False, pt


def _rtp_payload(pkt):
    """Back-compatible 3-tuple form."""
    payload, seq, is_rtcp, _pt = parse_rtp(pkt)
    return payload, seq, is_rtcp


# ---- RTP/H.264 depacketize (RFC 6184) ----
_AVC_STAP_A, _AVC_FU_A = 24, 28


def depacketize_h264(payload, fu, out):
    """H.264 has a 1-byte NAL header and different aggregation/fragmentation types than HEVC:
    STAP-A (24) instead of AP (48), FU-A (28) instead of FU (49)."""
    if not payload:
        return
    nt = payload[0] & 0x1F
    if nt == _AVC_STAP_A:
        i = 1
        while i + 2 <= len(payload):
            size = int.from_bytes(payload[i:i + 2], "big"); i += 2
            if size:
                out.append(_strip_trailer(payload[i:i + size]))
            i += size
    elif nt == _AVC_FU_A:
        if len(payload) < 2:
            return
        fuh = payload[1]
        if fuh & 0x80:      # start: rebuild the original 1-byte header
            fu[:] = bytes([(payload[0] & 0xE0) | (fuh & 0x1F)]) + payload[2:]
        else:
            fu.extend(payload[2:])
        if (fuh & 0x40) and fu:     # end
            out.append(_strip_trailer(bytes(fu))); fu.clear()
    else:
        out.append(_strip_trailer(payload))


def sniff_codec(payload):
    """Identify the codec from an RTP payload's own structure, or None if unclear.

    Necessary because the RTP payload type lies on this device: it sends HEVC under payload
    type 100. HEVC and H.264 are distinguishable from the packetization header — HEVC uses a
    2-byte header with AP=48 / FU=49, H.264 a 1-byte header with STAP-A=24 / FU-A=28 — and the
    ranges do not overlap, so a single packet is usually enough.
    """
    if len(payload) < 3:
        return None
    hevc_type = (payload[0] >> 1) & 0x3F
    avc_type = payload[0] & 0x1F

    # Aggregation/fragmentation packets are the strongest signal: each codec's values are
    # meaningless in the other's numbering.
    if hevc_type in (_HEVC_NAL_AP, _HEVC_NAL_FU):
        # Confirm with the layer/temporal-id byte, which is 1 for a single-layer stream.
        if payload[1] == 1:
            return "hevc"
    if avc_type in (_AVC_STAP_A, _AVC_FU_A):
        return "h264"
    # A plain single NAL: HEVC's forbidden_zero + reserved bits leave bit 7 clear and byte 1 == 1.
    if hevc_type < 32 and payload[1] == 1 and not (payload[0] & 0x80):
        return "hevc"
    if 1 <= avc_type <= 5:
        return "h264"
    return None


def depacketizer_for(codec):
    """Pick the right RFC: 7798 for HEVC, 6184 for H.264."""
    return depacketize_h264 if codec == "h264" else _depacketize


class ReorderBuffer:
    """Reorder RTP payloads by 16-bit sequence number before depacketizing.
    Releases in order; force-advances past a gap once `window` packets pile up
    (a lost packet), and drops stale (already-passed) arrivals."""
    def __init__(self, window=256):
        self.buf = {}
        self.next = None
        self.window = window
        # Loss accounting. This is how we tell "the device is sending us a poor stream" apart from
        # "the network is dropping packets" — two problems with completely different fixes.
        self.received = 0
        self.lost = 0          # sequence numbers we gave up waiting for
        self.late = 0          # arrived after we had already moved past them
        self.duplicates = 0

    @property
    def loss_pct(self):
        total = self.received + self.lost
        return round(100.0 * self.lost / total, 2) if total else 0.0

    def push(self, seq, item, out):
        if self.next is None:
            self.next = seq
        if ((seq - self.next) & 0xFFFF) > 0x8000:   # behind next -> already emitted, drop
            self.late += 1
            return
        if seq in self.buf:
            self.duplicates += 1
        self.received += 1
        self.buf[seq] = item
        self._drain(out)
        while len(self.buf) > self.window:
            # Force past a hole: everything between `next` and the earliest buffered packet is
            # never going to arrive, so count it as lost rather than losing it silently.
            earliest = min(self.buf, key=lambda s: (s - self.next) & 0xFFFF)
            self.lost += (earliest - self.next) & 0xFFFF
            self.next = earliest
            self._drain(out)

    def _drain(self, out):
        while self.next in self.buf:
            out.append(self.buf.pop(self.next))
            self.next = (self.next + 1) & 0xFFFF

    def flush(self, out):
        for s in sorted(self.buf, key=lambda s: (s - self.next) & 0xFFFF):
            out.append(self.buf[s])
        self.buf.clear()


class RTCPSession:
    """Receiver-side RTCP: Receiver Reports to keep the stream alive, PLI to ask for a keyframe.

    Measured, not guessed: the device sends Sender Reports (RTCP PT 200, which arrives as 72 once
    the marker bit is masked) on the SAME port as the RTP, and if nothing ever answers them it
    stops transmitting after roughly 30 seconds. Sending RRs is therefore not a quality
    refinement — it is what keeps video flowing.

    PLI (RFC 4585 payload-specific feedback, PT 206 / FMT 1) is the proper way to ask for a
    keyframe, replacing the media-stream restart that killed the stream outright.
    """

    def __init__(self, sock, ssrc=None):
        self.sock = sock
        self.ssrc = ssrc if ssrc is not None else random.randint(1, 0xFFFFFFFE)
        self.peer = None            # where their packets come from == where ours must go
        self.their_ssrc = None
        self.highest_seq = 0
        self.cycles = 0
        self.received = 0
        self.expected_prior = 0
        self.received_prior = 0
        self.last_sr_middle = 0     # middle 32 bits of the NTP stamp in their last SR
        self.last_sr_at = 0.0
        self.rr_sent = 0
        self.pli_sent = 0
        self.fir_sent = 0
        self.fir_seq = 0
        self.ltr_acked = 0

    # --- bookkeeping ---
    def note_rtp(self, pkt, addr):
        if self.peer is None:
            self.peer = addr
        if len(pkt) < 12:
            return
        seq = int.from_bytes(pkt[2:4], "big")
        if self.received and seq < (self.highest_seq & 0xFFFF) - 0x4000:
            self.cycles += 1        # sequence number wrapped
        self.highest_seq = (self.cycles << 16) | seq
        self.their_ssrc = int.from_bytes(pkt[8:12], "big")
        self.received += 1

    def note_rtcp(self, pkt, addr):
        if self.peer is None:
            self.peer = addr
        if len(pkt) < 4:
            return
        if (pkt[1] & 0x7F) != 72:   # only a Sender Report carries the timestamp we echo
            return
        if len(pkt) >= 16:
            # NTP is bytes 8..16; the middle 32 bits are the low half of seconds and high half
            # of the fraction, which is what LSR wants.
            self.last_sr_middle = int.from_bytes(pkt[10:14], "big")
            self.last_sr_at = time.time()

    # --- outgoing ---
    def _report_block(self):
        expected = self.highest_seq + 1
        lost = max(0, expected - self.received)
        expected_interval = expected - self.expected_prior
        received_interval = self.received - self.received_prior
        self.expected_prior, self.received_prior = expected, self.received
        lost_interval = expected_interval - received_interval
        fraction = 0
        if expected_interval > 0 and lost_interval > 0:
            fraction = min(255, (lost_interval << 8) // expected_interval)
        dlsr = 0
        if self.last_sr_at:
            dlsr = int((time.time() - self.last_sr_at) * 65536) & 0xFFFFFFFF
        return (int(self.their_ssrc or 0).to_bytes(4, "big")
                + bytes([fraction]) + (lost & 0xFFFFFF).to_bytes(3, "big")
                + (self.highest_seq & 0xFFFFFFFF).to_bytes(4, "big")
                + (0).to_bytes(4, "big")                      # jitter: not computed
                + (self.last_sr_middle & 0xFFFFFFFF).to_bytes(4, "big")
                + (dlsr & 0xFFFFFFFF).to_bytes(4, "big"))

    def send_rr(self):
        if not self.peer or self.their_ssrc is None:
            return False
        body = self.ssrc.to_bytes(4, "big") + self._report_block()
        pkt = bytes([0x81, 201]) + ((len(body) // 4)).to_bytes(2, "big") + body
        try:
            self.sock.sendto(pkt, self.peer)
            self.rr_sent += 1
            return True
        except OSError:
            return False

    def send_ltr_ack(self, rtp_timestamp):
        """Acknowledge a frame we decoded, so the encoder can predict from it.

        This is how this device actually recovers from loss, and PLI and FIR are not. A tcpdump of
        Device Hub's own tunnel settled it: over a 20-second mirroring session Apple sent 594 of
        these and not one PLI or FIR, and every single acknowledged value was the RTP timestamp of
        a video frame it had just received — one ack per frame, no repeats.

        The mechanism is long-term reference pictures. The encoder keeps acknowledged frames as
        reference candidates; when it detects loss it re-predicts from the newest frame the
        receiver confirmed rather than sending a fresh IDR. That is why this stream contains
        exactly one IDR and never another, and why an unacknowledged stream smears until the
        session ends. It only works if `ltrpEnabled` is set in the offer — see field 7 in
        _media_blob_video().

        Wire format, read straight off the capture: an RTCP APP packet (PT 204) of 16 bytes,
        length 3, with the four-byte name field carrying 0x00000005 rather than ASCII.

            80 cc 00 03 | <our SSRC> | 00 00 00 05 | <acknowledged RTP timestamp>
        """
        if not self.peer:
            return False
        pkt = (bytes([0x80, 204]) + (3).to_bytes(2, "big")
               + self.ssrc.to_bytes(4, "big")
               + (5).to_bytes(4, "big")
               + (rtp_timestamp & 0xFFFFFFFF).to_bytes(4, "big"))
        try:
            self.sock.sendto(pkt, self.peer)
            self.ltr_acked += 1
            return True
        except OSError:
            return False

    def send_fir(self):
        """Full Intra Request (RFC 5104, PT 206 / FMT 4).

        Sent alongside PLI because PLI alone was measured being ignored by this device: one IDR
        arrived at stream start and none followed across 20 seconds despite a PLI. FIR is the
        stronger request — "send a full intra picture now" — and carries a sequence number the
        sender uses to detect repeats, so it must increment per request.
        """
        if not self.peer or self.their_ssrc is None:
            return False
        self.fir_seq = (getattr(self, "fir_seq", 0) + 1) & 0xFF
        target = int(self.their_ssrc)
        # FCI: target SSRC, then the command sequence number in the top byte of a word.
        fci = target.to_bytes(4, "big") + bytes([self.fir_seq, 0, 0, 0])
        body = self.ssrc.to_bytes(4, "big") + (0).to_bytes(4, "big") + fci
        pkt = bytes([0x84, 206]) + ((len(body) // 4)).to_bytes(2, "big") + body
        try:
            self.sock.sendto(pkt, self.peer)
            self.fir_sent = getattr(self, "fir_sent", 0) + 1
            return True
        except OSError:
            return False

    def send_pli(self):
        """Ask for a fresh keyframe without disturbing the stream."""
        if not self.peer or self.their_ssrc is None:
            return False
        body = self.ssrc.to_bytes(4, "big") + int(self.their_ssrc).to_bytes(4, "big")
        pkt = bytes([0x81, 206]) + ((len(body) // 4)).to_bytes(2, "big") + body
        try:
            self.sock.sendto(pkt, self.peer)
            self.pli_sent += 1
            return True
        except OSError:
            return False


# The negotiated stream LIFETIME, and it is a genuine trade-off with no comfortable answer.
#
#   * It is a hard lifetime, not an idle timeout: video stops when it expires even with RTCP
#     receiver reports flowing the whole time (measured at the original 20 s).
#   * The stream cannot be restarted within a session — every attempt has failed — so this value
#     is effectively the maximum session length.
#   * The device allows ONE stream, so a run that dies without stopmediastream leaves the slot
#     occupied for the REMAINDER of this timeout, blocking every later session. At 3600 s that
#     meant an hour of lockout, which is what happened.
#
# So it wants to be long enough to be useful and short enough that a crash is not punishing.
# Ten minutes, now that the engine releases the session on SIGTERM/SIGINT/SIGHUP and atexit,
# which makes an abandoned stream the exception rather than the rule.
#
# Worth revisiting: the Device Hub capture shows Apple sends timeout=20 and still mirrors for
# minutes, because the device sustains the stream on RTCP (its answer carries RTCPSendInterval=1.0
# and RTCPTimeoutInterval=20.0) rather than on this value. Our own 20 s measurement above predates
# LTR-ACK, which we now send ~30/s alongside the 1/s receiver reports. If 20 no longer cuts video
# off, it is strictly better: an abandoned session would lock the device for 20 s instead of 10
# minutes. Set RPLAY_STREAM_TIMEOUT=20 to test that without editing code.
# An experiment that the device does not understand can hang startmediastream outright, and the
# slot then stays held for this whole lifetime — which is how a resolution experiment took Device
# Hub down with it. So when any request-shape knob is in play, default to a short lifetime: a
# failed experiment should cost a minute, not ten. An explicit RPLAY_STREAM_TIMEOUT still wins.
_EXPERIMENTAL_KNOBS = ("RPLAY_WIDTH", "RPLAY_HEIGHT", "RPLAY_FPS", "RPLAY_VIDEO_RESOLUTION",
                       "RPLAY_MAX_BITRATE", "RPLAY_MIN_BITRATE", "RPLAY_KEYFRAME_INTERVAL",
                       "RPLAY_DISPLAY_SCALE", "RPLAY_VIRTUAL_DISPLAY")
_DEFAULT_TIMEOUT = 60 if any(os.environ.get(k) for k in _EXPERIMENTAL_KNOBS) else 600
STREAM_TIMEOUT_S = int(os.environ.get("RPLAY_STREAM_TIMEOUT", _DEFAULT_TIMEOUT))

# Sending LTR acks only makes sense while the offer asks for LTR; keep the pair switchable.
LTR_ACK_ENABLED = os.environ.get("RPLAY_LTR_ACK", "1") != "0"


def _stream_config_from_env():
    """Build a streamConfig dict from RPLAY_* knobs. Empty means "let the device decide"."""
    cfg = {}
    for env, key in (("RPLAY_WIDTH", "CustomWidth"),
                     ("RPLAY_HEIGHT", "CustomHeight"),
                     ("RPLAY_FPS", "Framerate"),
                     ("RPLAY_VIDEO_RESOLUTION", "VideoResolution"),
                     ("RPLAY_MAX_BITRATE", "TXMaxBitrate"),
                     ("RPLAY_MIN_BITRATE", "TXMinBitrate"),
                     ("RPLAY_KEYFRAME_INTERVAL", "KeyFrameInterval")):
        raw = os.environ.get(env)
        if raw:
            try:
                # PLAIN values, not {"int": n}. The options dictionary uses typed wrappers, but
                # streamConfig does not: the device's own answer encodes CustomWidth as 1184,
                # RTCPSendInterval as 1.0 and DestIsIPv6 as True. Sending wrapped values here is
                # almost certainly what hung startmediastream the first time — the device would
                # have read an integer and found a dictionary.
                cfg[key] = int(raw)
            except ValueError:
                pass
    return cfg


def describe_answer(answer):
    """Print what the device actually chose, rather than what we asked for.

    The offer is a wish list; the device answers with the encoding it will really do — coded size,
    frame rate, bitrate ceiling. Those three decide picture quality entirely, and we were throwing
    the answer away, which meant every quality experiment had no readout. Measured against this
    screen: 1184x2576 at ~45 fps and a ~4 Mbps ceiling is 0.029 bits/pixel, roughly 3.5x below
    what UI content needs to look clean — and Device Hub gets the same numbers, so the artefacting
    under motion is the encoder's budget, not our pipeline.
    """
    if not isinstance(answer, dict):
        return
    blob = None
    for value in _walk(answer):
        if isinstance(value, bytes) and value[:1] == b"b" and b"plist" in value[:16]:
            try:
                pl = plistlib.loads(value)
            except Exception:
                continue
            for k, v in pl.items():
                if isinstance(v, bytes) and "MediaBlob" in k:
                    blob = v
    if blob is None:
        return
    try:
        raw = zlib.decompress(blob)
    except Exception:
        return
    # The answer is protobuf we have not fully mapped. Rather than guess field names, surface the
    # values that plausibly are the ones that matter: any varint that looks like a dimension, a
    # frame rate or a bitrate. Wrong labels would be worse than none, so they are marked unsure.
    fields = list(_varint_fields(raw))

    # The coded size is the number that decides picture quality, so pull it out by name rather
    # than leaving it in a list of unlabelled fields. Identified from a real answer: fields 4 and 5
    # of the same submessage carry width and height (1184 x 2544 for this panel).
    size = None
    for (fa, va), (fb, vb) in zip(fields, fields[1:]):
        if fa == 4 and fb == 5 and 240 <= va <= 8192 and 240 <= vb <= 8192:
            size = (va, vb)
            break

    print(f"  device answer: {len(raw)} bytes of protobuf")
    if size:
        w, h = size
        # A bitrate only means something per pixel. Roughly 0.10 bits/pixel is where UI content
        # stops falling apart in motion; this device grants about 4 Mbps whatever we ask for.
        bpp = 4.0e6 / (w * h * 45)
        print(f"    coded size: {w}x{h}   ~{bpp:.3f} bits/pixel at 4 Mbps/45fps"
              + ("   <- unchanged; the request did not take" if (w, h) == (1184, 2544) else
                 "   <- CHANGED"))
    # Deliberately not reporting a "bitrate ceiling" here. The answer echoes part of our own tier
    # table, so the largest value in it varies run to run (75 Mbps, then 100 Mbps) and reflects
    # what we offered rather than what the device granted. The measured peak in the health line is
    # the honest number; this one only looked authoritative.
    seen = [f"f{f}={v}" for f, v in fields
            if 200 <= v <= 4096 or 100_000 <= v <= 200_000_000]
    if seen:
        print(f"    other values (unmapped): {' '.join(seen[:14])}")


def _walk(obj):
    if isinstance(obj, dict):
        for v in obj.values():
            yield from _walk(v)
    elif isinstance(obj, (list, tuple)):
        for v in obj:
            yield from _walk(v)
    else:
        yield obj


def _varint_fields(buf, depth=0):
    """Yield (field_number, value) for varints, descending into length-delimited submessages."""
    o = 0
    while o < len(buf):
        try:
            key, o = _read_varint(buf, o)
        except Exception:
            return
        fno, wt = key >> 3, key & 7
        if wt == 0:
            try:
                val, o = _read_varint(buf, o)
            except Exception:
                return
            yield fno, val
        elif wt == 2:
            try:
                n, o = _read_varint(buf, o)
            except Exception:
                return
            sub, o = buf[o:o + n], o + n
            if depth < 4:
                yield from _varint_fields(sub, depth + 1)
        elif wt == 5:
            o += 4
        elif wt == 1:
            o += 8
        else:
            return


def _read_varint(buf, o):
    r = shift = 0
    while True:
        b = buf[o]; o += 1
        r |= (b & 0x7F) << shift; shift += 7
        if not b & 0x80:
            return r, o


def open_stream(dev_addr, our_addr, display_port, on_packet=None, codec="auto",
                timeout_s=STREAM_TIMEOUT_S):
    """Negotiate an RTP screen stream and start draining it in a background thread.
    Returns a handle; pass it to close_stream(). Opening this ALSO opens the HID auth
    gate (dtuhidd routes touch to UIKit only while a media stream runs)."""
    sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    sock.bind(("::", 0))
    recv_port = sock.getsockname()[1]
    for sz in (4 * 1024 * 1024, 1 * 1024 * 1024):
        try:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, sz); break
        except OSError:
            continue

    svc = CoreDeviceService(dev_addr, display_port)
    csid = uuid.uuid4()

    # ONE identity for this receiver, used in both the offer and every RTCP packet we send.
    #
    # These used to be two independent random numbers, and that quietly broke LTR. Field 5.1 of
    # the offer is our SSRC: the device echoes it back in its answer as RemoteSSRC, and Apple's
    # capture shows the same value in all three places — offer 725448759, answer RemoteSSRC
    # 725448759, and every LTR-ACK sent with SSRC 0x2b3d7837, which is that number.
    #
    # We were announcing one SSRC in the offer and sending acks from another, so the device saw
    # acknowledgements from a source it had never associated with this stream and discarded them.
    # That is the worst of both worlds: the encoder anchors prediction on a long-term reference
    # and never learns which frames we actually received, so an error is anchored instead of
    # washing out — exactly the "first frame perfect, then permanently garbled" symptom.
    ssrc = random.randint(1, 0xFFFFFFFE)
    request = {
        "clientSupportedFeatures": xpc.U64(_CLIENT_SUPPORTED_FEATURES),
        "direction": "output",
        "negotiatorOffer": build_offer(str(uuid.uuid4()).upper(), ssrc, codec=codec),
        "options": {
            "AVCMediaStreamNegotiatorAccessNetworkType": {"int": _ACCESS_NETWORK_TYPE},
            "AVCMediaStreamNegotiatorTransportProtocolType": {"int": _TRANSPORT_PROTOCOL_TYPE},
            "CoreDeviceVideoDisplayMode": {"string": "DisplayByID"},
            "VideoStreamForDisplayID": {"int": 1},
            "avcMediaStreamOptionClientSessionID": {"uuid": csid},
        },
        "receiverIP": our_addr, "receiverPort": xpc.U64(recv_port),
        "senderIP": dev_addr, "timeout": xpc.U64(timeout_s), "type": "video",
    }
    # Scaling the captured display is the cheapest way to cut the pixel rate, and it goes in the
    # options dictionary the device already accepts from us rather than in a new top-level key.
    scale = os.environ.get("RPLAY_DISPLAY_SCALE")
    if scale:
        request["options"]["VideoStreamDisplayScale"] = {"double": float(scale)}
        print(f"  display scale override: {scale}")

    # A virtual external display is encoded at a size WE choose, which is the whole point: the
    # device's own panel is 1184x2576 and the ~4 Mbps it grants cannot cover that in motion.
    # CoreDeviceUtilities spells out the requirement — "kVideoStreamDisplayModeVirtualExternal
    # requested, but missing width, height" — and the per-case coding keys are width/height/
    # refreshRate.
    virt = os.environ.get("RPLAY_VIRTUAL_DISPLAY")
    if virt:
        size, _, rate = virt.partition("@")
        w, _, h = size.lower().partition("x")
        source = {"width": xpc.U64(int(w)), "height": xpc.U64(int(h))}
        if rate:
            source["refreshRate"] = xpc.U64(int(rate))
        request["source"] = {"videoVirtualExternalDisplay": source}
        # DisplayByID names the device's own panel; a virtual display is a different mode.
        request["options"]["CoreDeviceVideoDisplayMode"] = {"string": "VirtualExternal"}
        request["options"].pop("VideoStreamForDisplayID", None)
        print(f"  virtual external display: {w}x{h}" + (f"@{rate}" if rate else ""))

    cfg = _stream_config_from_env()
    if cfg:
        # `streamConfig` is a sibling of `options` and `negotiatorOffer` in the request vocabulary
        # (CoreDeviceUtilities carries the whole key list), and its keys are the encoding settings
        # the device otherwise picks alone: CustomWidth, CustomHeight, Framerate, TXMaxBitrate.
        #
        # This is the one lever with real headroom. At the negotiated 1184x2576 and ~45 fps, the
        # ~4 Mbps the device grants is 0.029 bits/pixel — about 3.5x below what UI content needs,
        # which is why one swipe destroys the picture. Device Hub measures the same, so this is a
        # budget to escape rather than a bug to fix. Halving the coded size would put the same
        # bitrate at roughly 0.17 bits/pixel.
        #
        # This hung the device once, when the values were sent as {"int": n} wrappers. The
        # answer shows streamConfig uses plain scalars, so that mismatch is the likely cause and
        # is now fixed. Still treat it carefully: it is the only key here Apple does not send.
        #
        # KeyFrameInterval is the interesting one. The device answers 0 — one IDR at stream start
        # and never another — which is exactly why a single corrupted frame stays on screen for
        # the rest of the session instead of washing out. A small non-zero interval would make
        # the picture self-repair.
        #
        #     RPLAY_KEYFRAME_INTERVAL=60
        request["streamConfig"] = cfg
        print(f"  streamConfig override: {cfg}")
    print(f"  negotiating stream: receiver [{our_addr}]:{recv_port} <- device {dev_addr}")
    answer = svc.invoke("com.apple.coredevice.feature.startmediastream", request,
                        action_identifier="com.apple.coredevice.action.mediastreamstart")
    describe_answer(answer)

    stop = threading.Event()
    stats = {"packets": 0, "bytes": 0, "started": time.time()}
    rtcp = RTCPSession(sock, ssrc=ssrc)

    def drain():
        sock.settimeout(1.0)
        while not stop.is_set():
            try:
                pkt, addr = sock.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                break
            stats["packets"] += 1
            stats["bytes"] += len(pkt)
            # RTCP shares this port, so it has to be split off here and answered.
            if len(pkt) > 1 and 64 <= (pkt[1] & 0x7F) <= 95:
                rtcp.note_rtcp(pkt, addr)
            else:
                rtcp.note_rtp(pkt, addr)
            if on_packet:
                on_packet(pkt)

    def report():
        """Answer their Sender Reports. Without this the device stops sending after ~30s."""
        while not stop.is_set():
            time.sleep(1.0)
            rtcp.send_rr()

    t = threading.Thread(target=drain, daemon=True); t.start()
    rt = threading.Thread(target=report, daemon=True); rt.start()
    return {"svc": svc, "sock": sock, "csid": csid, "stop": stop, "thread": t,
            "rtcp_thread": rt, "rtcp": rtcp, "stats": stats}


def close_stream(h):
    h["stop"].set(); h["thread"].join(timeout=2)
    try:
        h["svc"].invoke("com.apple.coredevice.feature.stopmediastream",
                        {"avcMediaStreamOptionClientSessionID": {"uuid": h["csid"]}},
                        action_identifier="com.apple.coredevice.action.mediastreamstop")
    except Exception:
        pass
    try:
        h["sock"].close()
    except OSError:
        pass
    h["svc"].close()
    return h["stats"]["packets"]


def record(dev_addr, our_addr, display_port, out_path, duration=10.0):
    """Negotiate + record the screen to an Annex-B .h265. Returns (packets, nal_units)."""
    nals = [0]
    f = open(out_path, "wb")
    reorder = ReorderBuffer(window=256)
    fu = bytearray()

    def _emit(payload):
        out = []
        _depacketize(payload, fu, out)
        for nal in out:
            f.write(_ANNEXB + nal); nals[0] += 1

    def on_packet(pkt):
        payload, seq, is_rtcp = _rtp_payload(pkt)
        if is_rtcp or not payload:
            return
        ordered = []
        reorder.push(seq, payload, ordered)   # release RTP payloads in sequence order
        for p in ordered:
            _emit(p)

    h = open_stream(dev_addr, our_addr, display_port, on_packet=on_packet)
    time.sleep(duration)
    packets = close_stream(h)                 # joins the drain thread first (no race)
    rem = []
    reorder.flush(rem)
    for p in rem:
        _emit(p)
    f.close()
    return packets, nals[0]


if __name__ == "__main__":
    import sys
    if len(sys.argv) < 5:
        sys.exit("usage: python3 screen.py <dev_addr> <our_addr> <display_port> <out.h265> [secs]  (tunnel up)")
    secs = float(sys.argv[5]) if len(sys.argv) > 5 else 10.0
    p, n = record(sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4], secs)
    print(f"captured {p} packets, {n} NAL units -> {sys.argv[4]}")
