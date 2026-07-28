"""Transport B — direct RemotePairing door. NOT BUILT YET (this file is the plan, not code).

Why it matters: it is the only path that reaches an iPhone over wifi with no Apple daemon in
the runtime path — the `adb pair` / `adb connect` analogue for iOS. It is also the piece the
remote-Xcode story needs on the far end.

What is already proven (previous sessions):
  * the phone advertises `_remotepairing._tcp` and `_rp-tunnel._tcp` on the LAN via mDNS,
  * plain TCP connect to those ports works — no usbmuxd anywhere,
  * there is NO hardware attestation in CoreDevice/RemotePairing, so a non-Apple host can
    complete a real pairing.

What is missing is the handshake itself, mapped in ../../../doc/REMOTEPAIRING-PROTOCOL.md:
  1. discovery      — mDNS browse `_remotepairing._tcp` / `_rp-tunnel._tcp`
  2. control channel — RPPairingPacket framing, OPACK-encoded messages
  3. pair-setup     — SRP (first time; shows the on-device pairing prompt)
  4. pair-verify    — Curve25519/X25519 (every subsequent connect)
  5. encrypted tunnel — ChaCha20-Poly1305 stream, then CDTunnel-equivalent bringup

Crypto: use `cryptography` (already the project's only non-stdlib dependency) for X25519,
ChaCha20-Poly1305, HKDF and SHA-512; SRP-6a needs hand-rolling on top of `int` arithmetic.
For the C port, OpenSSL covers X25519 / ChaCha20-Poly1305 / digests.

Cross-check the wire details against pymobiledevice3's RemotePairing implementation
(`RPPairingPacket`, `_pair`, `_attempt_pair_verify`, `remote start-tunnel --userspace`) — used
strictly as an offline oracle, never as a runtime dependency.

When implemented, this class returns the same TunnelLink as UsbmuxTransport and nothing above
the transport layer changes.
"""
from . import Transport, TunnelLink


class RemotePairingTransport(Transport):
    name = "remotepairing"

    def __init__(self, address: str | None = None, port: int | None = None,
                 udid: str | None = None, pairing_store=None):
        self.address = address
        self.port = port
        self._udid = udid
        self.pairing_store = pairing_store

    @property
    def udid(self):
        return self._udid

    def open(self) -> TunnelLink:
        raise NotImplementedError(
            "RemotePairing transport is not built yet — see this module's docstring and "
            "doc/REMOTEPAIRING-PROTOCOL.md for the implementation plan"
        )


def discover(timeout: float = 3.0) -> list[dict]:
    """Browse for phones offering the RemotePairing door. NOT BUILT.

    Plan: mDNS browse `_remotepairing._tcp` + `_rp-tunnel._tcp`, returning
    [{"name","address","port","service"}]. stdlib has no mDNS client, so this needs either a
    ~200-line mDNS query/response of our own (preferred — keeps the dependency budget) or
    `dns-sd -B` / `avahi-browse` shelled out per platform.
    """
    raise NotImplementedError("RemotePairing discovery is not built yet")
