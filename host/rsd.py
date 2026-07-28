#!/usr/bin/env python3
"""Layer 3 — RSD service enumeration (thin wrapper over remotexpc.py).

Runs against [serverAddress]:serverRSDPort through the tunnel (needs tunnel_up.py's route).
"""
import sys

from remotexpc import enumerate_services

SCREEN_SVC = "com.apple.coredevice.screencaptureservice"
HID_SVC = "com.apple.coredevice.hid.universalhidservice"


def connect(host, port):
    return enumerate_services(host, port)


def service_port(peer_info, name):
    svc = peer_info.get("Services", {}).get(name)
    return svc.get("Port") if svc else None


def print_services(peer_info):
    props = peer_info.get("Properties", {})
    print(f"device: {props.get('ProductType')} iOS {props.get('OSVersion')} "
          f"build {props.get('BuildVersion')} udid {props.get('UniqueDeviceID')}")
    services = peer_info.get("Services", {})
    print(f"\n{len(services)} RSD services:")
    for name in sorted(services):
        star = "  <<<" if name in (SCREEN_SVC, HID_SVC) else ""
        print(f"  {services[name].get('Port'):>6}  {name}{star}")
    print()
    for tag, svc in (("SCREEN", SCREEN_SVC), ("HID/control", HID_SVC)):
        p = service_port(peer_info, svc)
        print(f"{tag:12}: {'port ' + str(p) if p else 'NOT ADVERTISED'}  ({svc})")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit("usage: python3 rsd.py <serverAddress> <serverRSDPort>   (tunnel must be up)")
    print_services(connect(sys.argv[1], int(sys.argv[2])))
