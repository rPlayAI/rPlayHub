"""Transport A — through Apple's usbmuxd. Works over USB and over the Mac-tethered wifi path.

This is the path that is built and verified live. usbmuxd does discovery and carriage; we are
just another libusbmux client. The tunnel rides inside the relayed stream.

Limitation worth remembering: this needs Apple's usbmuxd, so it does not port off macOS, and
the device must be paired with *this* Mac. The wifi (`ConnectionType=Network`) entry is
intermittent because iOS sleeps it — hence wait_available().
"""
import time

from . import Transport, TunnelLink, coredeviceproxy, lockdown, usbmux
from ..errors import HubError, TransportError

# adb-style device record, normalized so the registry does not care which transport found it.
def list_devices(socket_path=usbmux.USBMUXD_SOCKET) -> list[dict]:
    """Every device usbmuxd currently knows about."""
    out = []
    for d in usbmux.list_devices(socket_path):
        out.append({
            "udid": d.get("SerialNumber"),
            "device_id": d.get("DeviceID"),
            "connection": d.get("ConnectionType"),   # "USB" | "Network"
            "product_id": d.get("ProductID"),
            "transport": "usbmux",
        })
    return out


def probe_device(udid: str, socket_path: str = usbmux.USBMUXD_SOCKET) -> dict:
    """Lockdown identity read.

    A session is required: on a Network (wifi) connection iOS answers plain GetValue with
    `GetProhibited`, so the pair record and TLS session have to come first. Individual keys
    are still read defensively, because which values are readable varies by iOS version.
    """
    lc = lockdown.LockdownClient(udid)
    try:
        try:
            lc.start_session(usbmux.read_pair_record(udid, socket_path))
        except HubError:
            pass                    # fall through and report whatever is readable unsessioned
        out = {}
        for k in ("DeviceName", "ProductType", "ProductVersion", "BuildVersion"):
            try:
                out[k] = lc.get_value(k)
            except HubError as e:
                out[k] = f"<{e}>"
        return out
    finally:
        lc.close()


class UsbmuxTransport(Transport):
    name = "usbmux"

    def __init__(self, udid: str | None = None, socket_path: str = usbmux.USBMUXD_SOCKET,
                 mtu: int = coredeviceproxy.MTU):
        self._udid = udid
        self.socket_path = socket_path
        self.mtu = mtu

    @property
    def udid(self):
        return self._udid

    def resolve_udid(self) -> str:
        """Pin this transport to a concrete device (first attached, if none was given)."""
        if self._udid:
            return self._udid
        devs = usbmux.list_devices(self.socket_path)
        if not devs:
            raise TransportError("no devices attached to usbmuxd")
        self._udid = devs[0]["SerialNumber"]
        return self._udid

    def wait_available(self, timeout: float = 15.0) -> bool:
        """Poll usbmuxd until our device shows up. Wifi entries come and go."""
        if not self._udid:
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                if usbmux.list_devices(self.socket_path):
                    return True
                time.sleep(1.0)
            return False
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if any(d.get("SerialNumber") == self._udid
                   for d in usbmux.list_devices(self.socket_path)):
                return True
            time.sleep(1.0)
        return False

    def open(self) -> TunnelLink:
        udid = self.resolve_udid()
        try:
            pair_record = usbmux.read_pair_record(udid, self.socket_path)
            lc = lockdown.LockdownClient(udid)
            try:
                lc.start_session(pair_record)
                svc = lc.start_service(coredeviceproxy.SERVICE)
            finally:
                lc.close()
            sock = lockdown.connect_service(udid, svc["Port"], pair_record,
                                            svc["EnableServiceSSL"])
        except HubError as e:
            raise TransportError(f"usbmux tunnel setup failed for {udid}: {e}") from e
        except OSError as e:
            raise TransportError(f"usbmux tunnel setup failed for {udid}: {e!r}") from e

        try:
            params, _raw = coredeviceproxy.handshake(sock, self.mtu)
        except Exception as e:
            sock.close()
            raise TransportError(f"CDTunnel handshake failed for {udid}: {e!r}") from e

        if not (params.our_addr and params.dev_addr and params.rsd_port):
            sock.close()
            raise TransportError(f"incomplete tunnel parameters for {udid}: {params}")
        return TunnelLink(sock, params, transport_name=self.name)
