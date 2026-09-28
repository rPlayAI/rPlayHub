# RVRA and the resolution switch

How the resolution-switch mosaic was tracked down, and why the key involved is not discoverable
by interposing on VideoToolbox. Confirmed independently on a separate capture:
601/601 slice NALs on the wire carry the trailer, 0/1368 of what `avconferenced` feeds
VideoToolbox still has it, and the same wire bytes decode to a mosaic on a plain session but
cleanly on an RVRA session — all 601 frames.

`VideoResolutionAdaptationType = 3` is genuinely undiscoverable from a `Create`/`DecodeFrame`
interposer: the daemon sets it *after* session creation, and the recorded session says `VRAE:0`,
which points the opposite way. I had passed `ActiveVideoResolution` per frame myself, measured
0 of 401 frames changing, and wrongly concluded the key was inert — the correct reading was
"RVRA is off, so the key is discarded". That reading was wrong; RVRA being off is why the key looked inert.

The remaining instability is entirely in the view path. Three causes, in order of impact.

## 1. Implicit Core Animation on every tier switch

`MirrorView.swift` has no `CATransaction`. `displayLayer` is a raw sublayer, not view-backing, so
assigning `.frame` triggers the default ~0.25 s implicit animation. Tiers flip several times per
second, so the layer is permanently mid-animation and the picture continuously breathes. This
alone reads as "unstable resizing".

```swift
CATransaction.begin()
CATransaction.setDisableActions(true)
displayLayer.frame = CGRect(x: 0, y: 0, width: full.width, height: full.height)
CATransaction.commit()
```

## 2. Two publishers race

`AppDelegate.swift:562` and `:565` both write `view.activeSize`:

```swift
decoder.onActiveSize            = { self?.view.activeSize = CGSize(width: w, height: h) }  // parse time
view.displayLayer.onPresentSize = { self?.view.activeSize = size }                         // present time
```

The first runs on the receive thread, ahead of decode and display, so it keeps resizing the layer
to describe a frame that is not on screen yet. The `onPresentSize` path is the correct one —
delete the `decoder.onActiveSize` wiring and keep the attachment-on-the-pixel-buffer route as the
single source of truth.

## 3. The scale factor double-counts the padding

Current code applies both the padding fraction and the active ratio:

```swift
let full = CGSize(width: screen.width / fraction.width * ax, ...)
```

That puts the live region at `screen × videoSize/deviceSize` — **1.2 % too wide and 1.7 % too
tall** — but only at reduced tiers, since `ax == 1` at full size. So every switch jumps by ~1.7 %.

The two tiers are structurally different. Measured by scanning decoded frames for the grey
padding value:

| tier | declared | live region | padding |
|---|---|---|---|
| full | 1184×2576 | 1184×2576 | screen is 1170×2532 *inside* it |
| mid  | 1088×1920 | 1091×1920 | grey (value 139) beyond |
| low  |  720×1280 |  738×1280 | grey (value 139) beyond |

Cropping a 720-tier frame to exactly 720×1280 gives a complete home screen with no grey stripe —
the encoder squeezes the *whole* screen (anamorphically) into the active rect. There is no
alignment padding at reduced tiers. The 3 px / 18 px overshoot in the measured widths is scaler
edge bleed; trust the trailer value, not the scan.

So the live region is `deviceSize` at full tier and `activeSize` below it, never both:

```swift
let coded = videoSize
let live: CGSize = (activeSize.width > 0 && activeSize != coded)
    ? activeSize
    : CGSize(width: coded.width * fraction.width, height: coded.height * fraction.height)
let full = CGSize(width: screen.width * coded.width / live.width,
                  height: screen.height * coded.height / live.height)
```

## Verifying without a device

Assemble a pcap with `tools/pcapreplay`, then decode it twice through VideoToolbox — once on a
plain session, once with `NegotiationDetails=RVRA1:0;SW:1;FLS` plus
`VideoResolutionAdaptationType=3` — dumping PNGs and each output buffer's dimensions.
`tools/vtreplay` does the equivalent for a vtcapture recording.

Useful detail: with RVRA on, `CVPixelBufferGetWidth/Height` stays **1184×2576 for every frame**.
The buffer never changes size; only the live sub-rectangle inside it does. Anything in the view
reacting to buffer dimensions is a fourth bug.

## Caveat to carry

The tier list `{1184×2576, 1088×1920, 720×1280}` is this iPhone 13's. Another device needs its
own, and a trailer whose size is not in the list should be left alone rather than mis-stripped —
a false positive truncates real slice data.
