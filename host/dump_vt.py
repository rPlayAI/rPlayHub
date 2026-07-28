"""lldb hook: dump the exact HEVC bytes DeviceHub feeds VideoToolbox.

Breakpoints VTDecompressionSessionDecodeFrame (public VT API). For each call it pulls the
CMSampleBuffer's CMBlockBuffer (the encoded access unit, hvcC length-prefixed) + the VPS/SPS/PPS
from the format description, and writes an Annex-B .h265 = exactly what a CORRECT pipeline decodes.

Diff that against our network reconstruction and the difference IS our depacketization bug.
Requires SIP off (attach to DeviceHub). See run-vt-dump.sh.
"""
import lldb, os, struct

OUT = os.environ.get("VTDUMP_OUT", os.path.expanduser("~/dh_decoder_input.h265"))
st = {"f": None, "params_done": False, "n": 0}


def _f():
    if st["f"] is None:
        st["f"] = open(OUT, "wb")
    return st["f"]


def _eval(frame, expr):
    o = lldb.SBExpressionOptions()
    o.SetIgnoreBreakpoints(True)
    o.SetTimeoutInMicroSeconds(2000000)
    v = frame.EvaluateExpression(expr, o)
    return v.GetValueAsUnsigned() if v.IsValid() and v.GetError().Success() else 0


def _read(proc, addr, n):
    err = lldb.SBError()
    d = proc.ReadMemory(addr, n, err)
    return d if err.Success() else None


def _u64(proc, addr):
    err = lldb.SBError()
    return proc.ReadUnsignedFromMemory(addr, 8, err)


def on_decode(frame, loc, d):
    # C func VTDecompressionSessionDecodeFrame(session, sampleBuffer, ...) -> sbuf = x1
    return _dump(frame, frame.FindRegister("x1").GetValueAsUnsigned(), "VTDecode")


def on_enqueue(frame, loc, d):
    # ObjC -[... enqueueSampleBuffer:] -> self=x0 _cmd=x1 sampleBuffer=x2
    return _dump(frame, frame.FindRegister("x2").GetValueAsUnsigned(), "enqueue")


def _dump(frame, sbuf, who):
    if not sbuf:
        return False
    if not st.get("api"):
        st["api"] = who
        print(f"[vtdump] decode path = {who}")
    proc = frame.GetThread().GetProcess()

    # VPS/SPS/PPS from the format description — once
    if not st["params_done"]:
        fmt = _eval(frame, f"(void*)CMSampleBufferGetFormatDescription((void*){sbuf})")
        if fmt:
            scratch = _eval(frame, "(void*)malloc(64)")
            if scratch:
                for i in range(3):   # 0=VPS 1=SPS 2=PPS
                    _eval(frame, f"(int)CMVideoFormatDescriptionGetHEVCParameterSetAtIndex("
                                 f"(void*){fmt},{i},(const unsigned char**){scratch},"
                                 f"(unsigned long*)({scratch}+8),(unsigned long*)({scratch}+16),"
                                 f"(int*)({scratch}+24))")
                    ptr = _u64(proc, scratch)
                    size = _u64(proc, scratch + 8)
                    if ptr and 0 < size < 4096:
                        ps = _read(proc, ptr, size)
                        if ps:
                            _f().write(b"\x00\x00\x00\x01" + ps)
                _eval(frame, f"(void)free((void*){scratch})")
                st["params_done"] = True

    # the encoded access unit (hvcC = length-prefixed NALs)
    bb = _eval(frame, f"(void*)CMSampleBufferGetDataBuffer((void*){sbuf})")
    if bb:
        ln = _eval(frame, f"(unsigned long)CMBlockBufferGetDataLength((void*){bb})")
        if 0 < ln < (32 << 20):
            p = _eval(frame, f"(void*)malloc({ln})")
            if p:
                _eval(frame, f"(int)CMBlockBufferCopyDataBytes((void*){bb},0,{ln},(void*){p})")
                data = _read(proc, p, ln)
                _eval(frame, f"(void)free((void*){p})")
                if data:
                    i = 0                                  # hvcC length-prefixed -> Annex-B
                    while i + 4 <= len(data):
                        nlen = struct.unpack_from(">I", data, i)[0]
                        i += 4
                        if nlen == 0 or i + nlen > len(data):
                            break
                        _f().write(b"\x00\x00\x00\x01" + data[i:i + nlen])
                        i += nlen
                    st["n"] += 1
    _f().flush()
    if st["n"] in (1, 10, 50) or st["n"] % 100 == 0:
        print(f"[vtdump] {st['n']} frames -> {OUT}")
    return False   # keep running
