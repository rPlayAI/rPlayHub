# app/ — rPlayHub, the mirror + control app

A native macOS app: a live **View Screen** window showing the phone, where clicks become taps and
drags become swipes. Swift and AppKit, an Xcode project, shipped as a notarized DMG.

Builds clean today (Debug and Release, no warnings) and produces a working DMG. It has **not yet been
run against a phone** — that needs the engine up, which needs one `sudo`.

## Build and run

```
xcodebuild -project app/rPlayHub.xcodeproj -scheme rPlayHub -configuration Debug build
open app/rPlayHub.xcodeproj          # or just work in Xcode

sudo python3 host/mirror.py <udid>   # the engine first — it needs root for the tunnel
./scripts/package-dmg.sh             # build/rPlayHub-0.1.0.dmg
```

The app retries the engine connection every two seconds and shows what it is waiting for in the
window title, so the order you start them in does not matter.

## Window layout — cloned from Device Hub

Apple's Device Hub is three panes: a device sidebar (search, an "Available" section, rows showing
name, model and the iOS version right-aligned), the device in the middle, and an inspector on the
right. Ours keeps that shape and puts the live screen where the device mockup goes.

```
┌────────────────┬───────────────────────┬──────────────┐
│ Search         │                       │ Controls     │
│ Available      │    live screen        │  Pin         │
│ ● iPhone13 27.0│    click = tap        │  Home        │
│ ○ Christina 26.5│   drag  = swipe      │  Screenshot  │
│                │                       │  Record      │
│                │                       │ Stream       │
│                │                       │  1170x2532   │
└────────────────┴───────────────────────┴──────────────┘
```

**While the screen is embedded the controls pane is hidden**, because a controls pane beside the
device list is redundant — right-click the screen instead, which is how Device Hub does it. View >
Show Controls brings the pane back, and the device rows have the same commands on right-click.
A separate mirroring window that carries the sidebar itself is not built yet.

Icons are SF Symbols, not extracted artwork. Device Hub uses them too, and copying Apple's image
assets into a shipping product is a legal problem we do not need.

## Files

```
app/
  rPlayHub.xcodeproj/           the Xcode project (target: rPlayHub, app bundle)
  rPlayHub/
    main.swift                  NSApplication bootstrap
    DeviceSidebar.swift         the device list: search, rows, status dots, context menu
    ControlPanel.swift          the right pane: Pin/Home/Screenshot/Record + stream health
    AppDelegate.swift           window, engine connection with retry, status line
    MirrorView.swift            the View Screen: display layer + clicks → taps/swipes
    HEVCStream.swift            Annex-B → access units → CMSampleBuffer → display layer
    StreamClient.swift          reads the live video stream (blocking thread)
    ControlClient.swift         JSON-line control client
    TCPSocket.swift             minimal blocking TCP over Darwin sockets
    Info.plist                  bundle identity
    rPlayHub.entitlements       not sandboxed, network client — see DISTRIBUTION.md
  api/PROTOCOL.md               the control API contract
  DISTRIBUTION.md               signing, notarization, DMG, and why not the App Store
  PORTING.md                    the four OS seams for Linux and Windows
```

## How it works

The app is a **client**, not a reimplementation. The engine owns the privileged part — creating the
tunnel interface needs root — and publishes video on `127.0.0.1:9877` and control on
`127.0.0.1:9876`. That split is what lets the app ship unprivileged, signed and notarized normally.

```
engine ──Annex-B HEVC :9877──▶ StreamClient → AnnexBParser → HEVCStream → AVSampleBufferDisplayLayer
       ◀──JSON lines :9876──── ControlClient ← MirrorView (mouse)
```

### Decoding: VideoToolbox, hardware, no ffmpeg

Decode is **VideoToolbox**, the same as `~/rplay`. The app links only AppKit, AVFoundation and
CoreMedia — there is no ffmpeg or libav anywhere in the bundle.

What differs from rplay is only *who owns the session*. rplay's `hwdecoder.m` creates a
`VTDecompressionSession` explicitly, gets `CVImageBuffer`s back, and enqueues them into the display
layer. We hand length-prefixed HEVC samples plus a format description straight to
`AVSampleBufferDisplayLayer`, which drives VideoToolbox itself — same hardware decoder, fewer copies,
much less code.

Take the session back explicitly (port `refs/rplay/src/receiver/hwdecoder.m`) the moment we need the
pixels rather than just the picture: recording, frame capture from the video stream, image analysis,
or a Metal renderer. For display alone, the layer is the better path.

**ffmpeg stays in the picture for Linux.** It is the intended decode backend there (see
`PORTING.md` seam 3), and it is already the verification tool on every platform: `ffprobe` and
`ffplay` against a recording are how the stream gets checked independently of the app.

### Three other decisions worth knowing

- **Access units, not loose NALs.** VCL NALs are grouped into one sample buffer per picture, detected
  from `first_slice_segment_in_pic_flag` — the top bit of the byte after the 2-byte NAL header. Real
  recordings do contain multi-NAL access units, so this matters.
- **Drop, never flush.** When the display layer is behind, the frame is dropped. Flushing to catch up
  caused a lag bug in the code this was adopted from; dropping a frame is invisible, a flush is not.
- **The coded frame is not the screen.** `displayservice` encodes at 16-pixel alignment with no
  conformance-window crop, so a 1170×2532 screen arrives as 1184×2576 with black padding on the right
  and bottom. `MirrorView` crops to the device size reported by `list_devices`; mapping clicks against
  the full frame would drift every tap toward the top-left by up to 1.7%.

Clicks map against the letterboxed **screen rectangle**, never the view bounds. The view is
deliberately `isFlipped` — AppKit flips the backing layer's geometry for a non-flipped view, which
flips manually-added sublayers' content too and rendered the video upside down; flipping the view
fixes that and gives a top-left origin matching the device's, so no y inversion is needed. A drag
shorter than 2% of the screen is treated as a tap.

## Relationship to ~/rplay

The design is adopted from `~/rplay`'s app — the JSON-line API contract, the drop-don't-flush rule,
the window-per-device model — but this is our own code on our own CoreDevice stack, not a port of
theirs. Theirs decodes H.264 from AirPlay and injects touch over iAP HID; ours decodes HEVC from
`displayservice` and injects over `universalhidservice`.

Their sources are kept unmodified in `refs/rplay/src/` as reference, with provenance and a note on
what is *not* reusable in `refs/rplay/MANIFEST.md`. Worth reading before extending this:
`refs/rplay/src/ui/c_NSWindow.swift` (event forwarding decisions) and
`refs/rplay/src/receiver/hwdecoder.m` (the VideoToolbox path).

## Not built yet

- **Keyboard input.** `refs/rplay/src/ui/iPhoneKeyboardRemap.swift` is the reference; the engine
  needs a `type_text` / key-event method first.
- **Device list UI.** The engine is single-session today, so the app shows one window. The
  `adb`-shaped multi-device registry is designed but not built.
- **Screenshot to file** from the app — the engine already exposes `take_screenshot`.
- **A privileged helper** so the user does not run the engine by hand. See `DISTRIBUTION.md`.
