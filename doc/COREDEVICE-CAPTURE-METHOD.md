# How to capture Apple's CoreDevice offer/answer (and any RemoteXPC/media traffic)

The method used to recover Apple's real `startmediastream` offer, the device answer, and the RTCP
feedback. Reusable for any Device Hub / devicectl / Xcode CoreDevice exchange. Tools:
`host/capture-devicehub.sh` + `host/decode_devicehub.py`.

## The principle — why plain tcpdump is enough

CoreDevice's security is at the **tunnel edge, not inside it**. `remoted` establishes the tunnel
(QUIC/TLS-PSK for the network door, or the lockdown-TLS `CoreDeviceProxy` for USB), and everything after
that rides a **plain `utun` interface as cleartext IPv6**. So once the tunnel is up, there is **no TLS to
strip** — a `tcpdump` on that utun sees the real bytes:

```
     [ encrypted tunnel setup ]        <-- can't read (and don't need to)
  ── remoted brings up utunN (fd..::2, MTU 16000) ──
     RemoteXPC control  = HTTP/2 (TCP) carrying XPC dictionaries      <-- cleartext
     media plane        = RTP/HEVC + RTCP (UDP)                        <-- cleartext
```

That is the whole trick: **capture on the utun, below Apple's app crypto but above the tunnel crypto.**

## Layering you're decoding (magic numbers)

```
utun frame (DLT_NULL: 4-byte AF header)
  └ IPv6 (40-byte header; next-header 6=TCP, 17=UDP)
      ├ TCP → HTTP/2
      │        preface "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
      │        frames: <u24 len><u8 type><u8 flags><u32 stream_id>  (type 0 = DATA)
      │          └ DATA payload = XPC wrapper stream
      │               wrapper: magic 0x29B00B92 | flags u32 | len u64 | msg_id u64 | payload
      │               payload: magic 0x42133742 | version 5 | XPC object  (all little-endian)
      │                 XPC dict → the CoreDevice invocation / answer
      │                   CoreDevice.input.negotiatorOffer = binary plist
      │                     avcMediaStreamNegotiatorMediaBlob = zlib(level 9) protobuf  ← the prize
      └ UDP → RTP (PT 96–127) and RTCP (PT 200–223: SR/RR/SDES/APP…)
```

## Capture procedure (order matters)

`startmediastream` fires the instant "View Screen" opens, so tcpdump must already be running:

1. In Xcode/Device Hub, **connect to the device but do NOT open View Screen yet** — that brings up the
   tunnel utun.
2. Find that utun: the one with a `fd..` ULA address and/or `mtu 16000`
   (`capture-devicehub.sh` does this automatically).
3. Start capture (needs sudo, cleartext so `-s 0`):
   ```
   sudo tcpdump -i utunN -w devicehub.pcap -s 0
   ```
4. **Now** open View Screen, mirror ~15 s, stop tcpdump.
5. `python3 host/decode_devicehub.py devicehub.pcap`

## Decode pipeline (what `decode_devicehub.py` does)

1. **pcap** → parse classic pcap; strip `DLT_NULL` 4-byte AF header → IPv6/IPv4.
2. **TCP reassembly** — collect payloads per 4-tuple, order by sequence number, concatenate.
3. **HTTP/2 un-framing** — skip the preface, walk `<u24 len><type><flags><u32 sid>`, and **concatenate
   DATA-frame payloads per stream_id**. This is essential: a big invocation (the offer) spans several DATA
   frames, so a raw scan sees it chopped by 9-byte frame headers and fails. Reassemble first.
4. **Scan for XPC** — find the wrapper magic `92 0b b0 29`, `parse_wrapper` from there (the wrapper's own
   length delimits it), decode the XPC dict with the project's `xpc.py`.
5. **Select** — the invocation is the dict whose `CoreDevice.actionIdentifier` ends in `mediastreamstart`
   (match on action, not featureIdentifier — Apple leaves featureIdentifier empty); the answer is the dict
   with `CoreDevice.output.connection.streamConfig`.
6. **mediaBlob** — `plistlib.loads(negotiatorOffer)` → `zlib.decompress(avcMediaStreamNegotiatorMediaBlob)`
   → walk the protobuf (generic varint / length-delim / fixed) to a field tree you can diff.
7. **RTCP** — over UDP datagrams whose first packet has version 2 and PT 200–223, walk the compound packet;
   count SR/RR/SDES and APP sub-types. (Guard the PT range or RTP leaks in — RTP is also "version 2".)

## Gotchas that cost time

- **Reassemble HTTP/2 before scanning** — otherwise the offer (multi-frame) never decodes; only small
  single-frame messages (the answers) do.
- **Match the invocation on `actionIdentifier`**, not `featureIdentifier` (Apple sends the latter empty).
- **zlib level matters** for *sending* an offer (device rejects non-9), but for *reading* it's just
  `zlib.decompress`.
- **RTP vs RTCP** share version 2 — restrict RTCP parsing to PT 200–223.
- **Tunnel utun number changes** across reconnects — re-find it (ULA `fd..` / mtu 16000) each session.

## Alternative captures (when tcpdump isn't enough)

- **lldb hook** (`reference/capture/cdcap2.py`): breakpoint `nw_connection_send` / the receive completion
  in `remoted`/`DeviceHub` to grab the cleartext stream in-process — useful if you can't isolate the utun
  or want it keyed to a process. Same XPC/HTTP2 decode after.
- **Static** (`ipsw class-dump`/strings on `reference/binaries/AVConference`) for a field the wire didn't
  show — AVConference is the source of truth for the whole media negotiation (see the "avc" prefix).
