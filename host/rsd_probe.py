#!/usr/bin/env python3
"""What can this device actually do? Brings the tunnel up, enumerates RSD, prints, exits.

The question this exists to answer: a `coredevice.*` service either is or is not advertised by
a given iOS version, and the only way to find out is to ask RSD — which is only reachable once
the tunnel is routed onto an interface. So this needs root, but it is quick and it changes
nothing on the device.

    sudo python3 host/rsd_probe.py [udid] [--all]

Without --all it prints the three services mirroring and control depend on, plus anything whose
name hints at screen, display, or HID. With --all it dumps the whole catalog.
"""
import sys
import time

from rplayhub import rsd
from rplayhub.net import TunnelPump, open_tun
from rplayhub.report import write_report
from rplayhub.transport.usbmux_transport import UsbmuxTransport, list_devices, probe_device

INTERESTING = ("screen", "display", "hid", "capture", "media", "video")


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("-")]
    show_all = "--all" in argv

    udid = args[0] if args else None
    devices = list_devices()
    if not devices:
        print("no devices visible to usbmuxd", file=sys.stderr)
        return 1
    if udid is None:
        udid = devices[0]["udid"]

    dev = next((d for d in devices if d["udid"] == udid), None)
    if dev is None:
        print(f"{udid} is not attached; visible: {[d['udid'] for d in devices]}", file=sys.stderr)
        return 1

    identity = probe_device(udid)
    print(f"device : {identity.get('DeviceName')} — {identity.get('ProductType')} "
          f"iOS {identity.get('ProductVersion')} ({identity.get('BuildVersion')})")
    print(f"link   : {dev['connection']}  udid {udid}")

    transport = UsbmuxTransport(udid)
    link = transport.open()
    print(f"tunnel : {link.params}")

    tun = open_tun()
    tun.configure(link.params)
    pump = TunnelPump(tun, link).start()
    try:
        time.sleep(0.5)
        catalog = rsd.ServiceCatalog.fetch(link.params.dev_addr, link.params.rsd_port)
        print(f"rsd    : {len(catalog)} services\n")

        print("services this project needs:")
        labels = {
            rsd.DISPLAY_SVC: "screen video + the HID auth gate",
            rsd.SCREENSHOT_SVC: "screenshots",
            rsd.HID_SVC: "touch injection",
        }
        for name, why in labels.items():
            port = catalog.port(name)
            mark = f"port {port}" if port else "NOT ADVERTISED"
            print(f"  {'OK ' if port else '-- '} {mark:<16} {name}\n      ({why})")

        missing = catalog.missing_core()
        print()
        if not missing:
            verdict = "can mirror and be controlled"
            print("verdict: this device can mirror and be controlled.")
        elif rsd.DISPLAY_SVC in missing:
            verdict = f"NO MIRRORING — displayservice absent on iOS {identity.get('ProductVersion')}"
            print(f"verdict: NO MIRRORING on iOS {identity.get('ProductVersion')} — "
                  f"displayservice is not advertised.")
            print("         Touch injection also depends on it: the device only routes touch to")
            print("         UIKit while a media stream is running, so without displayservice")
            print("         there is no input either. Screenshots may still work.")
        else:
            verdict = f"partial — missing {', '.join(missing)}"
            print(f"verdict: partial — missing {', '.join(missing)}")

        report = write_report(f"rsd-probe-{udid}.json", {
            "udid": udid,
            "connection": dev["connection"],
            "identity": identity,
            "tunnel": {"device": link.params.dev_addr, "us": link.params.our_addr,
                       "rsd_port": link.params.rsd_port, "mtu": link.params.mtu},
            "service_count": len(catalog),
            "needed": {name: catalog.port(name) for name in labels},
            "missing_core": missing,
            "verdict": verdict,
            "properties": catalog.properties,
            "services": {name: catalog.port(name) for name in catalog.names},
        })
        print(f"\nreport written: {report}")

        if show_all:
            print(f"\nall {len(catalog)} services:")
            for name in catalog.names:
                print(f"  {catalog.port(name):>6}  {name}")
        else:
            hits = [n for n in catalog.names
                    if any(k in n.lower() for k in INTERESTING) and n not in labels]
            if hits:
                print(f"\nother screen/display/HID-ish services ({len(hits)}):")
                for name in hits:
                    print(f"  {catalog.port(name):>6}  {name}")
            print("\n(pass --all for the full catalog)")
        return 0
    finally:
        pump.stop()
        link.close()
        tun.close()


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
