# Building rPlayHub

Two products, built separately:

| | what | build |
|---|---|---|
| **`cdhost`** | the daemon — C, talks to the phone, needs root | `make -C host-c` |
| **`rPlayHub.app`** | the player — Swift, normal user process | `xcodebuild` (see below) |

They talk over `127.0.0.1:9876` (JSON lines) and `:9877` (Annex-B video), so either side can be
rebuilt and restarted without the other. `scripts/live.sh` builds and runs both together and is
the usual way in.

**Linux** builds the same daemon plus the SDL2 + Dear ImGui GUI in `client-c/`, and packages
both as a `.deb`. The dependencies, the three `scripts/` that build FFmpeg (RVRA-patched) and the
libimobiledevice stack, the `make` lines and `scripts/package-deb.sh` are all in
[doc/LINUX.md](doc/LINUX.md).

**Windows** builds the SDL2 + Dear ImGui GUI in `client-c/` using MSVC and the RVRA-patched
FFmpeg library. See [doc/WINDOWS.md](doc/WINDOWS.md) for prerequisites and build instructions.

---

## The daemon

```sh
make -C host-c            # -> host-c/cdhost
sudo ./host-c/cdhost      # root: it creates a utun
```

Requires OpenSSL headers. On macOS with Homebrew that is `brew install openssl@3`; the Makefile
looks in `/opt/homebrew/opt/openssl@3` and `OPENSSL=/path` overrides it. `zlib` comes with the
system.

Only the tunnel needs root. Nothing else does, which is why the app is a separate process — see
"Why two processes" below.

### The Developer Disk Image

After every phone reboot the engine mounts Apple's Developer Disk Image, and it needs the image
files on disk. A Mac with Xcode has them; on Linux (or a Mac without Xcode) fetch them once:

```sh
./scripts/fetch-ddi.sh    # -> ~/.local/share/rplayhub/iOS_DDI, where the engine looks
```

They are Apple's, so they are downloaded rather than kept in this repository. Details, other
sources and troubleshooting: [doc/DDI.md](doc/DDI.md).

### TLS backend

```sh
make -C host-c                # OpenSSL (default)
make -C host-c TLS=mbedtls    # vendored deps/mbedtls, compiled from source
```

mbedtls is what `~/rplay` and `~/carplay-dev` use and needs no system library, which is what will
make the Linux and Windows ports tractable. **It does not work yet**: it rejects Apple's pair-record
certificate with `-0x23E0` (`X509_INVALID_NAME` + `ASN1_OUT_OF_DATA`) because it is stricter than
OpenSSL about the subject/issuer name encoding Apple uses. The private key parses fine. No config
flag changes this — it needs a permissive parse or converting the identity first. The blocker is
confined to lockdown; everything above it is plain sockets.

### Tests, with no device attached

```sh
make -C core test
```

Verifies the protocol layer against the Python that is proven on real phones, and against a real
captured session:

* RemoteXPC opening exchange — byte-identical to `rplayhub.wire.remotexpc`
* media-stream offer — byte-identical, blob and full plist
* RTP depacketization — replays a captured Device Hub session and byte-compares the Annex-B output
* multi-frame reassembly, XPC codec, video stream assembly

These import the actual Python modules rather than a copy of them. That distinction is not
pedantic: an earlier version of the handshake check compared against a reimplementation written
alongside the C, so it validated an assumption instead of testing it, and passed while the two
differed by 15 MB of HTTP/2 flow-control window.

---

## The app

```sh
xcodebuild -project app/rPlayHub.xcodeproj -scheme rPlayHub -configuration Debug \
    -derivedDataPath build/DerivedData \
    CODE_SIGN_STYLE=Manual CODE_SIGN_IDENTITY=<sha1-of-a-signing-identity> \
    DEVELOPMENT_TEAM="" PROVISIONING_PROFILE_SPECIFIER="" build
```

Targets macOS 13.0, bundle id `com.rplay.rplayhub`, not sandboxed.

### Two things that are easy to get wrong

**Never build as root.** `codesign` cannot reach the user's login keychain, so the private key is
unavailable and it fails with `errSecInternalComponent` / "unable to build chain to self-signed
root". `live.sh` runs under `sudo` for the engine and drops to `$SUDO_USER` for the build; if you
invoke `xcodebuild` yourself, do it as yourself.

**Select the identity by hash, not by name.** The same certificate name can exist several times in
a keychain, revoked and valid, and xcodebuild resolving one by name may pick a revoked one and fail
the build:

```sh
security find-identity -v -p codesigning | grep -v CSSMERR_TP_CERT_REVOKED | grep -oE "[0-9A-F]{40}" | head -1
```

Ad-hoc signing (`CODE_SIGN_IDENTITY="-"`) builds and runs, but the code identity changes on every
build, so TCC treats each build as a different app and a granted camera permission never persists.
That matters only for the USB capture path.

---

## Both at once

```sh
sudo ./scripts/live.sh
```

Builds the app as the invoking user, launches it into the user's GUI session, then runs the Python
engine as root. Overrides:

```sh
RPLAYHUB_SIGN_IDENTITY=<sha1>     # pick the signing identity
RPLAY_KEYFRAME_EVERY_S=1.5        # how often to request a keyframe (0 disables)
RPLAYHUB_USB_CAPTURE=1            # opt in to the CoreMediaIO capture path
RPLAYHUB_VIDEO=direct|proxy|auto  # how the player gets video (default auto)
RPLAY_NO_VIDEO_PROXY=1            # daemon: do not host the Annex-B fan-out at all
```

Pass these **after** `sudo` — `sudo` strips the environment, so `VAR=x sudo ...` silently tests the
default instead.

### Why launching matters

`live.sh` uses `launchctl asuser` rather than plain `sudo -u`. A GUI process has to be inside the
user's login session for TCC to show a permission prompt at all; launched any other way from a root
script it is denied silently and permanently, and no amount of `tccutil reset` helps because the
request never reaches the user.

---

## Why two processes

Only the tunnel needs root — it creates a `utun` interface. Everything else (decoding, the window,
input handling) is ordinary user code, and the split keeps the privileged part small and separate.
It also means the player can be rebuilt, crashed and restarted without disturbing a live device
session, and the same daemon serves either engine: `cdhost` and the Python `host/mirror.py` speak
an identical contract, so the app cannot tell which it reached.

---

## Two ways the player gets video

**Direct (default when available).** The player asks the daemon for the tunnel addresses via
`tunnel_info`, then talks to the phone itself: negotiates the stream, binds its own RTP port, and
decodes. The daemon handles no video packets at all — confirm with `stream_info`, which reports
`streaming: false` and `rtp_packets: 0` while video is playing.

**Proxy (fallback).** The daemon depacketizes and serves Annex-B on `:9877`; the player reads it
over loopback. Required for the Python engine, which offers no `tunnel_info`, and still the right
shape for anything that is not the app: `ffplay`, a recorder, a second viewer.

```sh
RPLAYHUB_VIDEO=direct   # take RTP ourselves; fail rather than fall back
RPLAYHUB_VIDEO=proxy    # force the loopback route
RPLAYHUB_VIDEO=auto     # direct when tunnel_info exists (default)
```

Choose explicitly when testing. A silent fallback makes "which path am I actually measuring"
unanswerable, which matters because the two behave differently under load.

Direct is preferred because of coupling, not the extra hop. A proxy makes the daemon a producer
and the player a consumer, and that is unsafe for a real-time source that cannot retransmit: a
player reading slowly applies backpressure that reaches the RTP thread, packets are dropped, and
since the device sends one IDR per session the corruption is permanent — with nothing pointing at
the player as the cause. Owning the socket removes the second consumer rather than mitigating it,
and it also deletes the fan-out, the caches that let a late viewer decode, and the single viewer
slot that a stray client can take.
