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
button; the live screen only appears **on click**. **rPlayHub streams live video immediately** with
a control strip beneath. To match: canvas opens with a device image (a `take_screenshot` still is
the natural source) + a "View Screen" button; live mirror starts on click. (Design decision:
whether to keep auto-live is a product call — Device Hub is click-to-view.)

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
     uninstall). ✅ Filter + dropdown done on both apps; **`+`/`-` still needs engine API**
     (install/uninstall app) before the buttons do anything. App Clips category needs the engine
     to surface app TYPE (installation_proxy has it; `list_apps` only returns `isFirstParty`).
   - **Profiles**: list + **`+` / `-`** (install / remove profile) — same engine-API gap.
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

## Remaining, in rough priority

1. **Linux inspector restructure** to match the macOS one above (needs the icon-tab design call),
   including the click-active-icon-to-collapse behavior.
2. **Settings panel**: the device appearance/accessibility controls (needs engine methods to read/set
   them — a new capability).
3. **Canvas View Screen button** + static screenshot preview.
4. **Sidebar Unavailable section**; remaining toolbar items: `+`, list icon, keyboard/grid pair,
   `»` expander, device title repositioned left-aligned at the canvas start (not centered) — see
   the Toolbar section above for the exact catalogued order.
5. App-type field in `list_apps` (for the App Clips category).
6. Icon choices for Settings/Report/Info (currently `slider.horizontal.3`/`doc.text`/`info.circle`
   SF Symbol guesses) — a designer will supply the real ones later; don't spend effort extracting
   Device Hub's compiled `Assets.car` for this.

## Kickoff for the next session

Paste this to start a fresh session focused on finishing the Device Hub clone:

> Continue rplay-hub on branch `rendering-resolution-switch` (GitHub: rPlayAI/rplay-hub, private;
> git is the single source of truth — a Linux agent works the same tree, so `git pull` first, and
> `commit`+`push` your changes; never rsync). Goal: make rPlayHub a pixel-faithful **Device Hub
> clone** on macOS (`app/rPlayHub/`, Swift/AppKit) and Linux (`client-c/rplay-gui.cpp`, Dear ImGui).
> **Read `doc/DEVICEHUB-UI-CLONE.md` first** — it has the exact target structure (validated against
> the real Device Hub) and the priority order. Device Hub (Xcode 26) is on the user's shared
> desktop: you can `screencapture -x` to see it and drive it with `osascript`/System Events to
> examine each state (click its Settings/Report/ⓘ icons, the Info→Info/Apps/Profiles sub-tabs) —
> match those precisely. Both apps build today: macOS via
> `xcodebuild -project app/rPlayHub.xcodeproj -scheme rPlayHub -configuration Debug
> -derivedDataPath build/dd CODE_SIGNING_ALLOWED=NO build`, then run
> `build/dd/Build/Products/Debug/rPlayHub.app` and screenshot to verify; the engine is already
> running and the iPhone 13 live-mirrors. Linux `rplay-gui` builds on the Linux box (its Mac build
> hits a `-I../deps/ffmpeg` vs C++ `<version>` collision — Linux/CI is clean).
>
> Done already: macOS Edit+Device menus, the app-link regression fix (`usernet.h` `tun_*`
> fallbacks), and the Apps Filter+category dropdown on both apps. First tasks, in order:
> (1) **Engine API** for the `+`/`-` buttons — add `install_app`/`uninstall_app` and
> `install_profile`/`remove_profile` to `host-c/api_server.c` (see `app/api/PROTOCOL.md`), then wire
> the buttons on both front-ends. (2) **Inspector restructure** to Device Hub's two levels: a top
> row of 3 icon tabs (Settings/Report/Info) and, under Info, text sub-tabs Info/Apps/Profiles —
> fold our extra panels (Console, Files, Controls) in sensibly. (3) **Settings panel** (device
> Appearance/Text Size/Reduce Motion/…): needs new engine methods to read/set them. (4) Canvas
> **View Screen** button + static screenshot preview. (5) sidebar **Unavailable** section; toolbar
> `+`/list/keyboard/grid/`»`. (6) app-type field in `list_apps` for the App Clips category.
> Build + screenshot after each change; keep both platforms building; push frequently so the Linux
> agent stays in sync. The daemon needs sudo to (re)start — ask the user.
