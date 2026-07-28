"""RSD — Remote Service Discovery: what the device offers and on which ports.

One RemoteXPC handshake to the tunnel's rsd_port returns peer_info: device Properties plus a
Services map of ~85 entries. Every coredevice.* feature we use is a port in that map, so this
runs once per session bringup and the result is cached in the ServiceCatalog.
"""
import uuid

from .errors import ServiceMissing
from .wire import xpc
from .wire.remotexpc import RemoteXPC

MESSAGING_PROTOCOL_VERSION = 7
REMOTE_XPC_VERSION_FLAGS = 0x0100000000000006

SCREENSHOT_SVC = "com.apple.coredevice.screencaptureservice"
DISPLAY_SVC = "com.apple.coredevice.displayservice"
HID_SVC = "com.apple.coredevice.hid.universalhidservice"

# The three the mirroring/control product actually depends on.
CORE_SERVICES = (SCREENSHOT_SVC, DISPLAY_SVC, HID_SVC)


def handshake(host: str, port: int, timeout: float = 8.0) -> dict:
    """Connect to RSD through the tunnel and return raw peer_info."""
    x = RemoteXPC(host, port, timeout=timeout)
    try:
        x.send_request({
            "MessageType": "Handshake",
            "MessagingProtocolVersion": xpc.U64(MESSAGING_PROTOCOL_VERSION),
            "UUID": uuid.uuid4(),
            "Properties": {"RemoteXPCVersionFlags": xpc.U64(REMOTE_XPC_VERSION_FLAGS),
                           "SensitivePropertiesVisible": True},
            "Services": {},
        })
        while True:
            obj = x.receive()
            if isinstance(obj, dict) and ("Services" in obj or "Properties" in obj):
                return obj
    finally:
        x.close()


class ServiceCatalog:
    """Parsed, cached peer_info. Cheap to query; re-created on every reconnect."""

    def __init__(self, peer_info: dict):
        self.raw = peer_info
        self.properties = peer_info.get("Properties", {}) or {}
        self._services = peer_info.get("Services", {}) or {}

    @classmethod
    def fetch(cls, host, port, timeout=8.0):
        return cls(handshake(host, port, timeout=timeout))

    # --- queries ---
    def port(self, name: str) -> int | None:
        svc = self._services.get(name)
        return svc.get("Port") if svc else None

    def require(self, name: str) -> int:
        p = self.port(name)
        if p is None:
            raise ServiceMissing(f"device does not advertise {name}")
        return int(p)

    def has(self, name: str) -> bool:
        return self.port(name) is not None

    @property
    def names(self) -> list[str]:
        return sorted(self._services)

    def __len__(self):
        return len(self._services)

    # --- identity ---
    @property
    def udid(self):
        return self.properties.get("UniqueDeviceID")

    def device_info(self) -> dict:
        p = self.properties
        return {
            "udid": p.get("UniqueDeviceID"),
            "name": p.get("Name") or p.get("DeviceName"),
            "product_type": p.get("ProductType"),
            "os_version": p.get("OSVersion"),
            "build": p.get("BuildVersion"),
            "services": len(self._services),
        }

    def summary(self) -> str:
        i = self.device_info()
        return (f"{i['product_type']} iOS {i['os_version']} build {i['build']} "
                f"udid {i['udid']} — {i['services']} services")

    def missing_core(self) -> list[str]:
        """Which of the services mirroring/control needs are absent."""
        return [n for n in CORE_SERVICES if not self.has(n)]
