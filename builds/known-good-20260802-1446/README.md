# Known-good build — 20260802-1446

Snapshot of the two executables at commit `7b15add`, kept so a regression can be
compared against something that demonstrably worked rather than against memory.

    rPlayHub.app    the viewer
    cdhost          the engine (run with sudo; it creates the utun)

## Verified working at this commit

- Live mirroring, HEVC, RVRA enabled — no garbling on fast swipes. The decoder is
  put into resolution-adaptation mode (`VideoResolutionAdaptationType = 3`) and fed
  the per-frame size from the trailer on the last slice NAL. Without both, every
  encoder downshift decodes to a mosaic.
- 1170x2532 after cropping the encoder's alignment padding; measured 0.0% loss at
  40-50 fps on an iPhone 13 Pro (iPhone14,2) over wifi.
- Notch covered, screen corners rounded, no black edges at any resolution tier.
- Home — a real bottom-edge gesture through the touchscreen HID surface.
- Rotate — turns the view and the input mapping together; does not rotate the device.
- Diagnostics tab — lockdown over usbmuxd, independent of the tunnel.
- Tap, swipe, screenshot, recording.

## Known gaps at this commit

- Lock, volume and Siri are refused: they need the mainScreenButtons HID report
  format (_ServiceID 1026), which is not decoded.
- Keyboard input unimplemented (_ServiceID 512 is advertised and unused).
- Pair, Unpair, Restart not started — still waiting on the RSD service list from
  `sudo RPLAY_DUMP_SERVICES=1 ./host-c/cdhost`.
- Simulators not supported.
- cdhost never populates screen_w/screen_h; the app works around it with a
  per-model table. Implementing getdisplayinfo would retire that.

## Restoring

    cp -R builds/known-good-20260802-1446/rPlayHub.app build/dd/Build/Products/Debug/
    cp builds/known-good-20260802-1446/cdhost host-c/
