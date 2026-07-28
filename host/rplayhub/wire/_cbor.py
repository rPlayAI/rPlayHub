#!/usr/bin/env python3
"""Minimal CBOR (RFC 8949) encode/decode — enough for the CoreDevice tunnel handshake.
Supports: unsigned/negative int, byte/text string, array, map, bool, null, float64.
No external deps."""
import struct


def dumps(obj) -> bytes:
    out = bytearray()
    _enc(obj, out)
    return bytes(out)


def _enc_head(major, n, out):
    if n < 24:
        out.append((major << 5) | n)
    elif n < 0x100:
        out.append((major << 5) | 24); out.append(n)
    elif n < 0x10000:
        out.append((major << 5) | 25); out += struct.pack(">H", n)
    elif n < 0x100000000:
        out.append((major << 5) | 26); out += struct.pack(">I", n)
    else:
        out.append((major << 5) | 27); out += struct.pack(">Q", n)


def _enc(obj, out):
    if obj is True:
        out.append(0xF5)
    elif obj is False:
        out.append(0xF4)
    elif obj is None:
        out.append(0xF6)
    elif isinstance(obj, int):
        if obj >= 0:
            _enc_head(0, obj, out)
        else:
            _enc_head(1, -1 - obj, out)
    elif isinstance(obj, float):
        out.append(0xFB); out += struct.pack(">d", obj)
    elif isinstance(obj, (bytes, bytearray)):
        _enc_head(2, len(obj), out); out += obj
    elif isinstance(obj, str):
        b = obj.encode(); _enc_head(3, len(b), out); out += b
    elif isinstance(obj, (list, tuple)):
        _enc_head(4, len(obj), out)
        for x in obj:
            _enc(x, out)
    elif isinstance(obj, dict):
        _enc_head(5, len(obj), out)
        for k, v in obj.items():
            _enc(k, out); _enc(v, out)
    else:
        raise TypeError(f"cannot CBOR-encode {type(obj)}")


def loads(data: bytes):
    val, off = _dec(memoryview(data), 0)
    return val


def _dec_head(mv, off):
    ib = mv[off]; off += 1
    major, info = ib >> 5, ib & 0x1F
    if info < 24:
        return major, info, off
    if info == 24:
        return major, mv[off], off + 1
    if info == 25:
        return major, struct.unpack_from(">H", mv, off)[0], off + 2
    if info == 26:
        return major, struct.unpack_from(">I", mv, off)[0], off + 4
    if info == 27:
        return major, struct.unpack_from(">Q", mv, off)[0], off + 8
    raise ValueError(f"bad CBOR additional info {info}")


def _dec(mv, off):
    major, n, off = _dec_head(mv, off)
    if major == 0:
        return n, off
    if major == 1:
        return -1 - n, off
    if major == 2:
        return bytes(mv[off:off + n]), off + n
    if major == 3:
        return bytes(mv[off:off + n]).decode(), off + n
    if major == 4:
        arr = []
        for _ in range(n):
            v, off = _dec(mv, off); arr.append(v)
        return arr, off
    if major == 5:
        d = {}
        for _ in range(n):
            k, off = _dec(mv, off); v, off = _dec(mv, off); d[k] = v
        return d, off
    if major == 7:
        if n == 20:
            return False, off
        if n == 21:
            return True, off
        if n == 22:
            return None, off
        if n == 27:
            return struct.unpack_from(">d", mv, off)[0], off + 8
    raise ValueError(f"unsupported CBOR major {major} info {n}")
