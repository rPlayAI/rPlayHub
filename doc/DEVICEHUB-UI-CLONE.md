# Device Hub UI clone — exact structure to match

From close side-by-side examination of Apple's Device Hub (Xcode 26) against rPlayHub on
2026-08-28. This is the target for making both front-ends (macOS `app/rPlayHub/`, Swift/AppKit;
Linux `client-c/rplay-gui.cpp`, Dear ImGui) pixel-faithful clones. What's done and what remains
are marked.

## Window: three areas (both apps already have this shape)

`[ sidebar ] [ canvas ] [ inspector ]`

## Toolbar (top of window) — Device Hub

Left→right: `+` (add device), a list icon, the sidebar-toggle, the device title
(`iPhone13 / iOS 27.0`), a keyboard icon + a grid icon (paired, greyed when N/A), and a `»`
expander at the far right. **rPlayHub has only a title + sidebar toggle — the rest are missing.**

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

## Remaining, in rough priority

1. **Engine API for `+`/`-`**: `install_app`/`uninstall_app`, `install_profile`/`remove_profile`
   (backend; unblocks the buttons on both front-ends).
2. **Inspector restructure**: 3 icon tabs (Settings/Report/Info) + text sub-tab row — both apps.
3. **Settings panel**: the device appearance/accessibility controls (needs engine methods to read/set
   them — a new capability).
4. **Canvas View Screen button** + static screenshot preview.
5. **Sidebar Unavailable section**; **toolbar** `+`/list/keyboard/grid/`»`.
6. App-type field in `list_apps` (for the App Clips category).
