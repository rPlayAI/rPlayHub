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
3. **DONE (bringup) — Tunnel bringup (Layers 0-2)** in `cdhost.c` `main`, via `imd_bringup` in
   `host-c/imd.c`: `idevice_new_with_options` + `lockdownd_client_new_with_handshake` +
   `lockdownd_get_value` + `lockdownd_start_service("...CoreDeviceProxy")` + `idevice_connect` +
   `idevice_connection_enable_ssl`. `conn_t` gained a `void *idev` field; `cwrite`/`cread_n` route
   through `imd_conn_send`/`imd_conn_recv` (blocking, exact-count). The CoreDeviceProxy handshake
   and the utun packet pump are UNCHANGED -- they run over conn_t. **Verified live non-root**: the
   whole bringup through the tunnel handshake works against the phone (us/device/RSD/mtu all come
   back). Only the utun (Layer 3a) needs sudo, and it uses the same conn_t path. This replaced the
   original design: Replace `usbmux_connect_port(LOCKDOWN)` +
   `lockdown_*` + `tls.h` + `usbmux_read_pair_record` with `idevice_new_with_options` +
   `lockdownd_client_new_with_handshake` + `lockdownd_start_service("...CoreDeviceProxy")` +
   `idevice_connect` (+ `idevice_connection_enable_ssl`). Feed the `idevice_connection_t` to the
   existing CoreDeviceProxy handshake + utun pump via a small read/write shim (replacing `conn_t`'s
   `cwrite`/`cread_n`). This is the big one; after it, `deps/AccessorySDK`'s CFLite and our whole
   usbmux/lockdown/TLS block are gone from the tunnel path.
4. **Remaining plist usage** in `api_server.c` (profiles) and `ddi.c` (mounter plists + TSS) →
   libplist. After this the C engine has **no CoreFoundation dependency**, so it builds on
   Linux/Windows. (`ddi.c` already leans on `libtatsu` conceptually; libplist finishes it.)
5. **DONE (macOS) — CoreFoundation removed.** With the transport on libimobiledevice, the 250-line
   hand-rolled usbmux/lockdown/TLS/CFLite block in cdhost.c is dead and deleted; the Makefile no
   longer links `-framework CoreFoundation`. `otool -L cdhost` shows only libimobiledevice +
   libplist (+ OpenSSL, libz). **The whole C engine is CoreFoundation-free** and builds/links with
   no macOS framework -- the portability goal. Bringup re-verified live after removal.
6. **DONE (static link)** -- `make STATIC=1` links the four .a archives (libimobiledevice,
   -glue, libusbmuxd, libplist; + `-lc++` since libplist is C++), so the shipped binary needs no
   installed libimobiledevice/libplist. `otool -L` then shows only libc++, OpenSSL, libz, libSystem
   -- and with OpenSSL also static (STATIC=1 links libssl.a/libcrypto.a) the binary depends on
   **only system libs** (libc++, libz, libSystem). Nothing to install, nothing to bundle. `scripts/package-test-dmg.sh`
   builds the embedded engine with STATIC=1, so the app is fully self-contained. Homebrew ships the
   .a files; a Linux/Windows build points the paths at its own static build.
7. **Port build**: on Linux/Windows link the same stack (+ libtatsu for the DDI if used) from the
   system or a vendored static build (rplay's `build-libimobiledevice-osx.sh` is
   the recipe; it builds all three static). Only the four OS seams in `app/PORTING.md` (tunnel
   interface, video decode/display, input capture) remain platform-specific; the transport no
   longer is.

## Status

Stages 1-5 done and **fully verified against the phone under sudo** (2026-08-24): the whole path
now runs on libimobiledevice -- Layers 0-2, the utun + packet pump over the idevice_connection,
and services over the tunnel (take_screenshot returned a 1170x2532 PNG; list_apps returned 301
apps; device_info / enumerate / list_profiles live). The C engine is CoreFoundation-free and links
no macOS framework. One follow-up fix was needed: idevice_connection_receive_timeout returns
SUCCESS with 0 bytes on a timeout, which the pump reader must treat as "keep waiting", not EOF
(commit b16d1ee) -- otherwise the pump dies when the tunnel goes quiet after startup.

## Notes

- The CoreDevice tunnel itself (RemoteXPC/RSD/media in `core/`) is NOT libimobiledevice's -- it is
  our own iOS-17 code and stays. The library only replaces the classic usbmux/lockdown substrate
  beneath it.
- `PLIST_INT` and `PLIST_UINT` are the same enum value in libplist 2.0.4 -- one case, use
  `plist_get_uint_val`.
- The classic shim services we already speak over the tunnel (diagnostics_relay, afc, misagent,
  MCInstall, syslog, mobile_image_mounter) are reached through the tunnel with our own framing, not
  through libimobiledevice's usbmux-based service clients -- so those do not change.
