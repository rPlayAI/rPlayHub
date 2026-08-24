# Migrating off hand-rolled usbmux/lockdown to libimobiledevice

Why: the Linux and Windows ports need `libusbmuxd` (talks to the open-source `usbmuxd`) and
`libplist` instead of our hand-rolled usbmux client and the CFLite/CoreFoundation plist layer.
The library also collapses ~400 lines of usbmux + lockdown + TLS + pair-record handling into a
handful of calls, and it is battle-tested. macOS links the Homebrew stack; a port points
`IMD_PREFIX` at the system or a vendored build.

## Viability — proven 2026-08-24

A spike (`scratch: limd_spike.c`) did our whole Layer 0-1.5 against the live phone with the library:
`idevice_get_device_list_extended` (Layer 0), `lockdownd_client_new_with_handshake` (Layer 1 + TLS
session), `lockdownd_get_value` (device info), and `lockdownd_start_service` returned the
**CoreDeviceProxy** tunnel port + ssl flag (Layer 2 entry). So the tunnel bringup maps cleanly:
the library gets us to a TLS'd connection on the CoreDeviceProxy port, and our own `core/` code
does the CoreDeviceProxy handshake over it.

## Stages (keep the daemon building and working at each step)

1. **DONE — `device_info`** (`host-c/imd.c`). The Info tab's lockdown query, moved off
   usbmux+lockdown+CoreFoundation onto `idevice`/`lockdownd` + libplist. Same JSON shape; verified
   live (all domains, including the session-only battery/disk). Makefile links
   `-limobiledevice-1.0 -lplist-2.0`.
2. **DONE — `usbmux_enumerate`** (device list + per-device names, used by `list_devices`). Now
   `idevice_get_device_list_extended` + a session-less `lockdownd_get_value` per device for the
   name/version/type, in `host-c/imd.c`; -60 lines from cdhost.c. Returns exactly what usbmuxd
   reports (verified equal to `idevice_id -l`); network devices appear when their wifi connection
   is live, same as before. `device_id` dropped (the library keys on udid).
3. **Tunnel bringup (Layers 0-2)** in `cdhost.c` `main`. Replace `usbmux_connect_port(LOCKDOWN)` +
   `lockdown_*` + `tls.h` + `usbmux_read_pair_record` with `idevice_new_with_options` +
   `lockdownd_client_new_with_handshake` + `lockdownd_start_service("...CoreDeviceProxy")` +
   `idevice_connect` (+ `idevice_connection_enable_ssl`). Feed the `idevice_connection_t` to the
   existing CoreDeviceProxy handshake + utun pump via a small read/write shim (replacing `conn_t`'s
   `cwrite`/`cread_n`). This is the big one; after it, `deps/AccessorySDK`'s CFLite and our whole
   usbmux/lockdown/TLS block are gone from the tunnel path.
4. **Remaining plist usage** in `api_server.c` (profiles) and `ddi.c` (mounter plists + TSS) →
   libplist. After this the C engine has **no CoreFoundation dependency**, so it builds on
   Linux/Windows. (`ddi.c` already leans on `libtatsu` conceptually; libplist finishes it.)
5. **Port build**: on Linux/Windows link libimobiledevice + libplist (+ libusbmuxd, libtatsu) from
   the system or a vendored build; drop `-framework CoreFoundation`.

## Notes

- The CoreDevice tunnel itself (RemoteXPC/RSD/media in `core/`) is NOT libimobiledevice's -- it is
  our own iOS-17 code and stays. The library only replaces the classic usbmux/lockdown substrate
  beneath it.
- `PLIST_INT` and `PLIST_UINT` are the same enum value in libplist 2.0.4 -- one case, use
  `plist_get_uint_val`.
- The classic shim services we already speak over the tunnel (diagnostics_relay, afc, misagent,
  MCInstall, syslog, mobile_image_mounter) are reached through the tunnel with our own framing, not
  through libimobiledevice's usbmux-based service clients -- so those do not change.
