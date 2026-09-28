# "no devices" — check this FIRST (2026-08-29)

Before investigating anything below, check whether **usbmuxd has gone stale**. That was the whole
of one long false trail on 2026-08-29.

    dns-sd -B _apple-mobdev2._tcp local     # is the phone advertising Wi-Fi sync?
    sudo launchctl kickstart -k system/com.apple.usbmuxd

Symptoms that look damning but are not:

- `cdhost` prints `no devices`; `idevice_id -l` is empty; a direct `ListDevices` over the usbmuxd
  socket returns zero. Three independent clients agreeing looks like ground truth -- but all three
  ask the same daemon, so they agree on its staleness too.
- Device Hub works perfectly throughout, because it uses RemotePairing over Bonjour and never
  touches usbmuxd.

The tell: **lockdown answering on `iPhone13.local:62078` while usbmuxd claims nothing exists.**
A device that reachable is not a missing device. Likewise, `ReadPairRecord` succeeding for a UDID
that `ListDevices` does not return means the pairing is intact and only discovery is broken.

On the day: the phone had been advertising `_apple-mobdev2._tcp` the entire time, with the same
instance name it had when this last worked, while usbmuxd had been up **9 days 22 hours** since
boot and had never re-registered it. `launchctl kickstart` replaced it with a 28-second-old process
and the device came back immediately as `Network`. Nothing was wrong with the phone, the pairing,
or the engine -- our libimobiledevice use already passes `IDEVICE_LOOKUP_NETWORK` everywhere.

If a kickstart does NOT bring it back, then the pairing for this specific Mac is the suspect and a
USB cable settles it. Only after that is the analysis below worth reading.

---

# CoreDevice & RemotePairing Wireless Wi-Fi Connection Architecture

## Overview

There are two distinct paths for connecting to an iPhone wirelessly over Wi-Fi without USB:

---

### Path A: Wireless Lockdown / CoreDeviceProxy (Port 62078)

Implemented & verified in [`scripts/test_wifi_lockdown_session.py`](file:///Users/you/experimental/rPlayHub/scripts/test_wifi_lockdown_session.py):

```
+-------------------------------------------------------------------------------+
| 1. Query Pair Record from usbmuxd or /var/db/lockdown/<UDID>.plist            |
|    - HostCertificate, HostPrivateKey, HostID, SystemBUID                      |
+---------------------------------------+---------------------------------------+
                                        |
                                        v
+-------------------------------------------------------------------------------+
| 2. TCP Connect to iPhone over Wi-Fi: iPhone.local:62078                       |
|    - Send QueryType -> validates lockdown service is reachable                |
+---------------------------------------+---------------------------------------+
                                        |
                                        v
+-------------------------------------------------------------------------------+
| 3. StartSession Request                                                       |
|    - Request: StartSession { HostID, SystemBUID }                             |
|    - Response: { EnableSessionSSL: True, SessionID: ... }                     |
+---------------------------------------+---------------------------------------+
                                        |
                                        v
+-------------------------------------------------------------------------------+
| 4. Mutual TLS Handshake over Wi-Fi                                            |
|    - Client cert/key: HostCertificate & HostPrivateKey                        |
|    - Upgrades socket to secure TLS stream                                     |
+---------------------------------------+---------------------------------------+
                                        |
                                        v
+-------------------------------------------------------------------------------+
| 5. Start CoreDeviceProxy Service                                              |
|    - Request: StartService { "Service": "com.apple.internal.devicecompute... }|
|    - Establishes RemoteXPC / CoreDevice channel wirelessly!                   |
+-------------------------------------------------------------------------------+
```

---

### Path B: Direct RemotePairing Protocol (mDNS / Bonjour)

Documented in [`doc/REMOTEPAIRING-PROTOCOL.md`](file:///Users/you/experimental/rPlayHub/doc/REMOTEPAIRING-PROTOCOL.md):

1. **mDNS Discovery**:
   - `_remotepairing._tcp` (control channel)
   - `_rp-tunnel._tcp` (encrypted data tunnel)
   - `_remotepairing-manual-pairing._tcp` (pair setup)
2. **Pair-Setup / Pair-Verify**:
   - SRP-6a PIN exchange for first-time pairing
   - Curve25519 / X25519 pair-verify for subsequent connections
   - Lockdown upgrade pairing (`upgradeAutomationLockdownPairing`)
3. **Encrypted Tunnel**:
   - ChaCha20-Poly1305 stream framing (`ControlChannelMessageEnvelope`) with OPACK-encoded messages.

---

### Key Scripts in Workspace:

- **[`scripts/test_wifi_lockdown_session.py`](file:///Users/you/experimental/rPlayHub/scripts/test_wifi_lockdown_session.py)**: Direct lockdown session + TLS + CoreDeviceProxy start over Wi-Fi.
- **[`scripts/test_wifi_tls_handshake.py`](file:///Users/you/experimental/rPlayHub/scripts/test_wifi_tls_handshake.py)**: Wireless TLS handshake verification with pair record.
- **[`scripts/test_pair_record_usbmuxd.py`](file:///Users/you/experimental/rPlayHub/scripts/test_pair_record_usbmuxd.py)**: Pair record extraction via `usbmuxd` protocol.
- **[`scripts/browse_bonjour_devices.py`](file:///Users/you/experimental/rPlayHub/scripts/browse_bonjour_devices.py)**: Bonjour browse for `_remotepairing._tcp` services.

---

## Correction after testing on this machine (2026-08-29)

**Path A verifies only as far as `StartService`.** `test_wifi_lockdown_session.py` prints its
success banner on receiving the StartService *reply* — it never connects to the port that reply
returns, so "CoreDeviceProxy Service Started over Wi-Fi" does not mean a usable channel.

Taking it one step further, the connection itself does not complete over Wi-Fi:

```
ReadPairRecord from usbmuxd              OK   (works even with usbmuxd listing NO devices)
iPhone13.local:62078 QueryType           OK   com.apple.mobile.lockdown
StartSession + mutual TLS                OK   EnableSessionSSL, SessionID
StartService(CoreDeviceProxy)            OK   Port 60395, EnableServiceSSL
connect that port, TLS, CDTunnel frame   EOF  device closes it
same without TLS                         RST  connection reset
```

So the port number arrives but the channel cannot be opened: TLS is clearly required (plain TCP is
reset outright) yet the device drops the connection the moment the `CDTunnel` handshake is sent.
That is consistent with CoreDeviceProxy being restricted to the usbmux transport — note Device Hub
does not use this path at all, it uses **Path B** (RemotePairing over Bonjour), which is a
different protocol entirely.

### There is no working Wi-Fi bringup to copy

Checked directly: `host-c-jetski/cdhost` — the older hand-rolled usbmux implementation — reports
`no devices` on this machine right now, exactly as the current engine does. It reaches the service
port through `usbmux_connect_port`, i.e. the usbmuxd proxy, so it depends on usbmuxd listing the
device just as much as `imd.c` does. Three independent implementations agree that usbmuxd has no
device: the current engine (libimobiledevice), jetski (raw usbmux socket), and `idevice_id -l`.

**So a Wi-Fi-independent engine means implementing Path B (RemotePairing), not Path A** — mDNS
discovery of `_remotepairing._tcp`, pair-verify over Curve25519, and the ChaCha20-Poly1305 framed
control channel. That is a substantial piece of work, and `doc/REMOTEPAIRING-PROTOCOL.md` is where
it would start. The cheap alternative remains a USB cable.
