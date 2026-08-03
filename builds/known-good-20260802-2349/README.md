# Known-good build — 20260802-2349 (commit 9efbb10)

Supersedes builds/known-good-20260802-1446, which predates device selection and
leaked a receiver on every retry. Delete the older one rather than keeping both.

## Verified working

- Live mirroring, RVRA enabled, no garbling. 1170x2532, 0.0% loss, ~900 frames
  decoded over a swipe-heavy run on an iPhone 13 Pro (iPhone14,2 / iOS 27) over wifi.
- **Device selection from the sidebar.** cdhost takes no udid; clicking a device
  rebinds it by re-executing the daemon, and the app reconnects.
- Diagnostics tab, simulator listing, Home (a real gesture), Rotate (view only),
  tap, swipe, screenshot, recording.

## Known gaps

- Decoding is VideoToolbox-only and cannot be ported as-is: see
  doc/RVRA-AND-PORTABILITY.md. Turning RVRA off from the offer was tested and does
  not work.
- iOS 26 devices bind but cannot mirror; the daemon warns.
- Simulators list but do not mirror.
- Pair / Unpair / Restart not built. Restart has no blocker.
- Lock, volume, Siri refused: mainScreenButtons report format not decoded.
- Keyboard (_ServiceID 512) advertised and unused.

## Restoring

    cp -R builds/known-good-20260802-2349/rPlayHub.app build/dd/Build/Products/Debug/
    cp builds/known-good-20260802-2349/cdhost host-c/
