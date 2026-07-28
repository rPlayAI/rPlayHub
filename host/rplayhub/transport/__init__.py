"""The bottom seam: how tunnel bytes reach the phone.

Everything above this package (RSD, RemoteXPC, screen, HID) is transport-agnostic. It only
ever needs a byte stream that carries raw IPv6 packets plus the addresses to route into it.
That is exactly what a Transport hands back: a TunnelLink.

Implementations
--------------
  usbmux_transport.UsbmuxTransport   Apple's usbmuxd does discovery + carriage (USB or the
                                     Mac-tethered wifi path). Built and verified live.
  remotepairing.RemotePairingTransport   Direct TCP to the phone over wifi, our own pairing
                                     handshake, no Apple daemon in the path. NOT BUILT.
  relay.RelayTransport               Tunnel bytes spliced to a phone on another network.
                                     NOT BUILT.

Adding one is a new file here and nothing else: as long as open() returns a TunnelLink, the
whole stack above works unchanged. That is the entire reason this seam exists.
"""
import abc
from dataclasses import dataclass


@dataclass(frozen=True)
class TunnelParams:
    """What the tunnel handshake told us about the routing it expects.

    our_addr / dev_addr are the IPv6 endpoints of the tunnel; rsd_port is where the
    device's Remote Service Discovery listens once packets flow.
    """
    our_addr: str
    dev_addr: str
    rsd_port: int
    mtu: int
    netmask: str | None = None

    def __str__(self):
        return f"us={self.our_addr} device={self.dev_addr} rsd={self.rsd_port} mtu={self.mtu}"


class TunnelLink:
    """A live tunnel: the byte stream plus its parameters.

    `stream` is a socket carrying RAW IPv6 packets in both directions (no framing of its
    own beyond the packets themselves). `close()` releases whatever the transport had to
    hold open to keep it alive.
    """

    def __init__(self, stream, params: TunnelParams, on_close=None, transport_name="?"):
        self.stream = stream
        self.params = params
        self.transport_name = transport_name
        self._on_close = on_close
        self.closed = False

    def close(self):
        if self.closed:
            return
        self.closed = True
        try:
            self.stream.close()
        except OSError:
            pass
        if self._on_close:
            try:
                self._on_close()
            except Exception:
                pass

    def __repr__(self):
        return f"<TunnelLink {self.transport_name} {self.params}>"


class Transport(abc.ABC):
    """A way to get a TunnelLink to one device."""

    name = "transport"

    @property
    @abc.abstractmethod
    def udid(self) -> str | None:
        """Device this transport is bound to, once known."""

    @abc.abstractmethod
    def open(self) -> TunnelLink:
        """Bring a tunnel up. Raises TransportError on failure."""

    def wait_available(self, timeout: float = 15.0) -> bool:
        """Block until the device is reachable by this transport. Default: assume it is.

        Exists because the wifi path is intermittent — usbmuxd's Network entry comes and
        goes as iOS sleeps the connection, so the supervisor waits rather than failing.
        """
        return True

    def describe(self) -> str:
        return f"{self.name}({self.udid or 'auto'})"
