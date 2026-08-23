# RSD services — what an iPhone offers over the CoreDevice tunnel

Read from a live iPhone 13 Pro (`iPhone14,2`) on **iOS 27.0**, 2026-08-02, by handshaking with
RSD through the tunnel `cdhost` already holds. The raw map is `doc/rsd-services-ios27.json`.

Two things to know before using this:

- **Ports are per-session.** RSD assigns them at tunnel setup and they change every time. Look a
  service up by name at connect time; a port cached across sessions is a bug, and one cached
  across a reconnect is the bug that made `startmediastream` time out for days.
- **Names change between iOS releases.** This is one device on one release. Treat it as evidence,
  not as an API.

The `.shim.remote` suffix marks a classic lockdown service tunnelled over RemoteXPC. Those are the
same protocols `libimobiledevice` has spoken for years, which is why they are the cheapest ones to
implement — the wire formats are already known.

## Status column

- **used** — rPlayHub talks to it today
- **next** — a named feature depends on it and the protocol is understood
- **known** — well-documented service, nothing here needs it yet
- **unverified** — purpose inferred from the name alone; not probed

---

## Screen, input and media

| Service | Purpose | Status |
|---|---|---|
| `com.apple.coredevice.displayservice` | Negotiates and serves the live H.265 screen stream. The whole mirroring path. | **used** |
| `com.apple.coredevice.screencaptureservice` | Still screenshots. | **used** |
| `com.apple.coredevice.hid.universalhidservice` | Touch, keyboard and button injection. Surfaces: 257 touchscreen, 512 keyboard, 1026 mainScreenButtons, 1280 avpCustom, 1281 gesture. | **used** |
| `com.apple.coredevice.hid.universalhid` | Sibling of the above; relationship not established. | unverified |
| `com.apple.coredevice.hid.indigo` | Apple's internal HID transport name. Not probed. | unverified |
| `com.apple.coredevice.pasteboardservice` | Clipboard between Mac and device. | **next** |

## Device identity and control

| Service | Purpose | Status |
|---|---|---|
| `com.apple.coredevice.deviceinfo` | Device properties over CoreDevice, rather than lockdown. Would let the Diagnostics tab drop its Python subprocess. | **next** |
| `com.apple.mobile.diagnostics_relay.shim.remote` | **Restart, Shutdown, Sleep**, plus IORegistry queries. | **used** (`device_action`) |
| `com.apple.coredevice.diagnosticsservice` | CoreDevice-era diagnostics. Overlaps the relay above; which one Device Hub uses is not established. | unverified |
| `com.apple.coredevice.devicecontrol` | Device control surface. Not probed. | unverified |
| `com.apple.coredevice.configuration` | Device configuration. Not probed. | unverified |
| `com.apple.mobile.lockdown.remote.trusted` | Lockdown itself over the tunnel, on a trusted (paired) connection. | **next** |
| `com.apple.mobile.lockdown.remote.untrusted` | Lockdown before pairing — where a pair request begins. | **next** |
| `com.apple.dt.remotepairingdeviced.lockdown.shim.remote` | **Pairing** over RemotePairing. The service behind Pair / Unpair. | **next** |
| `com.apple.mobileactivationd.shim.remote` | Activation state and activation records. | known |
| `com.apple.mobile.assertion_agent.shim.remote` | Holds power assertions, keeping the device awake. | known |
| `com.apple.mobile.heartbeat.shim.remote` | Keepalive; the device drops idle sessions without it. | known |
| `com.apple.timed.remote` | Time synchronisation. | unverified |

## Apps and installation

| Service | Purpose | Status |
|---|---|---|
| `com.apple.mobile.installation_proxy.shim.remote` | Install, uninstall and enumerate apps. | **used** (`list_apps`, via Browse) |
| `com.apple.coredevice.appservice` | CoreDevice app control: launch, terminate, processes. Its `listapps` stalls on iOS 26.5. | **used** (`launch_app`, `terminate_app`, `list_processes`) |
| `com.apple.coredevice.iconservice` | App icons, for a list that shows them. | known |
| `com.apple.mobile.house_arrest.shim.remote` | Access an app's Documents container. | known |
| `com.apple.streaming_zip_conduit.shim.remote` | Streams an app bundle in during install. | known |
| `com.apple.remote.installcoordination_proxy` | Coordinates installs. | unverified |
| `com.apple.misagent.shim.remote` | Provisioning profiles. | **used** (`list_profiles`) |
| `com.apple.mobile.MCInstall.shim.remote` | Configuration profiles (MDM-style). | **used** (`list_profiles`) |
| `com.apple.backgroundassets.lockdownservice.shim.remote` | Background asset downloads. | unverified |

## Files and backup

| Service | Purpose | Status |
|---|---|---|
| `com.apple.afc.shim.remote` | Apple File Conduit — the media partition (`/var/mobile/Media`). | **used** (`list_dir`, `read_file`) |
| `com.apple.coredevice.fileservice.control` | CoreDevice file transfer, control channel. | **next** |
| `com.apple.coredevice.fileservice.data` | The matching data channel. | **next** |
| `com.apple.mobilebackup2.shim.remote` | Full device backup and restore. | known |
| `com.apple.mobilesync.shim.remote` | Contacts, calendars and the rest. | known |
| `com.apple.mobile.file_relay.shim.remote` | Legacy bulk file extraction. | known |
| `com.apple.mobile.storage_mounter_proxy.bridge` | Mounts storage. | unverified |
| `com.apple.mobile.mobile_image_mounter.shim.remote` | Mounts the Developer Disk Image. Required before most developer services will answer. | **used** (`host/ddi_mount.py` mounts end to end, no Xcode) |

## Logs, crashes and diagnostics

| Service | Purpose | Status |
|---|---|---|
| `com.apple.syslog_relay.shim.remote` | Live syslog. The Console panel. | **used** (`syslog`) |
| `com.apple.os_trace_relay.shim.remote` | Structured `os_log` stream — what Console.app actually shows. | **next** |
| `com.apple.crashreportcopymobile.shim.remote` | Copies crash reports off the device. Device Hub's Crashes panel. | **used** (`export_crashes`) |
| `com.apple.crashreportmover.shim.remote` | Moves crash reports into place before copying. | **used** |
| `com.apple.osanalytics.logTransfer` | Analytics log transfer. | unverified |
| `com.apple.sysdiagnose.remote` | Triggers and collects a sysdiagnose. | known |
| `com.apple.sysdiagnose.remote.trusted` | Same, on a trusted connection. | known |
| `com.apple.iosdiagnostics.relay.shim.remote` | Battery and hardware diagnostics. | known |
| `com.apple.corecaptured.remoteservice` | CoreCapture — wireless and media stack traces. | unverified |
| `com.apple.pcapd.shim.remote` | **Packet capture on the device itself.** How a phone-side pcap is taken without a proxy. | known |
| `com.apple.bluetooth.BTPacketLogger.shim.remote` | Bluetooth HCI logging. | known |

## Developer tools and debugging

| Service | Purpose | Status |
|---|---|---|
| `com.apple.coredevice.debugserverproxy` | Reaches `debugserver` — the LLDB attach path. | known |
| `com.apple.internal.dt.remote.debugproxy` | Internal debug proxy. | unverified |
| `com.apple.dt.testmanagerd.remote` | Runs XCTest bundles. | known |
| `com.apple.dt.testmanagerd.remote.automation` | UI automation half of the above. | known |
| `com.apple.instruments.dtservicehub` | Instruments: process list, sampling, counters. Where a process list comes from. | **next** |
| `com.apple.dt.remoteFetchSymbols` | Fetches symbol files for symbolication. | known |
| `com.apple.webinspector.shim.remote` | Safari Web Inspector for on-device pages. | known |
| `com.apple.GPUTools.MobileService.shim.remote` | GPU frame capture. | known |
| `com.apple.gputools.remote.agent` | GPU tools agent. | known |
| `com.apple.coredevice.openstdiosocket` | stdout/stderr socket for a launched process. | known |
| `com.apple.accessibility.axAuditDaemon.remoteAXService` | Accessibility audit — the tree a UI test walks. | known |
| `com.apple.accessibility.axAuditDaemon.remoteserver.shim.remote` | Shim form of the above. | known |
| `com.apple.springboardservices.shim.remote` | SpringBoard: icon layout, wallpaper, orientation *reporting*. | known |

## Networking and proxies

| Service | Purpose | Status |
|---|---|---|
| `com.apple.internal.dt.coredevice.untrusted.tunnelservice` | Sets up the tunnel itself, before trust. | known |
| `com.apple.internal.devicecompute.CoreDeviceProxy` | The proxy `cdhost` replaces by owning the tunnel directly. | known |
| `com.apple.internal.devicecompute.CoreDeviceProxy.shim.remote` | Shim form. | known |
| `com.apple.PurpleReverseProxy.Conn.shim.remote` | Reverse proxy, connection channel. | unverified |
| `com.apple.PurpleReverseProxy.Ctrl.shim.remote` | Reverse proxy, control channel. | unverified |
| `com.apple.companion_proxy.shim.remote` | Proxies to a paired Watch. | known |
| `com.apple.fusion.remote.service` | Not established. | unverified |
| `com.apple.internal.ptdremoted` | Not established. | unverified |
| `com.apple.mobile.insecure_notification_proxy.remote` | Darwin notifications, no pairing needed. | known |
| `com.apple.mobile.insecure_notification_proxy.shim.remote` | Shim form. | known |
| `com.apple.mobile.notification_proxy.remote` | Darwin notifications, paired. | known |
| `com.apple.mobile.notification_proxy.shim.remote` | Shim form. | known |

## Hardware, radio and platform

| Service | Purpose | Status |
|---|---|---|
| `com.apple.coredevice.locationservice` | Location simulation and reporting. | known |
| `com.apple.carkit.service.shim.remote` | CarPlay. | known |
| `com.apple.carkit.remote-iap.service` | CarPlay iAP accessory protocol. | known |
| `com.apple.atc.shim.remote` | Apple Type-C / accessory. | unverified |
| `com.apple.atc2.shim.remote` | Second generation of the above. | unverified |
| `com.apple.idamd.shim.remote` | IDAM — accessory management. | unverified |
| `com.apple.commcenter.mobile-helper-cbupdateservice.shim.remote` | Carrier bundle updates. | known |
| `com.apple.amfi.lockdown.shim.remote` | AMFI — code-signing enforcement state. | known |
| `com.apple.security.cryptexd.remote` | Cryptex (signed system extension) management. | known |
| `com.apple.RestoreRemoteServices.restoreserviced` | Restore / DFU-adjacent services. | known |
| `com.apple.preboardservice.shim.remote` | Pre-boot setup, before first unlock. | known |
| `com.apple.preboardservice_v2.shim.remote` | Version 2 of the above. | known |
| `com.apple.modelmanager.remote` | On-device model management. | unverified |
| `com.apple.private.siriappintentsd.orchestrator` | Siri app intents. | unverified |

---

## What is not here

**No orientation, accelerometer, motion or sensor service.** This was checked because rPlayHub's
Rotate button turns the view rather than the device, and that limitation had been asserted before
it was verified. Nothing advertised can rotate a physical device;
`springboardservices` reports orientation but does not set it.

## Bearing on planned features

| Feature | Service | Blocker |
|---|---|---|
| Restart / Shutdown | `mobile.diagnostics_relay` | built |
| Pair / Unpair | `dt.remotepairingdeviced.lockdown` + `lockdown.remote.untrusted` | the RemotePairing handshake is designed but not built (`doc/REMOTEPAIRING-PROTOCOL.md`) |
| Clipboard | `coredevice.pasteboardservice` | message format not decoded |
| Console | `syslog_relay` / `os_trace_relay` | syslog built; `os_trace` needs its binary format |
| Crash reports | `crashreportcopymobile` | built |
| App list | `coredevice.appservice` | built, with launch/terminate |
| Process list | `coredevice.appservice` (`listprocesses`) | built |
| Keyboard input | `hid.universalhidservice` (surface 512) | report format for the keyboard surface |
