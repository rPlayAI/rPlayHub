#!/usr/bin/env python3
"""coredevice.appservice over the tunnel — list apps / processes, launch, terminate.

Same CoreDevice envelope as screenshots (host/coredevice.py), different feature identifiers.
Usage (daemon running):
    python3 host/appservice.py apps | procs | launch <bundle-id> | kill <pid>
"""
import json
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import rsd
from coredevice import CoreDeviceService
from diagnostics_relay import rpc

SERVICE = "com.apple.coredevice.appservice"


def open_service():
    info = rpc("tunnel_info").get("result", {})
    addr, rsd_port = info.get("device_addr"), info.get("rsd_port")
    if not addr or not rsd_port:
        sys.exit(f"daemon has no live tunnel: {info}")
    port = rsd.service_port(rsd.connect(addr, rsd_port), SERVICE)
    if not port:
        sys.exit(f"{SERVICE} not advertised")
    return CoreDeviceService(addr, port)


def list_apps(svc):
    return svc.invoke("com.apple.coredevice.feature.listapps", {
        "includeAppClips": False, "includeRemovableApps": True,
        "includeHiddenApps": False, "includeInternalApps": False,
        "includeDefaultApps": True, "requireContainerAccess": False})


def list_processes(svc):
    return svc.invoke("com.apple.coredevice.feature.listprocesses", {})


def launch(svc, bundle_id):
    return svc.invoke("com.apple.coredevice.feature.launchapplication", {
        "applicationSpecifier": {"bundleIdentifier": {"_0": bundle_id}},
        "options": {
            "arguments": [], "environmentVariables": {}, "standardIOUsesPseudoterminals": True,
            "startStopped": False, "terminateExisting": True, "user": {"active": True},
            "platformSpecificOptions": b"bplist00\xd0\x08\x00\x00\x00\x00\x00\x00\x01\x01\x00\x00"
                                       b"\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00"
                                       b"\x00\x00\x00\x00\x00\x00\x00\x09",
        },
        "standardIOIdentifiers": {},
    })


def kill(svc, pid, signal=9):
    return svc.invoke("com.apple.coredevice.feature.sendsignaltoprocess", {
        "process": {"processIdentifier": int(pid)}, "signal": signal})


if __name__ == "__main__":
    a = sys.argv[1:]
    svc = open_service()
    try:
        if a[:1] == ["apps"]:    out = list_apps(svc)
        elif a[:1] == ["procs"]: out = list_processes(svc)
        elif a[:1] == ["launch"]: out = launch(svc, a[1])
        elif a[:1] == ["kill"]:  out = kill(svc, a[1])
        else: sys.exit(__doc__)
        print(json.dumps(out, indent=1, default=str)[:6000])
    finally:
        svc.close()
