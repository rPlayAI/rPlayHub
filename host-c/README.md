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
| 1.5 TLS session | TODO | OpenSSL client-cert; `MFiClientPlatformOpenSSL.c` is the model |
| 2 CoreDevice tunnel | TODO | `CDTunnel` magic + u16 + JSON; trivial, Python is the reference |
| 3 RemoteXPC + XPC codec | TODO | HTTP/2 framing + the XPC object codec; port `../host/rplayhub/wire/` |
| 4 screen / HID services | TODO | port `../host/screen.py`, `../host/hid.py` |
| 2b RemotePairing (direct wifi door) | TODO | the big one — see `../deps/AccessorySDK/PROVENANCE.md` |

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
