# Control API — newline-delimited JSON

Adopted from `~/rplay`'s contract (`refs/rplay/doc/sdk-api.md`) so its existing Python client and MCP
wrapper drive rplay-hub unchanged. Implemented today by `host/mirror.py`; `app/api/SDKServer.swift`
is the cloned Swift implementation of the same contract, still wired to rplay's own backend.

Two sockets, both **localhost only** — this is full device control, so it must not be reachable off
the machine:

| port | what |
|---|---|
| `127.0.0.1:9876` | control. One JSON object per line in, one per line out. |
| `127.0.0.1:9877` | video. A raw Annex-B HEVC byte stream, one per connected viewer. |

## Envelope

```json
{"id": 1, "method": "tap", "params": {"x": 200, "y": 400}}
{"id": 1, "ok": true, "result": {"x": 2800, "y": 2588, "logical_max": 16384}}
{"id": 2, "ok": false, "error": {"code": "bad_request", "message": "…"}}
```

`id` is echoed when present and omitted when absent, so notification-style calls work. Error codes:
`bad_json`, `bad_request`, `unsupported_method`, `internal_error`. rplay's wider vocabulary
(`device_not_found`, `device_disconnected`, `auth_required`, …) is reserved for when we grow into it.

Requests are serialized per connection **and** across connections, because a `coredevice.*` service
channel is a single XPC socket that cannot safely interleave. A screenshot therefore briefly blocks
taps; splitting the channels per method is the fix when that starts to matter.

## Methods

### `ping` → `{"pong": true}`
Liveness. Costs nothing, touches no device state.

### `list_devices` → `{"devices": [...]}`
```json
{"id": "00008110-…", "udid": "00008110-…", "name": "…", "product_type": "iPhone14,2",
 "os_version": "27.0", "connected": true, "transport": "usbmux",
 "screen_size": {"w": 1170, "h": 2532}}
```
One device today — the engine is single-session. `screen_size` is real device pixels, read from the
IHDR of a screenshot taken once at startup, and it is what pixel coordinates below are resolved
against. It is absent if that probe failed.

### `take_screenshot` → `{"format": "png", "width", "height", "image_b64"}`
PNG only: `screencaptureservice` returns PNG, so asking for `jpeg` is rejected rather than silently
answered in the wrong format. (rplay's version defaults to JPEG because it re-encodes a window grab.)

### `tap` → `{"x", "y", "logical_max": 16384}`
Params: `x`,`y` in device pixels, **or** `fx`,`fy` as 0..1 fractions. Optional `duration_ms`
(default 60). Coordinates are clamped into range — the HID report masks x/y to 16 bits, so an
out-of-range value would otherwise land somewhere else on screen instead of erroring.

The response is in rplay's 0..16384 logical space with `logical_max` echoed, so its clients keep
working. Internally we use 0..1 fractions, which `hid.py` scales to the report's 0..65535.

### `swipe` → `{"from": [x,y], "to": [x,y], "logical_max": 16384}`
Params: `x0`,`y0`,`x1`,`y1` in pixels, or `fx0`,`fy0`,`fx1`,`fy1` as fractions. Optional
`duration_ms` (default 300).

### `start_recording` → recording stats
Params: `path` (engine-local; defaults to a timestamped file under `recordings/`) and
`wait_for_keyframe` (default false).

Recording happens **in the engine**, which already has every NAL and the cached parameter sets, so
it writes a self-describing Annex-B file without the app re-encoding anything. The file opens with
the cached VPS/SPS/PPS.

The catch is real and worth understanding: **the device emits an IRAP only about every 10 seconds.**
A recording started at an arbitrary moment therefore begins mid-GOP, and a decoder cannot resolve
its reference frames until the next keyframe — ffprobe reports `nb_read_frames=N/A` on such a file
even though it is valid HEVC. Two ways to deal with it:

- default (`wait_for_keyframe: false`) — take everything immediately. A short recording is never
  empty, but may start mid-GOP. `saw_keyframe` in the result tells you which happened.
- `wait_for_keyframe: true` — drop leading slices until an IRAP arrives, so the file is decodable
  from its first frame, at the cost of starting up to a GOP (~10 s) late.

The real fix is **RTCP PLI**: ask the device for a keyframe when recording starts. Not built — this
is the concrete reason to build it, beyond cleaner video.

### `stop_recording` → final stats
```json
{"path": "…/recordings/00008110-…-20260727-222500.h265",
 "nals": 126, "bytes": 1511381, "duration_s": 4.3,
 "saw_keyframe": false, "skipped_before_keyframe": 0, "waiting_for_keyframe": false}
```

### `stream_info` → stream health
```json
{"port": 9877, "codec": "hevc", "container": "annexb", "viewers": 1,
 "nals": 48213, "rtp_packets": 51002, "uptime_s": 124.6}
```
Not in rplay's surface; added because "is video actually flowing" is the first question when a live
view looks wrong.

### `quit` → `{"quitting": true}`
Stops the daemon (it `_exit`s right after replying). Exists so a stuck-looking daemon can be
ended without a signal or sudo: `printf '{"id":1,"method":"quit"}\n' | nc 127.0.0.1 9876`.

### `device_action` → `{"action", "status": "Success"}`
Params `{"action": "restart" | "shutdown" | "sleep"}`. Speaks `diagnostics_relay.shim.remote`
(classic plist framing plus the RSDCheckin preamble). `restart` drops the tunnel; the app
reconnects about 45 s later. `shutdown` leaves the phone off until its side button is pressed.

### `list_apps` → `[{"bundleIdentifier", "name", "version", "isFirstParty", "isDeveloper", "isAppClip"}, ...]`
A JSON **array**, from `installation_proxy`'s Browse (appservice's `listapps` validates and then
never answers on iOS 26.5). `isFirstParty` is `ApplicationType != User`. `isDeveloper` is
`get-task-allow` out of the app's `Entitlements` — the flag Xcode sets `true` on a
development-signed build (to let a debugger attach), `false` on an App Store or ad-hoc one. This,
not `isFirstParty`, is Device Hub's actual "Developer" filter: confirmed against the live app,
whose Developer bucket held only 3 sideloaded test builds under one signer, not every third-party
app the way "not Apple's own" would. `isAppClip` is `IsAppClip` verbatim, for the App Clips filter.

### `list_processes` → `{"processTokens": [{"processIdentifier", "executableURL": {"relative"}}]}`
### `launch_app` → `{"processToken": {"processIdentifier", "executableURL", ...}}`
Params `{"bundle_id"}`. Brings the app to the foreground, terminating a running instance first.
### `terminate_app` → `{}`
Params `{"pid", "signal": 9}`. Find the pid with `list_processes`.

### `install_app` → `{"status": "Complete"}`
Params `{"path"}`: a local `.ipa` file (on the machine running the engine, not the device). Staged
into the device's `/PublicStaging` over AFC (the same Media-rooted service `list_dir`/`read_file`
use), then installed via `installation_proxy`'s classic `Install` command. Streams `{Status: ...}`
events internally like `list_apps`' Browse; only the final status is returned. Device Hub's Apps-tab
`+`.

### `uninstall_app` → `{"status": "Complete"}`
Params `{"bundle_id"}`. `installation_proxy`'s `Uninstall` command. Device Hub's Apps-tab `-`.

### `list_profiles` → `{"provisioning": [...], "configuration": [...]}`
Provisioning profiles from `misagent` (`name`, `app_id_name`, `team`, `uuid`, `expires`, `devices`),
configuration profiles from `MCInstall` (`identifier`, `name`, `organization`).

### `install_profile` → `{"status": "..."}`
Params `{"path"}`: a local `.mobileprovision` or `.mobileconfig` file, dispatched by extension. A
`.mobileprovision` is sent to `misagent` as its raw CMS-signed bytes (`MessageType: Install`); a
`.mobileconfig` is sent to `MCInstall` as its raw plist bytes (`RequestType: InstallProfile`, key
`Payload`). Neither is parsed or re-signed, only forwarded. Device Hub's Profiles-tab `+`.

### `remove_profile` → `{"status": "..."}`
Params `{"type": "provisioning", "uuid": "..."}` (misagent `Remove`, keyed by the UUID
`list_profiles` returns) or `{"type": "configuration", "identifier": "..."}` (MCInstall
`RemoveProfile`, keyed by the identifier `list_profiles` returns). Device Hub's Profiles-tab `-`.

`install_profile`/`remove_profile` are request/reply, not streaming: success is a misagent `Status`
of `0` or an MCInstall `Status` of `Acknowledged`/`Success`; anything else is an error.

### `device_info` → `{"default": {...}, "com.apple.mobile.battery": {...}, "com.apple.disk_usage": {...}, "unavailable": {...}}`
Optional `{"udid"}` (any attached device; default the bound one). Lockdown GetValue over a
session, the Info tab's data, in the shape `host/deviceinfo.py` used to produce.

### `list_dir` → `{"path", "entries": [{"name", "size", "mtime", "is_dir"}]}`
Params `{"service": "media" | "crash", "path"}`. AFC over the Media partition or the crash-report
directory (the mover is poked first so fresh reports appear).
### `read_file` → `{"size", "data_b64"}`
Same params; whole file, capped at 64 MB.
### `export_crashes` → `{"copied", "failed", "dir"}`
Params `{"dir"}`: copies every `.ips`/`.crash`/`.panic` into that local directory.

### `syslog` → `{"streaming": true}`, then events
The one streaming method. After the reply the same connection carries
`{"event": "syslog", "line": "..."}` objects, one per log line, until the client closes its side
(or sends any byte). Open a dedicated connection for it; nothing else can be called on that
socket while it streams.

## The video stream

Raw Annex-B HEVC (`00 00 00 01` start codes), no container, no timestamps. Decode it with
`ffplay -fflags nobuffer -flags low_delay tcp://127.0.0.1:9877`, or feed it to a
`VTDecompressionSession`.

A viewer that connects mid-stream is sent the **cached VPS, SPS and PPS first**, because without
those a decoder has nothing to configure itself with and shows nothing. A viewer that cannot keep up
is dropped rather than being allowed to block the capture thread.

There are no per-frame timestamps yet. A player will pace off its own clock, which is right for a
live mirror; recording or A/V sync would need the RTP timestamps carried through.

## Not implemented, deliberately

- **Auth.** rplay's design puts a token in `~/.tarplay/token` at mode `0600`, printed once at first
  launch, Docker/Jupyter style. That is the right model and should be adopted before this ever binds
  to anything but localhost.
- **Events / subscriptions.** rplay designs `subscribe` with `device.connected`,
  `device.disconnected`, `frame.ready`. A GUI will want these; the shape is already specified in
  `refs/rplay/doc/sdk-api.md`.
- **Multi-device.** The engine holds one session. The device registry that makes this `adb`-shaped
  is designed but not built.
- `type_text`, `press_button`, the grid helpers, `wait_for_change` — all specified upstream,
  none needed for a View Screen.
