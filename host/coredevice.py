#!/usr/bin/env python3
"""CoreDevice service invocation — the RPC envelope every coredevice.* feature uses.

    invoke(feature, input) -> output
over a RemoteXPC channel to the service's RSD port (through the tunnel). Envelope verified
against pmd3. Includes screenshot capture (screencaptureservice) as the first feature.
"""
import uuid

import xpc
from remotexpc import RemoteXPC

CORE_DEVICE_VERSION_STR = "629.3"


def _version_dict(v: str):
    parts = v.split(".")
    return {"components": [xpc.U64(int(p)) for p in parts],
            "originalComponentsCount": len(parts),   # plain int -> INT64
            "stringValue": v}


class CoreDeviceService:
    def __init__(self, host, port):
        self.x = RemoteXPC(host, port)

    def invoke(self, feature_identifier=None, input_=None, action_identifier=None):
        req = {
            "CoreDevice.CoreDeviceDDIProtocolVersion": 2,          # INT64
            "CoreDevice.coreDeviceVersion": _version_dict(CORE_DEVICE_VERSION_STR),
            "CoreDevice.deviceIdentifier": str(uuid.uuid4()),
            "CoreDevice.input": input_ or {},
            "CoreDevice.invocationIdentifier": str(uuid.uuid4()),
        }
        if feature_identifier is not None:
            req["CoreDevice.featureIdentifier"] = feature_identifier
            req["CoreDevice.action"] = {}
        if action_identifier is not None:
            req["CoreDevice.actionIdentifier"] = action_identifier
        resp = self.x.send_receive(req)
        out = resp.get("CoreDevice.output")
        if out is None:
            raise RuntimeError(f"invoke {feature_identifier} failed: {resp}")
        return out

    def close(self):
        self.x.close()


def screenshot(host, port, out_path, fmt="png"):
    """Capture a screenshot via screencaptureservice; write image bytes to out_path."""
    svc = CoreDeviceService(host, port)
    out = svc.invoke(
        "com.apple.coredevice.feature.capturescreenshot",
        {"displayUniqueID": None, "requestedFormat": fmt},
        action_identifier="com.apple.coredevice.action.capturescreenshot",
    )
    svc.close()
    img = out.get("image")
    if not img:
        raise RuntimeError(f"no image in output: {list(out)}")
    with open(out_path, "wb") as f:
        f.write(img)
    return len(img), out.get("imageFormat")


if __name__ == "__main__":
    import sys
    if len(sys.argv) < 4:
        sys.exit("usage: python3 coredevice.py <device_addr> <screencapture_port> <out.png>  (tunnel up)")
    n, fmt = screenshot(sys.argv[1], int(sys.argv[2]), sys.argv[3])
    print(f"saved {n} bytes ({fmt}) -> {sys.argv[3]}")
