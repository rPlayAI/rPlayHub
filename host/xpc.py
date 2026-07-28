#!/usr/bin/env python3
"""Minimal XPC-object + RemoteXPC-wrapper codec (Apple CoreDevice). No deps.

Wire format (all little-endian) from the RemoteXPC framework:
  wrapper : magic 0x29B00B92 | flags u32 | len u64 (=payload_len) | message_id u64 | [payload]
  payload : magic 0x42133742 | version 5 | XpcObject
  object  : type u32 | data(type)
Types: DICTIONARY 0xF000, ARRAY 0xE000, STRING 0x9000, UINT64 0x4000, INT64 0x3000,
       BOOL 0x2000, DOUBLE 0x5000, DATA 0x8000, UUID 0xA000, DATE 0x7000, NULL 0x1000.
Strings/data are 4-byte aligned; dict keys are aligned null-terminated (no length prefix).
"""
import struct, uuid

WRAPPER_MAGIC = 0x29B00B92
PAYLOAD_MAGIC = 0x42133742
PAYLOAD_VERSION = 5

# flags
F_ALWAYS_SET = 0x00000001
F_DATA_PRESENT = 0x00000100
F_WANTING_REPLY = 0x00010000
F_INIT_HANDSHAKE = 0x00400000

# types
T_NULL, T_BOOL, T_INT64, T_UINT64, T_DOUBLE = 0x1000, 0x2000, 0x3000, 0x4000, 0x5000
T_DATE, T_DATA, T_STRING, T_UUID = 0x7000, 0x8000, 0x9000, 0xA000
T_ARRAY, T_DICTIONARY = 0xE000, 0xF000


class U64(int):
    """Force UINT64 encoding."""


def _pad4(b: bytes) -> bytes:
    return b + b"\x00" * (-len(b) % 4)


# ---------------- encode ----------------
def _enc_obj(v) -> bytes:
    if v is None:
        return struct.pack("<I", T_NULL)
    if isinstance(v, bool):
        return struct.pack("<II", T_BOOL, 1 if v else 0)
    if isinstance(v, U64):
        return struct.pack("<IQ", T_UINT64, int(v))
    if isinstance(v, int):
        return struct.pack("<Iq", T_INT64, v)
    if isinstance(v, float):
        return struct.pack("<I", T_DOUBLE) + struct.pack("<d", v)
    if isinstance(v, uuid.UUID):
        return struct.pack("<I", T_UUID) + v.bytes
    if isinstance(v, str):
        sb = v.encode() + b"\x00"
        return struct.pack("<II", T_STRING, len(sb)) + _pad4(sb)
    if isinstance(v, (bytes, bytearray)):
        return struct.pack("<II", T_DATA, len(v)) + _pad4(bytes(v))
    if isinstance(v, (list, tuple)):
        return _enc_array(v)
    if isinstance(v, dict):
        entries = b"".join(_pad4(k.encode() + b"\x00") + _enc_obj(val) for k, val in v.items())
        inner = struct.pack("<I", len(v)) + entries
        return struct.pack("<II", T_DICTIONARY, len(inner)) + inner
    raise TypeError(f"cannot XPC-encode {type(v)}")


def _enc_array(v) -> bytes:
    inner = struct.pack("<I", len(v)) + b"".join(_enc_obj(x) for x in v)
    return struct.pack("<II", T_ARRAY, len(inner)) + inner


def encode_object(v) -> bytes:
    if isinstance(v, (list, tuple)):
        return _enc_array(v)
    return _enc_obj(v)


def build_wrapper(d=None, message_id=0, flags=None) -> bytes:
    if flags is None:
        flags = F_ALWAYS_SET | (F_DATA_PRESENT if d else 0)
    if d is None:
        body = struct.pack("<Q", message_id)          # no payload
    else:
        payload = struct.pack("<II", PAYLOAD_MAGIC, PAYLOAD_VERSION) + encode_object(d)
        body = struct.pack("<Q", message_id) + payload
    return struct.pack("<IIQ", WRAPPER_MAGIC, flags, len(body) - 8) + body


# ---------------- decode ----------------
class _Cur:
    def __init__(self, b):
        self.b = b; self.o = 0
    def u32(self):
        v = struct.unpack_from("<I", self.b, self.o)[0]; self.o += 4; return v
    def u64(self):
        v = struct.unpack_from("<Q", self.b, self.o)[0]; self.o += 8; return v
    def i64(self):
        v = struct.unpack_from("<q", self.b, self.o)[0]; self.o += 8; return v
    def f64(self):
        v = struct.unpack_from("<d", self.b, self.o)[0]; self.o += 8; return v
    def take(self, n):
        v = self.b[self.o:self.o + n]; self.o += n; return v
    def align4(self):
        self.o = (self.o + 3) & ~3
    def cstr_aligned(self):
        end = self.b.index(b"\x00", self.o)
        s = self.b[self.o:end].decode()
        self.o = (end + 1 + 3) & ~3
        return s


def _dec_obj(c: _Cur):
    t = c.u32()
    if t == T_NULL:
        return None
    if t == T_BOOL:
        return c.u32() != 0
    if t == T_INT64:
        return c.i64()
    if t == T_UINT64:
        return c.u64()
    if t == T_DOUBLE:
        return c.f64()
    if t == T_DATE:
        return c.u64()
    if t == T_UUID:
        return uuid.UUID(bytes=c.take(16))
    if t == T_STRING:
        n = c.u32(); s = c.take(n).split(b"\x00", 1)[0].decode(); c.align4(); return s
    if t == T_DATA:
        n = c.u32(); d = c.take(n); c.align4(); return d
    if t == T_DICTIONARY:
        _len = c.u32(); count = c.u32(); out = {}
        for _ in range(count):
            k = c.cstr_aligned(); out[k] = _dec_obj(c)
        return out
    if t == T_ARRAY:
        _len = c.u32(); count = c.u32()
        return [_dec_obj(c) for _ in range(count)]
    raise TypeError(f"unknown XPC type 0x{t:x} at {c.o}")


def parse_wrapper(b: bytes):
    """Return (flags, message_id, obj_or_None). Raises if truncated."""
    magic, flags, plen = struct.unpack_from("<IIQ", b, 0)
    if magic != WRAPPER_MAGIC:
        raise ValueError(f"bad wrapper magic 0x{magic:x}")
    off = 16
    message_id = struct.unpack_from("<Q", b, off)[0]; off += 8
    if plen == 0:
        return flags, message_id, None
    if len(b) < off + plen:
        raise ValueError("truncated wrapper payload")
    pmagic, pver = struct.unpack_from("<II", b, off)
    if pmagic != PAYLOAD_MAGIC:
        raise ValueError(f"bad payload magic 0x{pmagic:x}")
    c = _Cur(b[off + 8:])
    return flags, message_id, _dec_obj(c)


if __name__ == "__main__":  # offline round-trip self-test
    sample = {
        "MessageType": "Handshake",
        "MessagingProtocolVersion": U64(7),
        "UUID": uuid.UUID("12345678-1234-5678-1234-567812345678"),
        "Properties": {"RemoteXPCVersionFlags": U64(0x0100000000000006),
                       "SensitivePropertiesVisible": True, "Answer": -42},
        "Services": {"com.apple.coredevice.screencaptureservice": {"Port": U64(1234)}},
        "list": ["a", "bb", U64(3)],
    }
    w = build_wrapper(sample, message_id=1)
    flags, mid, obj = parse_wrapper(w)
    assert mid == 1, mid
    assert obj["MessageType"] == "Handshake"
    assert obj["MessagingProtocolVersion"] == 7
    assert str(obj["UUID"]) == "12345678-1234-5678-1234-567812345678"
    assert obj["Properties"]["RemoteXPCVersionFlags"] == 0x0100000000000006
    assert obj["Properties"]["SensitivePropertiesVisible"] is True
    assert obj["Properties"]["Answer"] == -42
    assert obj["Services"]["com.apple.coredevice.screencaptureservice"]["Port"] == 1234
    assert obj["list"] == ["a", "bb", 3]
    # no-payload control wrapper
    f2, m2, o2 = parse_wrapper(build_wrapper(None, flags=F_ALWAYS_SET | F_INIT_HANDSHAKE))
    assert o2 is None and (f2 & F_INIT_HANDSHAKE)
    print("xpc.py self-test OK — encode/decode round-trips (nested dict, array, uuid, u64, bool, int)")
