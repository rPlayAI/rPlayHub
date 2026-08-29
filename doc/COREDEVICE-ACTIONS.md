# CoreDevice actions — the catalog, and the Settings tab protocol

What Device Hub actually sends to change a device setting, plus the complete list of actions
Apple's own frameworks know about. Established 2026-08-28 from a live capture plus the framework
binaries; see `doc/DEVICEHUB-UI-CLONE.md` for how this fits the UI clone.

## It is not usbmux

Checked live with a device connected: `DeviceHub.app` holds **zero** connections to
`/var/run/usbmuxd`. Every device connection it has is TCP over IPv6 to the CoreDevice tunnel's
ULA address (`fd..::1`) on the mtu-16000 utun. usbmux is used only by `remotepairingd` to bring
that tunnel up; no service traffic rides it.

That is what makes this tractable: the tunnel is cleartext, so plain `tcpdump` on it sees
everything, and none of the invasive alternatives are needed — no usbmuxd socket proxy (moving
`/var/run/usbmuxd` aside), no dtrace against an Apple-signed binary (SIP blocks it anyway).

    host/capture-settings.sh utun10     # sudo; start BEFORE opening the tab
    python3 host/decode_settings.py settings-*.pcap

## The envelope

The same CoreDevice envelope screenshots and media already use (`host/coredevice.py`,
`core/rp_coredevice.c`) — but carrying **only an `actionIdentifier`, no `featureIdentifier`**.
`rp_cd_invoke` already accepts exactly that (NULL feature + action), so the engine needs no new
transport code.

```
request
    CoreDevice.CoreDeviceDDIProtocolVersion = 2
    CoreDevice.coreDeviceVersion            = {components, originalComponentsCount, stringValue}
    CoreDevice.deviceIdentifier             = <UUID>
    CoreDevice.invocationIdentifier         = <fresh UUID per call>
    CoreDevice.actionIdentifier             = com.apple.coredevice.action.setshowborders
    CoreDevice.input.showBorders.enabled    = true

response
    CoreDevice.output.showBorders.enabled   = true
```

**The payload key is not derivable from the action name.** It has to be taken as observed:

All twelve read actions were run against the device via `get_settings` on 2026-08-28, so these
shapes are **observed, not inferred**. There are **three different nesting conventions** — most
wrap in a named key, but two are flat and one is an enum. Assuming one convention for all of them
is the obvious way to send a malformed write:

| Setting (Device Hub row) | get / set action | payload shape |
|---|---|---|
| Show Borders | `get/setshowborders` | `{showBorders: {enabled: bool}}` |
| Increase Contrast | `get/setdeviceincreasecontrast` | `{increaseContrast: {enabled: bool}}` |
| VoiceOver | `get/setvoiceover` | `{voiceOverConfiguration: {enabled: bool}}` |
| Reduce Motion | `get/setreducemotion` | `{reduceMotion: {enabled: bool}}` |
| Reduce Transparency | `get/setreducetransparency` | `{reduceTransparency: {enabled: bool}}` |
| Color Filter | `get/setcolorfilter` | `{colorFilter: {enabled: bool}}` |
| Liquid Glass | `get/setliquidglassconfiguration` | `{configuration: {opacity: double 0..1}}` |
| Text Size | `get/setdevicetextsize` | `{textSize: {size: {<case>: {}}}}` — **enum**, see below |
| Appearance (Light/Dark) | `get/setuserinterfacestyle` | `{style: "light"}` — **flat**, no wrapper |
| Larger Accessibility Sizes | `get/setlargeraccessibilitysizesenabled` | `{enabled: bool}` — **flat** |
| Look and Feel | `get/setdevicelookandfeel` | `{lookAndFeel: {name: "Liquid Glass"}}` |
| (supported values) | `getsupportedlooksandfeels` | `{looksAndFeels: [{name: …}]}` — read-only |
| Location | `setsimulatedlocation` | `{latitude: double, longitude: double}` — **flat** |
| Location (clear) | `clearsimulatedlocation` | none |

**Text Size is an enum encoded as a single-key dictionary**, not a number or a string: the device
returned `{textSize: {size: {large: {}}}}`. The case name is the key and its value is an empty
dictionary — the XPC encoding of a Swift enum case with no associated value. Other case names
(`small`, `medium`, `extraLarge`, …) have not been enumerated; `getcustomizableappearanceelements`
is the likely place to discover the valid set.

A live read of the test device (iPhone13, iOS 27.0) returned: everything boolean `false`,
`textSize` `large`, `appearance` `light`, `lookAndFeel` `Liquid Glass` (the only supported one),
`liquidGlass.opacity` `0.55`.

## Which service hosts them — `com.apple.coredevice.configuration`

**Confirmed against the device, 2026-08-28.** `settings_probe` asked four candidates for
`getreducemotion`; only one answered:

```
com.apple.coredevice.configuration   ANSWERED   {"reduceMotion": {"enabled": false}}
com.apple.coredevice.devicecontrol   no output (rc=-3, never answered)
com.apple.coredevice.deviceinfo      no output (rc=-4, replied without CoreDevice.output)
com.apple.coredevice.appservice      no output (rc=-4)
```

So all the appearance/accessibility actions bind to `com.apple.coredevice.configuration`.
Location is separate (`com.apple.coredevice.locationservice`), as the capture already showed.

Note that `deviceinfo` and `appservice` *replied* but without `CoreDevice.output` (rc=-4), while
`devicecontrol` never answered at all (rc=-3) — a service answering the envelope is not evidence
it implements the action, which is exactly why the probe checks for output rather than for a
reply.

Re-run it any time with the daemon up:

    python3 -c "import socket;s=socket.create_connection(('127.0.0.1',9876));\
    s.sendall(b'{\"id\":1,\"method\":\"settings_probe\"}\n');print(s.recv(65536).decode())"

RSD ports are assigned per session, so the port above is not stable across runs — the service
*name* is what to resolve against, and `cdhost` now resolves it into
`api_session.configuration_port` at startup.

## The complete action catalog

Every `com.apple.coredevice.action.*` string in
`/Library/Developer/PrivateFrameworks/CoreDeviceUtilities.framework` and
`CoreDevice.framework` (Xcode 26). **Grep those binaries before capturing again** — an action's
existence and exact spelling can be settled offline; only its payload shape needs the wire.

Being listed here means Apple's framework knows the name. It does **not** mean the device
advertises it, that we have permission to call it, or that the payload shape is known.

**Appearance / accessibility** (the Settings tab)
`getcolorfilter` `setcolorfilter` `getcustomizableappearanceelements`
`getdeviceincreasecontrast` `setdeviceincreasecontrast` `getdevicelookandfeel`
`setdevicelookandfeel` `getdevicetextsize` `setdevicetextsize`
`getlargeraccessibilitysizesenabled` `setlargeraccessibilitysizesenabled`
`getliquidglassconfiguration` `setliquidglassconfiguration` `getreducemotion` `setreducemotion`
`getreducetransparency` `setreducetransparency` `getshowborders` `setshowborders`
`getsupportedlooksandfeels` `getuserinterfacestyle` `setuserinterfacestyle` `getvoiceover`
`setvoiceover`

**Location**
`availablelocationscenarios` `setlocationscenario` `setsimulatedlocation`
`clearsimulatedlocation` `startlocationsimulation`

**Apps, profiles, files** (mostly already covered by our libimobiledevice paths)
`appinstall` `appuninstall` `launch` `spawn` `fetchappicon` `streamapplist`
`configurationprofile.install` `configurationprofile.remove` `configurationprofiles.list`
`provisioningprofile.install` `provisioningprofile.remove` `provisioningprofiles.list`
`listfiles` `listroots` `rootinstall` `rootuninstall` `devicefs` `filenodedetails`
`transferfiles` `receivefiles` `rsyncfiles` `streamfilechanges` `projectsetinstall`
`filehandle.*` (openfile, opendirectory, createdirectory, createsymboliclink, readsymboliclink,
removefile, removedirectory, rename, fileoperation, directoryoperation, getattributes,
setattributes, getextendedattribute, setextendedattribute, listextendedattributes,
removeextendedattribute, getrealpath, createsession, endsession)

**Device info and lifecycle**
`displayinfo` `displayinfoupdates` `loadextendeddeviceinfo` `mobilegestaltquery` `gettrainname`
`featureflags` `lockstate` `reboot` `renamedevice` `enterdfu` `setnvram` `sysdiagnose`
`tags` `pair` `unpair` `provisiondevice` `removeprovisioneddevice` `default.user.credentials`
`acquireusageassertion` `listusageassertions`

**Media / screen** (already implemented)
`capturescreenshot` `mediastreamstart` `mediastreamstop` `mediastreamstatus`
`mediastreamgetsupportinfo` `prepareresizabledisplay` `streamresizabilitystate`
`snapshotfetchscreenshots`

**Processes**
`streamprocesslist` `sendsignaltoprocess` `sendmemorywarningtoprocess` `terminationhandler`

**DDI / service plumbing**
`enableddiservices` `disableddiservicesaction` `fetchddimetadata` `updatehostddis`
`removehostddis` `fetchdyldsharedcachefiles` `fetchmachodylibs` `createservicesocket`
`listserviceplugins` `connect` `disconnect` `authListingIdentifiers`
`darwinnotificationobserve` `darwinnotificationpost` `listhostaudiodevices`

**Virtualization / snapshots** (simulators and VMs, not physical devices)
`exportvirtualmachinearchive` `snapshotcreation` `snapshotremove` `snapshotresume`
`snapshotsetname`

## Related files

- `host/settings.py` — action/payload table, `probe` / `get` / `set`. Reference implementation
  to port into `host-c/api_server.c`.
- `host/capture-settings.sh`, `host/decode_settings.py` — how the above was obtained.
- `reference/rsd-services-ios27.json`, `doc/RSD-SERVICES.md` — the service catalog the ports
  were matched against.
- `core/rp_coredevice.h` — `rp_cd_invoke`, which already takes an action with a NULL feature.


## Two transports, and why cdhost can go blind while Device Hub does not (2026-08-29)

Worth knowing before debugging "no devices" again. There are two independent ways to reach a
phone from this Mac, and they share nothing:

| | Transport | Needs usbmuxd? |
|---|---|---|
| Device Hub | CoreDevice / RemotePairing | **no** |
| cdhost | usbmux (USB, or Wi-Fi sync) | **yes** |

`cdhost` starts at `idevice_get_device_list_extended`, which asks **usbmuxd**. Our libimobiledevice
use is fully Wi-Fi-aware — every `idevice_new_with_options` passes
`IDEVICE_LOOKUP_USBMUX | IDEVICE_LOOKUP_NETWORK`, and enumeration collapses the USB and Network
entries for one phone into a single row — but libimobiledevice's "network" still means *a device
usbmuxd is publishing*, discovered over Bonjour `_apple-mobdev2._tcp` (Wi-Fi sync). It does not
mean "reachable on the network".

Observed 2026-08-29: usbmuxd reporting **zero** devices and `idevice_id -l` empty, while Device Hub
had a live tunnel and the device answered `ping6` across it in 6ms. Both true at once. The old
workaround — open Device Hub and cdhost starts working — relied on the phone *also* having a live
Wi-Fi-sync association for Device Hub to nudge; with only the CoreDevice pairing up, there is
nothing to nudge and the workaround stops helping. The reliable fix is a USB cable.

### Attaching to an existing tunnel (experimental, `RPLAY_ATTACH_TUNNEL=1`)

Because that CoreDevice tunnel is an ordinary routed utun, anything on the machine can use it —
which makes a usbmux-free path possible. `cdhost` can now find the tunnel and skip Layers 0-3a:

    RPLAY_ATTACH_TUNNEL=1 ./host-c/cdhost              # finds the tunnel, searches for RSD
    RPLAY_ATTACH_TUNNEL=1 RPLAY_ATTACH_RSD=52722 ...   # skip the search
    RPLAY_ATTACH_SCAN=49152-57000                      # widen the search range

**Working:** tunnel discovery (a utun with a `fd..` address and the 16000 MTU those use), deriving
the device address (`::1` against our `::2`), and reaching RSD — `✅ RSD reachable` against Device
Hub's own tunnel.

**The RSD port has to be searched for**, because it is chosen per session and only the app that
built the tunnel is told it. Measured on two tunnels it sits at the bottom of the open set (52722
with 52721 beside it; 51724 on one of ours) while the ~85 service ports cluster far above
(56104+), so the lowest handful of open ports is a short list rather than a guess.

**Not working yet:** `rsd_enumerate` gets "no RSD answer after 0 message(s)" on a foreign tunnel,
where `host/remotexpc.py` completes the same handshake against the same address and port and
returns all 85 services. So the transport and routing are fine and the gap is in the C RemoteXPC
handshake specifically — that is the one thing left to debug before this path is usable.

Note the machine had two ULA prefixes up (Tailscale `fd7a:...` alongside CoreDevice `fd9e:...`),
which is worth ruling in or out as a source-address-selection problem, though a plain TCP connect
to the same port succeeds.
