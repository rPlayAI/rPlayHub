"""Transport C — relay a tunnel to a phone on another network. NOT BUILT YET (plan only).

This is the "remote iPhone" path. The insight that makes it cheap: the CoreDevice tunnel is
just a stream of raw IPv6 packets, so a relay needs exactly one splice point and everything
above it (RSD, screen, HID, and Xcode's own traffic) rides across untouched.

    ┌─ phone side ──────────────┐            ┌─ developer side ─────────────┐
    │ rplayhub agent            │            │ rplayhub server              │
    │  UsbmuxTransport (or      │◀── TCP ───▶│  RelayTransport               │
    │  RemotePairing) → link    │   (TLS)    │   → TunnelLink → utun → RSD  │
    └───────────────────────────┘            └──────────────────────────────┘

Wire protocol to implement (deliberately trivial — the hard parts are already solved above
and below it):
    hello    client → agent: {"udid": ..., "mtu": ...}            (JSON, u32-length-prefixed)
    accept   agent → client: the TunnelParams dict as JSON        (u32-length-prefixed)
    then     both directions: u32 length + one raw IPv6 packet, forever

Open design points, in the order they need deciding:
  * Auth/encryption: this carries full device control, so it must not be a bare TCP socket.
    TLS with a pre-shared client cert is the least new machinery.
  * The IPv6 addresses in TunnelParams are chosen by the *phone-side* handshake, so two
    relayed devices can collide on the developer's host. The relay may need to rewrite
    addresses, or each device gets its own utun and routing table entry (simpler; what the
    registry already does).
  * MTU: relaying over the internet under the tunnel's 16000-byte MTU means the transport
    must not fragment mid-packet. The length prefix above handles that.

## Making a relayed phone visible to Xcode — the part that is NOT just a transport

Getting our own mirroring/control working over the relay is the easy half: our stack sits on
top of TunnelLink already. Xcode is the hard half, because Xcode does not talk to us — it
talks to Apple's own daemons on the developer's Mac. So the relayed device has to be injected
into Apple's discovery, not ours. Two candidate mechanisms, NEITHER YET VERIFIED:

  1. Wifi-sync style: usbmuxd discovers network devices over mDNS (`_apple-mobdev2._tcp`).
     Advertising that service locally for the remote UDID and proxying lockdown may make
     Apple's usbmuxd list it as ConnectionType=Network, which classic Xcode paths follow.
  2. CoreDevice style (Xcode 15+): Xcode discovers wireless devices through the
     RemotePairing/tunnel machinery. Advertising `_remotepairing._tcp` locally and relaying
     it to the remote phone would put the device in front of Xcode's own stack.

Both need a live experiment before any of it is designed further; (2) is the better bet on
current Xcode and shares all its wire work with remotepairing.py, which is why that one is
worth building first. Until one is confirmed, "remote iPhone works with Xcode" is an
unvalidated goal, not a feature.
"""
from . import Transport, TunnelLink


class RelayTransport(Transport):
    name = "relay"

    def __init__(self, endpoint: str, udid: str | None = None):
        self.endpoint = endpoint
        self._udid = udid

    @property
    def udid(self):
        return self._udid

    def open(self) -> TunnelLink:
        raise NotImplementedError(
            "Relay transport is not built yet — see this module's docstring for the wire "
            "protocol and the open design points"
        )
