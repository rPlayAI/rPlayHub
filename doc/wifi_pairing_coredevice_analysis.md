# CoreDevice & RemotePairing Wireless Wi-Fi Connection Architecture

## Overview

There are two distinct paths for connecting to an iPhone wirelessly over Wi-Fi without USB:

---

### Path A: Wireless Lockdown / CoreDeviceProxy (Port 62078)

Implemented & verified in [`scripts/test_wifi_lockdown_session.py`](file:///Users/you/experimental/rplay-hub/scripts/test_wifi_lockdown_session.py):

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

Documented in [`doc/REMOTEPAIRING-PROTOCOL.md`](file:///Users/you/experimental/rplay-hub/doc/REMOTEPAIRING-PROTOCOL.md):

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

- **[`scripts/test_wifi_lockdown_session.py`](file:///Users/you/experimental/rplay-hub/scripts/test_wifi_lockdown_session.py)**: Direct lockdown session + TLS + CoreDeviceProxy start over Wi-Fi.
- **[`scripts/test_wifi_tls_handshake.py`](file:///Users/you/experimental/rplay-hub/scripts/test_wifi_tls_handshake.py)**: Wireless TLS handshake verification with pair record.
- **[`scripts/test_pair_record_usbmuxd.py`](file:///Users/you/experimental/rplay-hub/scripts/test_pair_record_usbmuxd.py)**: Pair record extraction via `usbmuxd` protocol.
- **[`scripts/browse_bonjour_devices.py`](file:///Users/you/experimental/rplay-hub/scripts/browse_bonjour_devices.py)**: Bonjour browse for `_remotepairing._tcp` services.

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
