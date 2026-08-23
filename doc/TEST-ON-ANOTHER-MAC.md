# rPlayHub — testing on another Mac

This DMG is a single app. The engine that reaches the device runs as a background system service
embedded in the app; you approve it once and never run any command by hand.

TestFlight is not an option yet — the engine needs system privileges to create the tunnel
interface, which the App Store sandbox forbids. This is the Developer ID build; it runs on any Mac
once notarized.

## Install

1. Drag **rPlayHub.app** to /Applications and open it.
2. On first launch it asks to enable its background engine. Click **Open Login Items** and turn on
   **rPlayHub**. (macOS runs the engine with system privileges after this one-time approval — the
   same way Apple's own tools run their device daemon.) That is the only setup step on the Mac.

## One-time setup on the iPhone

1. **Pair and trust**: connect by USB once, unlock, tap **Trust**. (Finder, Xcode, or Apple's
   Device Hub all create the pairing record.)
2. **Developer Mode on**: Settings → Privacy & Security → Developer Mode → on, then reboot.
3. iOS **27 or later** to mirror the screen. iOS 26 and earlier connect and report info but cannot
   mirror — the same limit Apple's Device Hub has.

## Use it

Open rPlayHub, pick the phone in the sidebar. The engine is already running in the background.

## After the iPhone reboots (iOS 17+)

iOS discards the developer disk image on every reboot; without it the screen/control services go
quiet. If you have Xcode or Apple's Device Hub, connect the phone with either once to remount it.
(Self-contained remount from within rPlayHub is on the roadmap — see app/DISTRIBUTION.md.)

## If the screen stays black

- The phone must be **unlocked** for the developer image to mount.
- The engine logs to /tmp/rplayhub-engine.log — it prints each connection layer and says plainly
  when a device is too old to mirror or when a session has gone stale (leave/rejoin wifi).
- To turn the engine off, toggle rPlayHub off under System Settings > General > Login Items.
