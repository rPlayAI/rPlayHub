#!/usr/bin/env python3
"""Verify Layers 0-1 against the attached iPhone, then scout the Layer 2 entry point.

Run: python3 probe.py [udid]
"""
import sys

import usbmux
import lockdown

# Candidate Layer-2 entry points (CoreDevice tunnel). The "untrusted" tunnel service is the
# iOS 17+ door that pymobiledevice3 uses over USB before any RemotePairing.
TUNNEL_SERVICES = [
    "com.apple.internal.dt.coredevice.untrusted.tunnelservice",
    "com.apple.internal.devicecompute.CoreDeviceProxy",
]


def main():
    udid = sys.argv[1] if len(sys.argv) > 1 else None

    print("== Layer 0: usbmux ==")
    devs = usbmux.list_devices()
    if not devs:
        print("  no devices attached"); return 1
    for d in devs:
        print(f"  DeviceID={d.get('DeviceID')} udid={d.get('SerialNumber')} "
              f"conn={d.get('ConnectionType')} pid={d.get('ProductID')}")
    if udid is None:
        udid = devs[0].get("SerialNumber")
    print(f"  -> using {udid}")

    print("\n== Layer 1: lockdown ==")
    lc = lockdown.LockdownClient(udid)
    info = {k: lc.get_value(k) for k in
            ("DeviceName", "ProductType", "ProductVersion", "BuildVersion", "UniqueChipID")}
    for k, v in info.items():
        print(f"  {k:15} = {v}")

    print("\n== Layer 1.5: lockdown session (pair record + TLS) ==")
    pr = usbmux.read_pair_record(udid)
    print(f"  pair record keys: {sorted(pr.keys())}")
    sess = lc.start_session(pr)
    print(f"  session started: {sess}")

    print("\n== Layer 2 scout: CoreDevice tunnel service ==")
    for svc in TUNNEL_SERVICES:
        try:
            r = lc.start_service(svc)
            print(f"  OK   {svc}\n         -> port={r['Port']} ssl={r['EnableServiceSSL']}")
        except lockdown.LockdownError as e:
            print(f"  no   {svc}\n         -> {e}")
    lc.close()

    print("\nLayers 0-1 verified against a real device. Next: Layer 2 (RemoteXPC + tunnel pairing).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
