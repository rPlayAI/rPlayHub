# cdhost — CoreDevice host in C

The C side of rplay-hub. Same protocol as the Python harness in `../host/`, verified live against
the same iPhone (iPhone13,3, iOS 26.5.2).

C and Swift are the shipping languages: **C** for the portable protocol core, **Swift** for the
macOS app (the Device Hub-style "View Screen" window). The Python in `../host/` is an
experiment and verification harness only — it is where wire formats get proven before being
committed to in C or Swift.

## Status

| layer | C | notes |
|---|---|---|
| 0 usbmux | ✅ `cdhost.c` | list devices, connect-to-port |
| 1 lockdown | ✅ `cdhost.c` | QueryType, GetValue (real device info) |
| 1.5 TLS session | ✅ `tls_openssl.c` | client-cert from the usbmuxd pair record; `tls_mbedtls.c` builds but see below |
| 2 CoreDevice tunnel | ✅ `cdhost.c` | `CDTunnel` magic + u16 + JSON handshake |
| 3a utun + packet pump | ✅ `cdhost.c` | needs root; RSD then reachable with an ordinary socket |
| 3 RemoteXPC + XPC codec | ✅ `../core/rp_remotexpc.c` | handshake is **byte-identical** to the Python (195 bytes) |
| 3b RSD service discovery | ✅ `cdhost.c` | one handshake returns the ~85-entry service map |
| 4 screen / HID services | TODO | port `../host/screen.py`, `../host/hid.py` |
| 2b RemotePairing (direct wifi door) | TODO | the big one — see `../deps/AccessorySDK/PROVENANCE.md` |

The protocol core is compiled from `../core`, not duplicated here: `rp_xpc.c`, `rp_http2.c` and
`rp_remotexpc.c` contain no sockets, no allocation and no Apple headers, which is what lets the
same files build for Linux and Windows. `cdhost.c` holds only the platform plumbing — usbmuxd,
OpenSSL, utun.

`make -C ../core test` verifies the whole protocol layer **with no device attached**, including
the RemoteXPC opening exchange byte-for-byte against the Python. That matters because the
handshake's ordering and flags come from a captured session rather than from a specification: one
wrong byte and the device closes the connection without explanation.

## Support SDK

The pieces we need are vendored in-repo at **`../deps/AccessorySDK/`** (`Support` + `External`,
~5 MB). That is the relevant SDK — *not* CarPlay, which was only where the earlier experiments
happened.

`../deps/AccessorySDK/PROVENANCE.md` lists where it came from and which file covers which need:
SRP-6a, the pair-setup/pair-verify state machine (`PairingUtils.c`), OPACK, TLV8,
ChaCha20-Poly1305, Curve25519/Ed25519, LibTomMath, BonjourBrowser, and CFLite binary plists. It also
points at `~/iPhoneMirroring/iphone-mirror-client/src/rapport.c`, which is working C for the modern
Pair-Verify flow and the better primary crib.

## Build

- **macOS (fast verify):** `make` → system CoreFoundation + Homebrew OpenSSL, then `./cdhost`.
- **Linux/OEM:** `make` → compiles against the vendored CFLite/crypto headers.

## Wire notes

Same ground truth as `../host/README.md`: usbmux unix-socket plist framing, lockdown
`<u32 be len><XML plist>`, usbmux `PortNumber` in network byte order.

## TLS backend

Selectable at build time, because the choice is not obvious and because swapping it is the way to
*test* a suspicion rather than argue about it:

    make                # OpenSSL (default)
    make TLS=mbedtls    # vendored ../deps/mbedtls, built from source

mbedtls is what `~/rplay` and `~/carplay-dev` use, and it is what makes the Linux and Windows ports
tractable — no system library to find, no version skew, no shipping question.

**It does not work yet for lockdown.** Measured against a real pair record, mbedtls rejects Apple's
host certificate with `-0x23E0` = `X509_INVALID_NAME` + `ASN1_OUT_OF_DATA`; the private key parses
fine. mbedtls is stricter than OpenSSL about the subject/issuer name encoding Apple uses. No config
flag changes that — it needs a permissive parse or converting the identity before handing it over.
So OpenSSL remains the default and mbedtls is a working skeleton with one known blocker.

Worth knowing for the ports: this blocker is in the **lockdown** path only. The tunnel, RemoteXPC
and every service call above it are plain sockets and carry no TLS of their own.
