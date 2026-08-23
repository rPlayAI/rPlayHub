# rPlayHub — testing on another Mac

This DMG carries both halves of rPlayHub: **rPlayHub.app** (the window you click) and
**engine/cdhost** (the privileged engine it talks to). They talk over localhost, exactly as on the
build machine.

TestFlight is not an option yet — the engine must run as root to create the tunnel interface, and
the App Store sandbox forbids that. This is the Developer ID build; it runs on any Mac once
notarized.

## One-time setup on the iPhone

1. **Pair and trust** the phone with this Mac: connect by USB once, unlock, tap **Trust**.
   (Finder, Xcode, or Apple's Device Hub all create the pairing record rPlayHub needs.)
2. **Developer Mode on**: Settings → Privacy & Security → Developer Mode → on, then reboot.
3. iOS **27 or later** to mirror the screen. iOS 26 and earlier connect and report info but cannot
   mirror — the same limit Apple's Device Hub has.

## Each session

1. Drag **rPlayHub.app** to /Applications (or run it in place).
2. In Terminal, start the engine as root:

       sudo /Volumes/rPlayHub*/engine/cdhost        # or wherever you copied the engine folder

   Keep this Terminal open — it holds the tunnel. Ctrl-C stops it.
3. Launch **rPlayHub**, pick the phone in the sidebar.

## After the iPhone reboots (iOS 17+)

iOS discards the developer disk image on every reboot, and without it the screen/control services
go quiet. Two ways to restore it:

- **If you have Xcode or Apple's Device Hub**, connect the phone with either once; it remounts the
  image. Then use rPlayHub.
- **Self-contained** (no Xcode): from a checkout of the rPlayHub source, with the engine running,
  `./scripts/activate-after-reboot.sh` mounts it for you (needs Python 3 and the iOS DDI files).
  Bundling that into the app is on the roadmap (see app/DISTRIBUTION.md).

## If the screen stays black

- The phone must be **unlocked** for the developer image to mount.
- If the phone left wifi mid-session, the engine can keep a dead session — restart `cdhost`.
- Check the Terminal running `cdhost`: it prints each layer as it connects and says plainly when a
  device is too old to mirror.
