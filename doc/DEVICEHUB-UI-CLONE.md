# Device Hub UI clone — exact structure to match

From close side-by-side examination of Apple's Device Hub (Xcode 26) against rPlayHub on
2026-08-28. This is the target for making both front-ends (macOS `app/rPlayHub/`, Swift/AppKit;
Linux `client-c/rplay-gui.cpp`, Dear ImGui) pixel-faithful clones. What's done and what remains
are marked.

## Window: three areas (both apps already have this shape)

`[ sidebar ] [ canvas ] [ inspector ]`

## Toolbar (top of window) — Device Hub

The ENTIRE toolbar, including the 3 Settings/Report/Info icons, lives in the window's title bar
row itself — same height as the traffic lights, one continuous `NSToolbar`, not a separate row
below the title. Confirmed left→right from a full-width title-bar screenshot (`screencapture`
+ `osascript`/System Events driving the live app):

`[traffic lights]` `+` (add device) `≡` (list icon) `▢▏` (sidebar-toggle) — then the device
title (`iPhone13` / `iOS 27.0`, two lines, left-aligned right after the sidebar toggle, sitting
where the canvas begins — **not centered across the window** like a standard macOS title) —
then a keyboard icon + a grid/frame icon (paired, greyed when N/A) — then a gap — then `»`
(overflow expander) — then a gap — then, at the very trailing edge: the 3 icon tabs
(Settings/Report/Info).

**rPlayHub has**: sidebar-toggle + the 3 icon tabs, both correctly living in the title bar
(`AppDelegate.buildToolbar`/`NSToolbarDelegate`, `InspectorPane.iconTabs` hosted in an
`NSToolbarItem`). **Still missing**: `+`, the list icon, the keyboard/grid icon pair, the `»`
expander, and positioning the device title left-aligned at the canvas start instead of centered.

### Icon tabs also toggle the inspector shut — not a separate button

Confirmed against the real Device Hub (clicked its Info icon twice): re-clicking whichever of
Settings/Report/Info is already active **collapses the entire inspector pane** (canvas expands
to fill the space, no icon shows highlighted while collapsed); clicking any icon while collapsed
reopens it with that tab active. There is no dedicated separate show/hide button for the right
panel — the same 3 icons double as that toggle. ✅ Done on macOS (`InspectorPane.iconTabChanged`,
`.selectAny` tracking + hand-rolled exclusivity, `setHidden()` keeping the existing Device-menu
"Show/Hide Controls" command in sync). **Not yet done on Linux.**

## Sidebar — Device Hub

- **Search** field at top.
- **Available** section header, then device rows: name (bold) + model subtitle, OS version
  right-aligned. Selected row highlighted.
- **Unavailable** section header at the bottom for offline/unusable devices.
- rPlayHub has Search + Available; **the Unavailable section header is the gap.**

## Canvas — Device Hub  ← the "View Screen" difference

Device Hub shows a **static device mockup** with the device name + OS below and a **"View Screen"**
button; the live screen only appears **on click**. To match: canvas opens with a device image (a
`take_screenshot` still is the natural source) + a "View Screen" button; live mirror starts on
click.

✅ **macOS, done and confirmed live (2026-08-28), true click-to-connect**: `AppDelegate` no longer
starts USB capture or the RTP/proxy video path inside `connect()` unconditionally — that only runs
once `wantsVideo` is true, set by clicking `MirrorView`'s new View Screen button (`onViewScreen`)
or already true from a prior click when a background reconnect re-runs `connect()`. Until clicked,
the canvas shows the device name + OS and the button, over whatever still picture the existing
idle-screenshot mechanism (`refreshStillIfIdle`, unchanged) already fetches; a plain blue rectangle
(`clipLayer.backgroundColor`, matching Device Hub's own "no screenshot yet" placeholder, sampled
off the live app) shows before even that. Selecting a different device resets `wantsVideo` to
false. **Confirmed end-to-end against the live iPhone**: static prompt shows, click hides it and
starts genuine live video (watched real device content start playing).
**Not yet done on Linux** (`rplay-view`/`rplay-gui` stream immediately, same as macOS did before).

## Inspector — Device Hub  ← the tab-structure difference (the main ask)

**Two levels:**

1. **Top row: 3 ICON tabs** — **Settings**, **Report**, **Info (ⓘ)**.
   - **Settings** (sliders icon): device appearance/accessibility controls — Appearance (Light/Dark),
     Liquid Glass (slider), Color Filter, Text Size (slider), Reduce Motion, Increase Contrast,
     Show Borders, Reduce Transparency, VoiceOver, Location. (These are settable on the device.)
   - **Report** (document icon): a diagnostics report view.
   - **Info** (ⓘ): device info + the named sub-tabs below.
2. **Second row (under Info): TEXT-named tabs** — **Info / Apps / Profiles**.
   - **Apps**: a **Filter** search field + a **category dropdown** (`All Apps` ─ separator ─
     `App Clips` / `Default` / `Developer`), the app list, and **`+` / `-` buttons** (install /
     uninstall). ✅ All done and confirmed live: Filter + dropdown, `+`/`-` (engine API in
     `api_server.c`), and accurate category matching (`isDeveloper` = `get-task-allow` out of
     Entitlements, not just `isFirstParty` — confirmed against the live Device Hub, whose
     Developer bucket held only 3 sideloaded test builds, not every third-party app; `isAppClip`
     = `IsAppClip` verbatim, confirmed both apps agree the test device has none).
   - **Profiles**: list + **`+` / `-`** (install / remove profile). ✅ Engine API done
     (`install_profile`/`remove_profile` in `api_server.c`), wired on both front-ends.
   - **Info**: device identifiers/health.

**rPlayHub today**: a SINGLE row of **6 icon tabs** (Controls / Info / Apps / Profiles / Files /
Console). To clone Device Hub: split into the 3-icon top row + text sub-tabs. Reconcile our extras
(Console, Files, Controls) — likely fold them under Info's sub-tab row (Device Hub proper has only
Info/Apps/Profiles), or keep them as additional sub-tabs.

## Done so far (2026-08-28)

- macOS menu bar: added **Edit** (Cut/Copy/Paste/Select All were missing) and **Device**
  (Screenshot/Home/Restart/Shut Down/Sleep); About opens the standard panel. Fixed the app-link
  regression from the Linux `media.c` rewrite (usernet.h kernel fallbacks for `tun_*`).
- macOS **Apps tab**: Filter field + category dropdown.
- Linux `rplay-gui`: console filter → bottom bar; Apps Filter + category dropdown.
- **Engine API for `+`/`-`** (`host-c/api_server.c`, documented in `app/api/PROTOCOL.md`):
  `install_app` stages a local `.ipa` into `/PublicStaging` over AFC (new write-side AFC:
  `AFC_OP_MAKE_DIR`/`AFC_OP_FILE_WRITE`) and installs it via `installation_proxy`'s classic
  `Install`; `uninstall_app` is `installation_proxy`'s `Uninstall`; `install_profile` forwards a
  local `.mobileprovision`/`.mobileconfig` to misagent/MCInstall by extension; `remove_profile`
  takes `{type, uuid}` or `{type, identifier}`. Engine builds clean (`cd host-c && make`).
  Wired on **both** front-ends: macOS `AppsPanel`/`ProfilesPanel` (`+` opens an `NSOpenPanel`,
  `-` confirms via `NSAlert`) — built and screenshot-verified via `xcodebuild`; Linux
  `rplay-gui.cpp` `tab_apps`/`tab_profiles` (`+` is a typed local path field, matching every other
  local-path field in that app; `-` confirms via an ImGui popup modal) — **not yet build-verified**,
  the Mac hits the known `deps/ffmpeg/version` vs. C++ `<version>` header collision on this file
  too; needs a Linux-side `make rplay-gui` check.
  Not yet exercised against the live device (would actually install/remove something real) —
  do that deliberately, not as a side effect of a build check.
- **macOS inspector restructure**: `InspectorPane.swift` now matches Device Hub's real two-level
  shape, confirmed live against the actual app (`screencapture` + `osascript`/System Events
  driving it) — 3 icon tabs (Settings/Report/Info) living in the **window's title bar itself**
  (moved there after an initial pass wrongly put them top-right of the content area — see the
  Toolbar section above), and under Info a second row of TEXT tabs: Info/Apps/Profiles (Device
  Hub's real three) plus our folded-in Files/Console/Controls (Device Hub has no equivalents for
  those three — product decision, see below). Settings and Report are `ComingSoonPanel` stubs
  pending the engine methods item 2 below needs. Re-clicking the active icon tab also collapses
  the whole inspector (see Toolbar section — this is Device Hub's actual behavior, verified
  against the live app). Built and screenshot-verified via `xcodebuild`.
  **Linux still has the OLD flat single-row-of-6 text tabs** (`rplay-gui.cpp`'s `BeginTabBar`
  with Info/Apps/Console/Files/Profiles/Diagnostics) — this app has no icon font anywhere, so
  its "3 icon tabs" row would need to fake icons somehow or just be 3 more text tabs; needs a
  design call before porting this restructure over.

## Product decision (2026-08-28): folded-in extra tabs

Device Hub's Info sub-tab row is only Info/Apps/Profiles. rPlayHub's extra panels (Controls,
Files, Console on macOS; Files, Console, Diagnostics on Linux) have no Device Hub equivalent, so
per the user's call they're folded in as additional text sub-tabs alongside Info/Apps/Profiles
rather than kept as their own icon tabs or moved into Settings. On macOS this made the sub-tab
row wider than Device Hub's real one (6 segments vs. 3) — fixed by pinning it to the inspector's
full width and shrinking the segment font, rather than by cutting any panel.

## Row/list styling fixed (2026-08-28)

Both real NSTableView quirks that paint across a list's ENTIRE visible bounds regardless of
actual row count (not just real Device Hub differences) — `usesAlternatingRowBackgroundColors`
and `gridStyleMask` — were the cause of "row shadows" appearing under an empty category filter.
Fixed on macOS (Apps/Profiles/Files): flat rows, a per-cell `NSBox` bottom divider instead of
`gridStyleMask`, and an explicit background color (`#E4E4E4`, sampled directly off the live
Device Hub window) instead of white. Confirmed clean against the live app in both states.
**Not yet checked on Linux** — `ImGuiTableFlags_BordersInnerH` may or may not have the same
"fills empty space" behavior; worth verifying once building on Linux is possible again.

## Canvas width, pre-connect mockup sizing, and pane backgrounds fixed (2026-08-28, later same day)

A second pass after the inspector restructure above, triggered by the user comparing the live
apps side-by-side and finding the canvas pane badly squeezed (~100pt instead of Device Hub's own
389pt, measured directly off its live window) and the pre-connect mockup rendering huge and black
instead of small and blue. Root causes, all on macOS (`MirrorView.swift`, `InspectorPane.swift`,
`DiagnosticsPanel.swift`, `AppDelegate.swift`):

- **`stage` (the canvas pane) had no resting-width constraint of its own** — only sidebar and
  inspector did (`AppDelegate.buildWindow`'s pane-width loop). With the lowest `NSSplitView`
  holding priority AND nothing to defend, any legitimate over-demand elsewhere shrank it
  arbitrarily far. Fixed by adding `stage` to that loop with an explicit **389pt** resting width
  (Device Hub's own canvas, measured live) and bumping inspector's from 260 to 320 (needed for our
  six sub-tabs vs. Device Hub's three).
- **Two wrapping labels with no width constraint** (`DiagnosticsPanel`'s status message and its
  "Unavailable" service list) reported their full unwrapped single-line width as intrinsic content
  size, dragging the inspector — and therefore the canvas next to it — wider still. A plain width
  constraint does NOT fix this for a wrapping `NSTextField`; `preferredMaxLayoutWidth` is what Auto
  Layout actually consults. Fixed both with `preferredMaxLayoutWidth = 224`.
- **`textTabs` (the six sub-tabs) used `setWidth(_:forSegment:)` on a `.texturedRounded` segmented
  control**, which turned out not to reliably control intrinsic content size under Auto Layout —
  measured via Accessibility inspection (`entire contents of window` + per-element `position`/
  `size`) that it stayed at its old (wide) size regardless. Switched to `segmentDistribution =
  .fillEqually` plus an explicit `widthAnchor` tied to the pane's own width, which actually works.
- **Pre-connect mockup was the wrong size**: `bounds.width * 0.12` was Device Hub's WINDOW-relative
  ratio (94pt / 923pt window) mistakenly applied to this view's own canvas-only bounds — silently
  halving it. Fixed to `bounds.width * 0.24` (94pt / 390pt canvas, the actual live ratio).
- **Pre-connect mockup rendered full-canvas-sized and black instead of small and blue**, two
  compounding bugs: (1) `presentedSize` is genuinely `(0,0)` for the entire time before
  `deviceSize` arrives asynchronously, and the top-level guard in `screenRect()` fell back to full
  `bounds` — which happens to be roughly phone-proportioned, so it read as "correct but huge"
  rather than obviously wrong; fixed with a generic 9:19.5 fallback aspect while gated. (2)
  `displayLayer` (opaque black by default, sized to fill `clipLayer`) sits above `clipLayer`'s blue
  placeholder fill unconditionally — fixed with `displayLayer.isHidden = isGated`.
- **`viewScreenStack` (name/OS/button) positioning lived after two video-geometry `guard` returns**
  that fire whenever `videoSize` is `(0,0)` — true for the ENTIRE pre-View-Screen lifetime — so it
  never ran while gated and the stack stayed at a stale/default frame. Moved earlier in `layout()`,
  right after the cutout geometry, before those guards.
- **Inspector background didn't match the sidebar's** (`#E4E4E4`, sampled off the live app) — it
  had no explicit background at all and fell through to white. Fixed by giving `InspectorPane`
  itself that background; individual sub-panels' own list/table backgrounds already matched, so
  this just fixed the panels (Info, stubs) that don't have their own scroll/table view.
- **View Screen's icon** now matches Device Hub's own: `rectangle.stack.badge.person.crop`,
  identified by cropping Device Hub's live button pixel-for-pixel and comparing against rendered
  candidates pulled from the system's SF Symbols name list (`CoreGlyphs.bundle/
  name_availability.plist` inside an iOS Simulator runtime — 9476 public symbol names; confirmed
  via `assetutil` against `DeviceHub.app`'s `Assets.car` that it has no bundled icon assets besides
  the app icon itself, so every icon in it, ours included, is a stock symbol to identify, not an
  asset to extract).
- **The mockup's bezel border was tried gated to `isGated` only** (reasoning: at live-video size it
  became an 11-12pt black ring painted over the picture, since `clipLayer.borderWidth` scaled with
  `clipSize` and a `CALayer` border always draws above its sublayers). **User compared side-by-side
  against Device Hub's own live view and asked for it back in both states** — a real device also
  has a visible bezel edge around its screen, and no-border read further from Device Hub than
  keeping it did. **Reverted; current code applies the border unconditionally.** Worth knowing if
  this comes up again: the mechanism (border-over-video) is understood even though the fix was
  rejected — don't re-derive it, just don't re-apply it without checking with the user first.

All confirmed via repeated live `screencapture` + `osascript`/System Events comparison against the
real Device Hub window, and via direct Accessibility-tree inspection (`entire contents of window`,
`position`/`size` of every element) to settle exact pane widths when screenshots alone couldn't
distinguish "the layout is actually wrong" from "AX reports a wrapping label's unwrapped bounds,
not its real frame" — the latter cost significant back-and-forth before being ruled out; trust the
screenshot over AX `size` for any element that might be word-wrapping.

**Not yet done on Linux** — none of this pass touched `rplay-gui.cpp`; the equivalent ImGui layout
(if it has an analogous "did one pane starve another" issue) hasn't been checked.

## Open issues, found but not yet resolved (2026-08-28)

- **"Open in New Window" (`ScreenWindow.swift`) frozen frames — fix implemented, needs live
  verification (third 2026-08-28 session)**: the likely mechanism is that moving `MirrorView` to
  a different window rebinds its layers to that window's presentation context, and
  `AVSampleBufferDisplayLayer` keeps ACCEPTING frames across the move — enqueue succeeds,
  `framesPresented` keeps counting, which is also why `refreshStillIfIdle` saw a "healthy" stream
  and never rescued with a screenshot — while its internal renderer stays bound to the old
  context and silently stops painting. `MirrorView.viewDidMoveToWindow` now calls
  `displayLayer.flushAndRemoveImage()` (covers detach AND reattach) and logs the move. Could not
  be reproduced/verified live — cdhost was down for the whole session — so this is a
  best-supported-hypothesis fix, not a confirmed one: with the daemon up, click View Screen,
  right-click → Open in New Window, and watch for both the log line and frames continuing.
- ✅ **`scheduleRetry` backoff — done and verified (third 2026-08-28 session)**: one-shot timer
  doubling 2s→60s per failed attempt, reset to 2s on any successful connect (USB, direct,
  proxied) and on deliberate `reconnect()`; each scheduled retry now logs its reason + delay.
  Verified against a dead daemon: log shows retries at exactly 2/4/8/16/32/60s, capped at 60.
  Note for future log-chasing: a DerivedData-launched app can't find the repo root (no
  `scripts/live.sh` above it), so `AppBuild.log` writes to `$(getconf DARWIN_USER_TEMP_DIR)/app.log`
  instead of `logs/app.log` — an empty repo log does not mean no logging.
- ✅ **Black-void canvas when the engine is unreachable — fixed (third 2026-08-28 session)**:
  nothing called `updateViewScreenPrompt()` until `connect()` succeeded, so with cdhost down the
  canvas stayed ungated and `displayLayer`'s opaque black filled the pane. The app now starts in
  the gated state (blue mockup + "no device" + View Screen), screenshot-verified.
- **Settings and Report tabs need a live capture, and none exists yet.** Checked every `.pcap` in
  the repo (`logs/`, `reference/captures/`, `build/`) — all of them (the two "devicehub" captures
  are byte-identical) cover only the View Screen media-stream session
  (`mediastreamstart`/`status`/`getsupportinfo`, power assertions, `com.apple.mobile.lockdown`),
  never Settings or Report being opened. This matters because of WHY Info/Apps/Profiles/Files never
  needed a Device-Hub-specific capture in the first place: they're built on **libimobiledevice**,
  which already implements the classic "lockdown" protocol (`installation_proxy`, `misagent`,
  `AFC`, `diagnostics_relay`, `springboardservices`) as public, well-documented services used by
  countless third-party tools — no reverse-engineering of Device Hub's own traffic was ever
  needed for those. Settings and Report are almost certainly backed by modern, private
  **CoreDevice** XPC services instead (`doc/LIBIMOBILEDEVICE-MIGRATION.md` is explicit that
  CoreDevice/RemoteXPC is NOT libimobiledevice's territory), which have no such existing reference
  — a targeted capture of Device Hub's own traffic while those specific tabs are open is the only
  real path in. `host/capture-devicehub.sh` (needs `sudo tcpdump`) is the tool: connect the device
  in Device Hub WITHOUT clicking View Screen, run the script against the CoreDevice tunnel utun it
  finds automatically, then click into Settings and/or Report (instead of, or in addition to, View
  Screen) while it captures. The user needs to run this (sudo); screenshots of both tabs' actual
  content would also help even without a capture, to know what fields/controls to build toward.

## Settings tab: protocol SOLVED (2026-08-28, third session)

No longer blocked. The user ran `host/capture-settings.sh` while clicking through the tab, and
the capture (`settings-20260828-223037.pcap`) decodes cleanly with `host/decode_settings.py`.

**It is not usbmux, and there was never anything to capture there.** Checked live with a
connected device: `DeviceHub.app` holds **zero** connections to `/var/run/usbmuxd`. Every device
connection it has is TCP over IPv6 to the CoreDevice tunnel's ULA address on the mtu-16000 utun.
usbmux is used only by `remotepairingd` to bring that tunnel up. So a plain `tcpdump` on the
tunnel sees all of it in cleartext, and none of the invasive options (moving the usbmuxd socket
aside for a proxy, or dtrace against an Apple-signed binary, which SIP blocks) are needed.

**Wire format** — the same CoreDevice envelope screenshots and media already use
(`host/coredevice.py`, `core/rp_coredevice.c`), but carrying **only an `actionIdentifier`, no
`featureIdentifier`**. `rp_cd_invoke` already accepts exactly this (NULL feature + action), so
the engine needs no new transport code:

```
request   CoreDevice.actionIdentifier = com.apple.coredevice.action.setshowborders
          CoreDevice.input.showBorders.enabled = true
response  CoreDevice.output.showBorders.enabled = true
```

**The payload key is not derivable from the action name** and must be taken as observed:
`setshowborders`→`showBorders`, `setdeviceincreasecontrast`→`increaseContrast`,
`setvoiceover`→`voiceOverConfiguration`, `setliquidglassconfiguration`→`configuration.opacity`
(a double, 0..1 — captured at 0.64, so this is the Liquid Glass slider).

**Confirmed on the wire** (both directions): `get/setshowborders`,
`get/setdeviceincreasecontrast`, `get/setvoiceover`, `get/setliquidglassconfiguration`,
`getreducemotion`, `getreducetransparency`, `getcolorfilter`, `getdevicetextsize`,
`availablelocationscenarios`, `setsimulatedlocation` (`input.latitude`/`input.longitude`),
`clearsimulatedlocation`.

**Also present in Apple's catalog but not exercised during the capture** — pulled from
`/Library/Developer/PrivateFrameworks/CoreDeviceUtilities.framework`, which carries every
`com.apple.coredevice.action.*` string, so these are confirmed to exist rather than guessed:
`setreducemotion`, `setreducetransparency`, `setcolorfilter`, `setdevicetextsize`,
`get/setuserinterfacestyle` (**this is the Appearance Light/Dark row**), `get/setdevicelookandfeel`,
`get/setlargeraccessibilitysizesenabled`, `getsupportedlooksandfeels`,
`getcustomizableappearanceelements`, `setlocationscenario`. That framework is the reference for
any action we still need — grep it before capturing again.

**The one open unknown: which RSD service hosts them.** The capture shows accessibility actions on
one device port and location actions on another (`com.apple.coredevice.locationservice`, matched by
port offset against `reference/rsd-services-ios27.json`), but RSD ports are per-session so the port
alone does not name the service. `host/settings.py probe` settles it by trying each candidate
(`configuration`, `devicecontrol`, `deviceinfo`, `appservice`, `diagnosticsservice`) with the
read-only `getreducemotion` — needs a live tunnel, so run it with the daemon up.

`host/settings.py` has the full action/payload table and get/set/probe; it is the reference
implementation to port into `host-c/api_server.c` (as `get_settings`/`set_setting`) and then wire
to the `SettingsPanel` on both front-ends.

## Remaining, in rough priority

1. **Settings tab** — protocol solved (see above). Next: run `host/settings.py probe` with the
   daemon up to name the service, then engine API + `SettingsPanel` replacing `ComingSoonPanel`.
   The **Report** tab is still unexamined — no capture, no screenshot yet.
2. **"Open in New Window" frozen-frame bug** — fix implemented (see "Open issues"), needs a live
   check with the daemon up. `scheduleRetry` backoff ✅ done and verified.
3. **Verify the Linux build** — ✅ done (third 2026-08-28 session): the Linux box (Ubuntu 22.04,
   g++ 11.4, imgui v1.91.8, nlohmann/json v3.11.3) pulled ed50db4 and force-rebuilt `rplay-gui`,
   `cdhost`, and `rplay-view` — zero errors, zero warnings; the hand-reviewed `rplay-gui.cpp`
   changes were all valid as written. Build-verified only: the iPhone was unplugged on that box,
   so the nested tab bars / Apps `+`/`-` wiring still need a live run there. The SECOND 2026-08-28
   session's canvas/inspector-width fixes have not been ported to Linux at all yet.
4. **Sidebar Unavailable section**; remaining toolbar items: `+`, list icon, keyboard/grid pair,
   `»` expander, device title repositioned left-aligned at the canvas start (not centered) — see
   the Toolbar section above for the exact catalogued order.
5. Icon choices for Settings/Report tab icons (currently `slider.horizontal.3`/`doc.text` SF Symbol
   guesses — Info's `info.circle` and View Screen's `rectangle.stack.badge.person.crop` are now
   confirmed matches, see above) — extracting Device Hub's `Assets.car` won't help (confirmed empty
   of icon assets beyond the app icon); the method that worked for View Screen — crop the live
   icon, compare against rendered candidates from the system SF Symbols name list — applies here
   too, once Settings/Report are actually being built.
6. **Click-active-tab-to-collapse** on Linux — Device Hub's Settings/Report/Info icons also toggle
   the whole inspector shut on a re-click (done on macOS, see above); ImGui's `BeginTabBar` doesn't
   naturally support that, so this needs a custom Selectable-based tab row instead if wanted.
7. Row-shadow check on Linux — `ImGuiTableFlags_BordersInnerH` (Apps table) may or may not have
   the same "fills empty space regardless of row count" behavior `gridStyleMask` had on macOS;
   worth a look once Linux building is possible again.

## Kickoff for the next session

Paste this to start a fresh session focused on finishing the Device Hub clone:

> Continue rplay-hub on branch `rendering-resolution-switch` (GitHub: rPlayAI/rplay-hub, private;
> git is the single source of truth — a Linux agent works the same tree, so `git pull` first, and
> `commit`+`push` your changes; never rsync). Goal: make rPlayHub a pixel-faithful **Device Hub
> clone** on macOS (`app/rPlayHub/`, Swift/AppKit) and Linux (`client-c/rplay-gui.cpp`, Dear ImGui).
> **Read `doc/DEVICEHUB-UI-CLONE.md` first, in full** — it has the exact target structure (validated
> against the real Device Hub), a detailed account of two sessions' worth of fixes and the specific
> bugs behind them (worth reading even for finished items — the mechanisms recur), the currently
> open/unresolved issues, and the priority order. Device Hub (Xcode 26) is on the user's shared
> desktop: `screencapture -x` to see it and drive it with `osascript`/System Events to examine each
> state precisely. Both apps build today: macOS via
> `xcodebuild -project app/rPlayHub.xcodeproj -scheme rPlayHub -configuration Debug build`, then run
> `~/Library/Developer/Xcode/DerivedData/rPlayHub-*/Build/Products/Debug/rPlayHub.app` and
> screenshot to verify. Linux `rplay-gui` builds on the Linux box (its Mac build hits a
> `-I../deps/ffmpeg` vs C++ `<version>` collision — Linux/CI is clean).
>
> Start with whichever of these three is actually unblocked when you pick this up (check with the
> user — the first is blocked on something only they can do):
> (1) **Settings and Report tabs** — blocked on the user running `host/capture-devicehub.sh` (needs
> their sudo) while clicking into those two tabs specifically, or at minimum screenshotting their
> content; nothing to build until then. (2) **"Open in New Window" frozen-frame bug**
> (`ScreenWindow.swift` — see "Open issues" in the doc for where to start) and **`scheduleRetry`'s
> unbounded 2-second reconnect loop** (`AppDelegate.swift` — needs backoff). (3) **Linux
> build verification** — two sessions' worth of macOS-only changes (the inspector restructure, the
> canvas/inspector-width fixes) have never been build-checked on `rplay-gui.cpp`; needs the Linux
> agent or a Linux-side `make rplay-gui`.
>
> Build + screenshot after each change; keep both platforms building; push frequently so the Linux
> agent stays in sync. The daemon needs sudo to (re)start — ask the user, don't attempt it yourself.
> It self-exits on tunnel death by design ("bug #7") and this happens often during active testing —
> expect to ask for restarts repeatedly and budget for it, don't treat each one as a surprise.
