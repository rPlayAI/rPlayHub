#!/usr/bin/env python3
"""Everything lockdown will tell us about a device, as one JSON object.

This is the Diagnostics tab's data source. It deliberately does NOT go through the CoreDevice
tunnel: lockdown rides usbmuxd directly, so it needs neither root nor a running cdhost, and it
keeps working while the tunnel is down or the stream has stopped. That independence is the whole
reason the panel can be useful when something is wrong.

    python3 host/deviceinfo.py                 # first attached device
    python3 host/deviceinfo.py --udid <udid>
    python3 host/deviceinfo.py --raw           # every key lockdown returns, not just the panel's

Domains beyond the default one need a session, and a device that is locked or not trusted will
refuse some of them. Anything unavailable is reported as such rather than omitted, because "we
could not read the battery" and "this device has no battery" are different answers and the panel
should not have to guess which it got.
"""
import argparse
import json
import sys

import lockdown
import usbmux

# Domain -> the keys worth showing. Restricting the query matters: asking for a whole domain
# returns dozens of internal keys that change between releases, and one unreadable key inside a
# bulk request can fail the request rather than the key.
PANEL = {
    None: [
        "DeviceName", "ProductType", "ProductVersion", "BuildVersion", "ProductName",
        "UniqueDeviceID", "SerialNumber", "ModelNumber", "RegionInfo", "HardwareModel",
        "CPUArchitecture", "DeviceClass", "DeviceColor", "ChipID", "UniqueChipID",
        "ActivationState", "PasswordProtected", "TimeIntervalSince1970", "TimeZone",
        "WiFiAddress", "BluetoothAddress", "EthernetAddress", "FirmwareVersion",
    ],
    "com.apple.mobile.battery": [
        "BatteryCurrentCapacity", "BatteryIsCharging", "ExternalConnected", "ExternalChargeCapable",
    ],
    "com.apple.disk_usage": [
        "TotalDiskCapacity", "TotalDataCapacity", "TotalDataAvailable", "TotalSystemCapacity",
        "TotalSystemAvailable", "AmountDataAvailable", "AmountDataReserved",
    ],
    "com.apple.mobile.data_sync": ["DeviceCapacity"],
}

# UniqueChipID is the ECID, which everyone reads as hex; TimeIntervalSince1970 is a float epoch.
# Presentation stays here rather than in the UI so a second front end does not reinvent it.
DERIVED = {
    "ECID": lambda v: format(int(v), "X") if v is not None else None,
}


def collect(ld, raw=False):
    out, errors = {}, {}
    if raw:
        try:
            return {"raw": ld.all_values()}, {}
        except Exception as e:                      # noqa: BLE001 -- report, never abort
            return {}, {"raw": str(e)}

    for domain, keys in PANEL.items():
        bucket = {}
        for key in keys:
            try:
                v = ld.get_value(key, domain)
            except Exception as e:                  # noqa: BLE001
                errors[f"{domain or 'default'}.{key}"] = str(e)
                continue
            if v is not None:
                bucket[key] = v
        if bucket:
            out[domain or "default"] = bucket

    default = out.get("default", {})
    if "UniqueChipID" in default:
        default["ECID"] = DERIVED["ECID"](default["UniqueChipID"])
    return out, errors


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--udid")
    ap.add_argument("--raw", action="store_true", help="every key lockdown returns")
    ap.add_argument("--no-session", action="store_true",
                    help="skip StartSession; only the unauthenticated default domain will answer")
    args = ap.parse_args()

    udid = args.udid
    if not udid:
        devices = usbmux.list_devices()
        if not devices:
            json.dump({"error": "no device attached"}, sys.stdout)
            print()
            return 1
        # usbmuxd calls it SerialNumber, and it is the UDID -- the actual hardware serial is a
        # different, shorter string that lockdown returns under SerialNumber too. Same name, two
        # meanings, one layer apart.
        udid = devices[0].get("SerialNumber") or devices[0].get("udid")

    result = {"udid": udid}
    try:
        ld = lockdown.LockdownClient(udid)
    except Exception as e:                          # noqa: BLE001
        json.dump({"udid": udid, "error": f"lockdown: {e}"}, sys.stdout)
        print()
        return 1

    # Without a session only the default domain answers, and battery/storage come back empty.
    # A failure here is worth reporting but not fatal: the panel still shows model and OS.
    if not args.no_session:
        try:
            result["session"] = ld.start_session()
        except Exception as e:                      # noqa: BLE001
            result["session_error"] = str(e)

    values, errors = collect(ld, raw=args.raw)
    result.update(values)
    if errors:
        result["unavailable"] = errors
    try:
        ld.close()
    except Exception:                               # noqa: BLE001
        pass

    json.dump(result, sys.stdout, indent=2, default=str)
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
