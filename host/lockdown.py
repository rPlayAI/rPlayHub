#!/usr/bin/env python3
"""Layer 1 — lockdown client (device port 62078), dependency-light.

Framing on the lockdown service pipe: <u32 be length><XML plist>.
Basic GetValue / QueryType work WITHOUT an SSL session (no pair record needed) — enough to
read ProductVersion, UniqueChipID, the RSD/CoreDevice hints, and to StartService.

Verified against a real device via probe.py.
"""
import os, plistlib, socket, ssl, struct, tempfile

import usbmux

LOCKDOWN_PORT = 62078


class LockdownError(Exception):
    pass


def make_ssl_context(pair_record: dict) -> ssl.SSLContext:
    """Client SSL context using the host identity from the pair record (for lockdown
    sessions and EnableServiceSSL services). Returns (ctx, pem_path); caller unlinks pem_path."""
    cert = pair_record["HostCertificate"]
    key = pair_record["HostPrivateKey"]
    cert = cert if isinstance(cert, bytes) else cert.encode()
    key = key if isinstance(key, bytes) else key.encode()
    tf = tempfile.NamedTemporaryFile("wb", suffix=".pem", delete=False)
    tf.write(cert + b"\n" + key); tf.close()
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    ctx.minimum_version = ssl.TLSVersion.TLSv1
    try:
        ctx.set_ciphers("ALL:@SECLEVEL=0")
    except ssl.SSLError:
        pass
    ctx.load_cert_chain(tf.name)
    return ctx, tf.name


def connect_service(udid: str, port: int, pair_record: dict | None, use_ssl: bool) -> socket.socket:
    """Open a raw (optionally SSL) socket to a device service port."""
    sock = usbmux.connect(udid, port)
    if use_ssl:
        if pair_record is None:
            pair_record = usbmux.read_pair_record(udid)
        ctx, pem = make_ssl_context(pair_record)
        try:
            sock = ctx.wrap_socket(sock, server_hostname=None)
        finally:
            os.unlink(pem)
    return sock


class LockdownClient:
    def __init__(self, udid: str | None = None):
        self._sock = usbmux.connect(udid, LOCKDOWN_PORT)
        self.udid = udid
        self.session_id = None
        self._ssl_started = False
        t = self._request({"Request": "QueryType"})
        if t.get("Type") != "com.apple.mobile.lockdown":
            raise LockdownError(f"unexpected lockdown type: {t}")

    # --- framed plist over the device pipe ---
    def _send(self, payload: dict):
        body = plistlib.dumps(payload)
        self._sock.sendall(struct.pack(">I", len(body)) + body)

    def _recv(self) -> dict:
        (n,) = struct.unpack(">I", self._recvn(4))
        return plistlib.loads(self._recvn(n))

    def _recvn(self, n: int) -> bytes:
        buf = b""
        while len(buf) < n:
            chunk = self._sock.recv(n - len(buf))
            if not chunk:
                raise LockdownError("device closed lockdown connection")
            buf += chunk
        return buf

    def _request(self, payload: dict) -> dict:
        self._send(payload)
        r = self._recv()
        if r.get("Error"):
            raise LockdownError(f"lockdown error: {r['Error']} (req={payload.get('Request')})")
        return r

    # --- API ---
    def get_value(self, key: str | None = None, domain: str | None = None):
        req = {"Request": "GetValue"}
        if key is not None:
            req["Key"] = key
        if domain is not None:
            req["Domain"] = domain
        return self._request(req).get("Value")

    def all_values(self) -> dict:
        return self.get_value() or {}

    def start_session(self, pair_record: dict | None = None) -> dict:
        """StartSession with the stored pair record; upgrade the pipe to TLS if asked.
        Required before StartService of most services (fixes SessionInactive)."""
        if pair_record is None:
            pair_record = usbmux.read_pair_record(self.udid or self.get_value("UniqueDeviceID"))
        r = self._request({"Request": "StartSession",
                           "HostID": pair_record["HostID"], "SystemBUID": pair_record["SystemBUID"]})
        self.session_id = r.get("SessionID")
        if r.get("EnableSessionSSL"):
            self._upgrade_to_ssl(pair_record)
        return {"SessionID": self.session_id, "ssl": self._ssl_started}

    def _upgrade_to_ssl(self, pair_record: dict):
        cert = pair_record["HostCertificate"]
        key = pair_record["HostPrivateKey"]
        cert = cert if isinstance(cert, bytes) else cert.encode()
        key = key if isinstance(key, bytes) else key.encode()
        with tempfile.NamedTemporaryFile("wb", suffix=".pem", delete=False) as f:
            f.write(cert + b"\n" + key)
            pem_path = f.name
        try:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            ctx.minimum_version = ssl.TLSVersion.TLSv1  # lockdown speaks old TLS
            try:
                ctx.set_ciphers("ALL:@SECLEVEL=0")
            except ssl.SSLError:
                pass
            ctx.load_cert_chain(pem_path)
            self._sock = ctx.wrap_socket(self._sock, server_hostname=None,
                                         do_handshake_on_connect=True)
            self._ssl_started = True
        finally:
            os.unlink(pem_path)

    def start_service(self, name: str) -> dict:
        """Ask lockdown to spin up a service; returns {Port, EnableServiceSSL?}."""
        r = self._request({"Request": "StartService", "Service": name})
        return {"Port": r.get("Port"), "EnableServiceSSL": r.get("EnableServiceSSL", False),
                "ServiceName": name, "raw": r}

    def close(self):
        try:
            self._sock.close()
        except OSError:
            pass


if __name__ == "__main__":
    import sys
    lc = LockdownClient(sys.argv[1] if len(sys.argv) > 1 else None)
    for k in ("DeviceName", "ProductType", "ProductVersion", "BuildVersion",
              "UniqueChipID", "UniqueDeviceID", "WiFiAddress", "CPUArchitecture"):
        try:
            print(f"{k:16} = {lc.get_value(k)}")
        except LockdownError as e:
            print(f"{k:16} ! {e}")
    lc.close()
