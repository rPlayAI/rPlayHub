"""The tun device — a real kernel interface so ordinary sockets reach the device.

Once the tunnel's IPv6 addresses are on an interface and a route points at it, RSD and every
coredevice.* service is reachable with plain socket.create_connection(). That is what buys us
a normal-looking stack above this line.

Two implementations behind one interface:
  MacUtun    — AF_SYSTEM / com.apple.net.utun_control. Verified live.
  LinuxTun   — /dev/net/tun. Written from the documented ABI, NOT yet run against a device.

The platform difference the rest of the code must not see: macOS utun prefixes every packet
with a 4-byte address family, Linux with IFF_NO_PI does not. recv_packet()/send_packet()
normalize that away, so the pump is platform-free.

Both need root (creating an interface and adding routes is privileged).
"""
import abc
import fcntl
import os
import platform
import select
import socket
import struct
import subprocess

IPV6_HDR_LEN = 40


class TunDevice(abc.ABC):
    """One point-to-point IPv6 interface carrying one device's tunnel."""

    name: str = "?"

    @abc.abstractmethod
    def configure(self, params) -> None:
        """Assign our address and route the device address at this interface."""

    @abc.abstractmethod
    def recv_packet(self, timeout: float = 0.5) -> bytes | None:
        """One IPv6 packet from the host stack, or None on timeout."""

    @abc.abstractmethod
    def send_packet(self, packet: bytes) -> None:
        """Hand one IPv6 packet to the host stack."""

    @abc.abstractmethod
    def close(self) -> None:
        """Tear the interface down."""

    def __repr__(self):
        return f"<{type(self).__name__} {self.name}>"


# --------------------------------------------------------------------------- macOS
CTLIOCGINFO = 0xC0644E03            # _IOWR('N', 3, struct ctl_info)
UTUN_CONTROL_NAME = b"com.apple.net.utun_control"
UTUN_OPT_IFNAME = 2
_AF_INET6_PREFIX = struct.pack(">I", socket.AF_INET6)


class MacUtun(TunDevice):
    def __init__(self):
        s = socket.socket(socket.AF_SYSTEM, socket.SOCK_DGRAM, socket.SYSPROTO_CONTROL)
        info = struct.pack("I96s", 0, UTUN_CONTROL_NAME)
        info = fcntl.ioctl(s, CTLIOCGINFO, info)
        ctl_id = struct.unpack("I96s", info)[0]
        s.connect((ctl_id, 0))          # unit 0 => kernel picks a free utun number
        ifname = s.getsockopt(socket.SYSPROTO_CONTROL, UTUN_OPT_IFNAME, 256)
        self.name = ifname.split(b"\x00", 1)[0].decode()
        self._sock = s
        self._closed = False

    def configure(self, params):
        prefixlen = 64
        subprocess.run(["ifconfig", self.name, "inet6", params.our_addr,
                        "prefixlen", str(prefixlen)], check=True)
        subprocess.run(["ifconfig", self.name, "up"], check=True)
        subprocess.run(["route", "add", "-inet6", params.dev_addr, "-interface", self.name],
                       check=False, capture_output=True)

    def recv_packet(self, timeout=0.5):
        self._sock.settimeout(timeout)
        try:
            pkt = self._sock.recv(65536)
        except (socket.timeout, TimeoutError):
            return None
        except OSError:
            raise
        return pkt[4:] if len(pkt) > 4 else None

    def send_packet(self, packet):
        self._sock.send(_AF_INET6_PREFIX + packet)

    def close(self):
        if self._closed:
            return
        self._closed = True
        try:
            self._sock.close()
        except OSError:
            pass
        # The interface disappears with the control socket, but ask anyway: a stale utun with
        # our address still on it would break the next session's routing.
        subprocess.run(["ifconfig", self.name, "destroy"], check=False, capture_output=True)


# --------------------------------------------------------------------------- Linux
TUNSETIFF = 0x400454CA
IFF_TUN = 0x0001
IFF_NO_PI = 0x1000


class LinuxTun(TunDevice):
    """UNVERIFIED — written from the documented /dev/net/tun ABI, never run against a phone.

    Expect to debug this when the first Linux target appears. The parts most likely to be
    wrong are the iproute2 invocations, not the ioctl.
    """

    def __init__(self, name_hint="rplay%d"):
        self._fd = os.open("/dev/net/tun", os.O_RDWR)
        ifr = struct.pack("16sH", name_hint.encode(), IFF_TUN | IFF_NO_PI)
        res = fcntl.ioctl(self._fd, TUNSETIFF, ifr)
        self.name = struct.unpack("16sH", res)[0].split(b"\x00", 1)[0].decode()
        self._closed = False

    def configure(self, params):
        subprocess.run(["ip", "-6", "addr", "add", f"{params.our_addr}/64",
                        "dev", self.name], check=True)
        subprocess.run(["ip", "link", "set", "dev", self.name, "up",
                        "mtu", str(params.mtu)], check=True)
        subprocess.run(["ip", "-6", "route", "add", f"{params.dev_addr}/128",
                        "dev", self.name], check=False, capture_output=True)

    def recv_packet(self, timeout=0.5):
        r, _, _ = select.select([self._fd], [], [], timeout)
        if not r:
            return None
        pkt = os.read(self._fd, 65536)      # IFF_NO_PI: no prefix
        return pkt or None

    def send_packet(self, packet):
        os.write(self._fd, packet)

    def close(self):
        if self._closed:
            return
        self._closed = True
        try:
            os.close(self._fd)
        except OSError:
            pass


def open_tun() -> TunDevice:
    """Create a tun device for this platform. Needs root."""
    if os.geteuid() != 0:
        raise PermissionError("creating a tunnel interface needs root (re-run under sudo)")
    system = platform.system()
    if system == "Darwin":
        return MacUtun()
    if system == "Linux":
        return LinuxTun()
    raise NotImplementedError(f"no tun implementation for {system}")
