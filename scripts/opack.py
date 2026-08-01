"""OPACK decoder, transcribed from deps/AccessorySDK/Support/OPACKUtils.c.

OPACK is how every RemotePairing control-channel message is serialised
(doc/REMOTEPAIRING-PROTOCOL.md: "all the Codable structs marshal through
OPACK"). The tag table below is copied from the vendored encoder's own header
constants rather than reconstructed from memory, because guessing wire formats
is the specific mistake that has cost this project the most time.

    from opack import decode
    obj, consumed = decode(buf)
"""
import struct
import uuid

TERMINATOR = 0x03


def decode(b, off=0, depth=0):
    """Decode one OPACK object at `off`. Returns (value, new_offset)."""
    if depth > 32:
        raise ValueError("OPACK nesting too deep")
    if off >= len(b):
        raise ValueError("OPACK truncated")
    t = b[off]
    off += 1

    if t == 0x01:
        return True, off
    if t == 0x02:
        return False, off
    if t == 0x04:
        return None, off
    if t == 0x05:
        return uuid.UUID(bytes=b[off:off + 16]), off + 16
    if t == 0x06:
        return ("date", struct.unpack_from("<d", b, off)[0]), off + 8

    # 0x07 = -1, 0x08 = 0, ... 0x2F = 39
    if 0x07 <= t <= 0x2F:
        return t - 0x08, off

    if t == 0x30:
        return b[off], off + 1
    if t == 0x31:
        return struct.unpack_from("<H", b, off)[0], off + 2
    if t == 0x32:
        return struct.unpack_from("<I", b, off)[0], off + 4
    if t == 0x33:
        return struct.unpack_from("<Q", b, off)[0], off + 8
    if t == 0x34:
        return int.from_bytes(b[off:off + 16], "little"), off + 16
    if t == 0x35:
        return struct.unpack_from("<f", b, off)[0], off + 4
    if t == 0x36:
        return struct.unpack_from("<d", b, off)[0], off + 8

    # strings
    if 0x40 <= t <= 0x60:
        n = t - 0x40
        return b[off:off + n].decode("utf-8", "replace"), off + n
    if t in (0x61, 0x62, 0x63, 0x64):
        w = {0x61: 1, 0x62: 2, 0x63: 4, 0x64: 8}[t]
        n = int.from_bytes(b[off:off + w], "little")
        off += w
        return b[off:off + n].decode("utf-8", "replace"), off + n
    if t == 0x6F:
        e = b.index(0, off)
        return b[off:e].decode("utf-8", "replace"), e + 1

    # data
    if 0x70 <= t <= 0x90:
        n = t - 0x70
        return b[off:off + n], off + n
    if t in (0x91, 0x92, 0x93, 0x94):
        w = {0x91: 1, 0x92: 2, 0x93: 4, 0x94: 8}[t]
        n = int.from_bytes(b[off:off + w], "little")
        off += w
        return b[off:off + n], off + n
    if t == 0x9F:
        chunks = b""
        while b[off] != TERMINATOR:
            v, off = decode(b, off, depth + 1)
            chunks += v
        return chunks, off + 1

    # UIDs
    if 0xA0 <= t <= 0xC0:
        return ("uid", t - 0xA0), off
    if t in (0xC1, 0xC2, 0xC3, 0xC4):
        w = {0xC1: 1, 0xC2: 2, 0xC3: 3, 0xC4: 4}[t]
        return ("uid", int.from_bytes(b[off:off + w], "little")), off + w

    # arrays
    if 0xD0 <= t <= 0xDE:
        out = []
        for _ in range(t - 0xD0):
            v, off = decode(b, off, depth + 1)
            out.append(v)
        return out, off
    if t == 0xDF:
        out = []
        while b[off] != TERMINATOR:
            v, off = decode(b, off, depth + 1)
            out.append(v)
        return out, off + 1

    # dictionaries
    if 0xE0 <= t <= 0xEE:
        out = {}
        for _ in range(t - 0xE0):
            k, off = decode(b, off, depth + 1)
            v, off = decode(b, off, depth + 1)
            out[k if isinstance(k, (str, int)) else str(k)] = v
        return out, off
    if t == 0xEF:
        out = {}
        while b[off] != TERMINATOR:
            k, off = decode(b, off, depth + 1)
            v, off = decode(b, off, depth + 1)
            out[k if isinstance(k, (str, int)) else str(k)] = v
        return out, off + 1

    raise ValueError(f"unknown OPACK tag 0x{t:02x} at {off-1}")


def looks_like_opack(b):
    """Cheap plausibility test: does the whole buffer decode as one object?"""
    try:
        v, n = decode(b)
        return n == len(b)
    except Exception:
        return False


def pretty(o, indent=0):
    pad = "  " * indent
    if isinstance(o, dict):
        return "{\n" + "".join(
            f"{pad}  {k!r}: {pretty(v, indent+1)}\n" for k, v in o.items()) + pad + "}"
    if isinstance(o, list):
        return "[\n" + "".join(f"{pad}  {pretty(v, indent+1)}\n" for v in o) + pad + "]"
    if isinstance(o, bytes):
        return f"<{len(o)} bytes> {o[:32].hex()}" + ("..." if len(o) > 32 else "")
    return repr(o)
