# Device Hub parity — what a drop-in replacement still needs

Written 2026-08-22, the day live mirroring was re-verified on iPhone 13 Pro / iOS 27 over wifi
after the bitrate-experiment build. The goal: rPlayHub replaces Apple's Device Hub for everything
it does, with no feature a user would notice missing.

**Epistemic note.** Device Hub's exact surface is known to us from three kinds of evidence: our
captures of its sessions (`doc/DEVICEHUB-CAPTURE-FINDINGS.md`), its UI as cloned in
`app/README.md`, and references in `doc/RSD-SERVICES.md`. Rows below are marked **confirmed**
(those sources), or **inferred** (Device Hub plausibly does this; not verified against it).
On 2026-08-23 both apps were open side by side for the first time (Device Hub 's window was
screenshotted with the iPhone12 selected): its inspector has **Info / Apps / Profiles** tabs, its
sidebar lists physical devices and *used* simulators under one "Available" heading, and its
device pane is a device picture with a **View Screen** button. The right-click menus were not
yet compared item by item -- that remains the cheapest way to finish this table.

## Where we stand

**Confirmed working today** (2026-08-02 sessions, re-verified 2026-08-22): device sidebar with
search and click-to-bind · live View Screen at full quality (click = tap, drag = swipe) · Home
gesture · screenshot · recording · Diagnostics tab (model, build, ECID, serial, battery, storage —
answers even while Device Hub holds the device) · simulators section · wifi + USB transports ·
engine-side multi-viewer broadcast on :9877.

**Added 2026-08-23** (all verified live on the iPhone 13 Pro / iOS 27 and iPhone 12 / iOS 26.5
unless noted): Restart / Shutdown / Sleep (strip buttons and both right-click menus, each
confirming first; Sleep not yet fired from the GUI) · **Apps** inspector tab: list (301 apps),
double-click / Launch, Terminate · **Console** inspector tab: live syslog with filter ·
simulator list filtered to used ones, matching Device Hub's 9-of-37 · daemon `quit` method and
owned SIGINT/SIGTERM (a rebound daemon had ignored Ctrl-C) · 5 s connect deadline on the
display session (a stale tunnel address froze the GUI for the kernel's 75 s).

**Architecture difference worth knowing:** the app holds its own displayservice TCP session
directly over the tunnel interface (`DirectStream.swift`; verified by lsof during today's live
run), so the engine's `stream_info` counters read zero while mirroring works. Anyone debugging
"no video" from the API side must know the counters do not cover the app's own path.

## Feature table

| Feature | Device Hub | rPlayHub | Path / service | Blocker |
|---|---|---|---|---|
| Device list, search, bind | confirmed | ✅ | usbmuxd list + lockdown names for every device + select_device | none (unbound devices were UDID-titled until 2026-08-23) |
| Simulators: used ones only | **confirmed** (screenshot) | ✅ 2026-08-23 | simctl + `data/Containers` present | none |
| Inspector tabs Info / Apps / Profiles | **confirmed** (screenshot) | all three ✅ (+ we add Controls, Files, Console) | `misagent` / `MCInstall` for profiles | none (Profiles built 2026-08-23) |
| Right-click menu items | inferred | ours has 11 items incl. power group | — | not yet compared item by item |
| Live screen view | confirmed (capture) | ✅ | displayservice | none |
| Click/drag → tap/swipe | inferred | ✅ | universalhidservice 257 | none |
| Home button | inferred | ✅ | bottom-edge gesture | none |
| Screenshot | inferred | ✅ | screencaptureservice | none |
| Recording | inferred | ✅ | stream capture | none |
| Device info panel | confirmed | ✅ | lockdown over usbmuxd | none |
| Restart / Shutdown / Sleep | inferred | ✅ built 2026-08-23 (wire proven from Python; GUI buttons not yet fired) | `diagnostics_relay.shim.remote` | none |
| App list (launch/terminate) | inferred | ✅ verified live 2026-08-23 | list: `installation_proxy` Browse; launch/terminate: `coredevice.appservice` | none |
| Console / live log panel | inferred | ✅ verified live 2026-08-23 | `syslog_relay.shim.remote` | `os_trace_relay` (structured) still needs its binary format |
| Crash reports panel | **confirmed** (`doc/RSD-SERVICES.md`) | ✅ 2026-08-23 (Files tab → Crash Reports, Export All) | `crashreportmover` + AFC over `crashreportcopymobile` | none |
| Clipboard sync | inferred | ❌ | `coredevice.pasteboardservice` | message format not decoded |
| Physical keyboard input | inferred | ❌ | HID surface 512 (`mainKeyboard`) | needs one capture of Device Hub typing -- see "Keyboard: what is known" |
| Lock / volume / Siri buttons | inferred | ❌ | HID surface 1026 (`mainScreenButtons`) | same capture as the keyboard row |
| Pair / Unpair / trust prompt | inferred | ❌ | `dt.remotepairingdeviced.lockdown` + `lockdown.remote.untrusted` | RemotePairing handshake designed but not built (`doc/REMOTEPAIRING-PROTOCOL.md`) |
| File browsing (Media partition) | inferred | ✅ 2026-08-23 (Files tab; list + pull, no write yet) | `afc.shim.remote` | none |
| Backup / restore | inferred | ❌ | `mobilebackup2.shim.remote` | classic protocol; large surface |
| Software update / activation state | inferred | ❌ | `mobileactivationd.shim.remote` | unverified |
| Rotate physical device | n/a | n/a | — | **impossible**: no orientation/motion service exists (verified against all 85 services) |

**2026-08-23 build note.** The power, app and console rows were implemented in one pass: engine
methods `device_action`, `list_apps` / `list_processes` / `launch_app` / `terminate_app`, the
streaming `syslog`, and `quit` (`app/api/PROTOCOL.md`); app side, three power buttons on the
control strip and in both right-click menus (each confirms), two new inspector tabs (Apps,
Console), and the simulator filter. Both targets build clean. Things learned the hard way that
day, each now fixed: the inspector's 260-pt width is priority 700, so any new panel content must
yield horizontally or it pushes the screen out of the window; a table cell's labels inside an
`NSStackView` are not `subviews` of the cell; the Apps tab must refetch when the engine
connection changes, because a reconnect usually means a different device. The diagnostics_relay wire
was proven by `host/diagnostics_relay.py` against the live phone; live against the iOS 26.5 iPhone 12 the same day:
syslog streams, `launch_app` opened Settings, `list_processes` answers, and the app list comes
from `installation_proxy` Browse (320 apps in 0.2 s) because appservice's `listapps` validates
its input (`requireContainerAccess`, `includeAppGroupIdentifiers`, `includeContainerPaths` are
all required) and then never answers -- `host/appservice.py apps` reproduces the stall. The
power actions were not fired from the app yet (Sleep is the benign one to try first).

## Priorities if parity is the goal

Ordered by gap closed per unit work:

1. ~~Restart / Shutdown / Sleep~~ — built.
2. ~~App list~~ — built (via `coredevice.appservice`, which also gives launch/terminate; icons
   would need `coredevice.iconservice`).
3. ~~Syslog console~~ — built for syslog proper.
4. **Crash reports** — small step past syslog once afc-style transfer exists (which file browsing also needs: two features, one substrate).
5. **Keyboard input** — needs one decoded HID report format (surface 512); unlocks typing in the View Screen.
6. **Pair / Unpair** — biggest single lift; blocked on building the RemotePairing handshake that `doc/REMOTEPAIRING-PROTOCOL.md` already specifies down to wire messages. Note this handshake is ALSO the prerequisite for remote-device support (`doc/REMOTE-SUPPORT.md`) — doing it serves both goals.

## Keyboard: what is known (2026-08-23)

Surface 512 is not a USB keyboard. `dtuhidd` on the phone decodes every report by its first byte as
an Apple "gesture" report ID -- the touchscreen's is 9 (58 bytes, which we already send and which
Apple's own capture confirms byte for byte) -- and it says so in syslog, which makes the phone an
oracle: `No gesture for report ID n` for unknown IDs, `Failed to decode report for gesture` for
known IDs given the wrong shape, silence for a clean decode. Probing 1-24 with 16 zero bytes:
IDs **2, 3, 4, 7, 14, 15, 16, 17** decode; **1, 5, 9, 11, 12, 13, 18, 19** are known but want
another length; the rest are unknown. Boot-keyboard layouts (with and without a report-ID byte)
and each accepted ID filled with keycode 0x04 typed nothing into a focused Safari address bar.
Apple's existing capture (`reference/captures/devicehub-iphone13-ios27.pcap`) holds no keyboard
traffic, only touchscreen (257/0x09) and 152 reports on surface 1281 (ID 0x13, 19 bytes -- the
pointer/gesture surface, also not decoded yet).

**The five-minute experiment that finishes this:** with Device Hub connected to the iPhone 13 (not
yet viewing), `sh host/capture-devicehub.sh <Device Hub's utun>` (cdhost has its own utun, so name
the interface), then View Screen, click a text field, type `abc`, press a volume button, and stop.
`python3 host/decode_hid_capture.py devicehub-*.pcap --surface 512` prints every keyboard report;
`--surface 1026` the button ones. With those bytes in hand, `type_text` and the lock/volume buttons
are each an afternoon on the existing HID path.

## Structural gaps (not single features)

- **One device at a time.** cdhost binds a single phone; rebinding re-executes the daemon. Device Hub shows several devices at once. The multi-device registry is designed (adb-shaped) but not built.
- **Rebinding is a re-exec, and it showed.** Switching devices re-executes the daemon; the app
  reconnects after 4 s, and in that window any tab that asks the engine says "not connected".
  Worse, a re-executed daemon once ignored Ctrl-C and a root `kill -INT` (inherited signal
  state is the only mechanism that fits); `own_signals()` in `cdhost.c` and the `quit` API
  method are the answer, to be confirmed the next time a rebound daemon is stopped.
- **Dead-session liveness (bug #7).** When the phone leaves wifi the daemon keeps answering `tunnel_info`/`stream_info` while nothing flows; only a restart clears it. Device Hub does not have this failure. Any parity claim should include fixing it — it is exactly the kind of thing a drop-in replacement gets judged on.
- **iOS < 27 cannot mirror.** Same limit as Device Hub ("screen viewing is unsupported"), so not a parity gap — but the app should say so as plainly as Device Hub does when such a device is selected. It currently warns in the daemon log only.
- **Portability caveat.** Decoding the RVRA-adapted stream works on Apple hardware only today (`doc/RVRA-AND-PORTABILITY.md`). Irrelevant for replacing Device Hub on macOS, decisive if the same product is asked to serve Linux/Windows viewers.

## What a user must do before any of this works (2026-08-23)

Device Hub and rPlayHub share the same on-device prerequisites; we have replaced none of them:

1. **Pair and trust** the phone with the Mac once over USB (the pairing record in
   `/var/db/lockdown`). Our Pair/Unpair is not built, so an Apple tool creates the record.
2. **Developer Mode on** (Settings → Privacy & Security). Without it the phone mounts no DDI and
   the RSD map has no `coredevice.*` services. Needs a reboot and an on-device confirmation.
3. **A personalized Developer Disk Image mounted, after every phone reboot.** Verified on the
   iPhone 12 / iOS 26.5: `mobile_image_mounter LookupImage Personalized` reports one mounted, at
   `/dev/disk4s1`, put there by Xcode/Device Hub. The base image is Apple's, on the Mac at
   `/Library/Developer/DeveloperDiskImages/iOS_DDI/Restore/` (`*.dmg`, trust cache, root hash,
   `BuildManifest.plist`; iOS 27 build `27A5218g` came with the Xcode 27 beta). The
   personalization is an Apple-signed manifest bound to the phone's ECID and a per-boot nonce,
   obtained with a TSS request to `gs.apple.com`. Today only Apple's tools do this on this Mac;
   pymobiledevice3 does it from Linux and Windows, so it is portable and documented.
   **Unknown, and worth a five-minute experiment:** whether the services we use
   (`displayservice`, `screencaptureservice`, `universalhidservice`, `appservice`) need the DDI
   at all. Restart the phone with our Restart button, run cdhost before any Apple tool touches
   the phone, and see whether those ports are advertised and a screenshot comes back.
4. Xcode is NOT needed at runtime. The source checkout is: the Info tab runs
   `host/deviceinfo.py`, and the simulator section needs `xcrun simctl`.

**The mount module exists as a prototype: `host/ddi_mount.py`** (2026-08-23). Proven live on the
iPhone 12: identity and nonce over the mounter, and a TSS request that Apple's server answered
with a 3,090-byte `ApImg4Ticket` -- built by our code, no Xcode, no Device Hub. Getting the
request shape right took diffing against one built by libtatsu (`SepNonce`,
`PearlCertificationRootPub`, the `@VersionInfo` string, `EPRO`/`ESEC` on rule-less entries). The
phone also caches the manifest it last accepted, keyed by the current nonce, and
`QueryPersonalizationManifest` returns it without any network -- the script tries that first.
`ReceiveBytes` + `MountImage` are sent exactly as ideviceimagemounter's debug log shows, but the
mount itself has not been exercised: it needs a rebooted phone with nothing mounted (`mount`
refuses when an image is present). Porting to C is straightforward -- plists and a TLS POST --
and is what makes a TestFlight user self-sufficient after a reboot (`app/DISTRIBUTION.md`).

## What parity does NOT require

- RTCP PLI/FIR keyframe requests are already working here and ignored by Apple's own client — ours is strictly more capable since the SSRC fix (`doc/DEVICEHUB-CAPTURE-FINDINGS.md` corrections).
- LTR-ACK: negotiated but never exercised by either side; keep sending because Apple does, expect nothing from it.
- The bitrate experiment (still unrun — see `doc/BITRATE-EXPERIMENT-HANDOFF.md`) governs portability and picture quality, not feature parity.
