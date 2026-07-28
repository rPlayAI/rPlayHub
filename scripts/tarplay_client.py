"""
Thin Python client for the tarplayd JSON-line protocol on 127.0.0.1:9876.

Each public method opens a fresh TCP connection, sends one JSON request,
reads one JSON response, and closes. Stateless and easy to reason about
for a Phase-1 SDK; we'll add a persistent-connection variant once the
WebSocket transport lands.

Usage:
    from tarplay_client import TarplayClient
    c = TarplayClient()
    print(c.list_devices())
    c.tap(500, 1200)
    img_bytes = c.screenshot()        # raw JPEG bytes
"""

from __future__ import annotations

import base64
import json
import socket
import uuid
from typing import Any


class TarplayError(RuntimeError):
    """Daemon returned ok=false. .code is the machine-readable error code."""

    def __init__(self, code: str, message: str):
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message


class TarplayClient:
    def __init__(self, host: str = "127.0.0.1", port: int = 9876,
                 timeout_s: float = 10.0):
        self.host = host
        self.port = port
        self.timeout_s = timeout_s

    # --- low-level ------------------------------------------------------

    def _request(self, method: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        req = {"id": str(uuid.uuid4()), "method": method,
               "params": params or {}}
        line = (json.dumps(req) + "\n").encode("utf-8")

        with socket.create_connection((self.host, self.port),
                                      timeout=self.timeout_s) as s:
            s.sendall(line)
            buf = bytearray()
            while True:
                chunk = s.recv(65536)
                if not chunk:
                    raise TarplayError("connection_closed",
                                       "daemon closed without response")
                buf.extend(chunk)
                if 0x0A in buf:
                    break

        line, _, _ = bytes(buf).partition(b"\n")
        resp = json.loads(line.decode("utf-8"))
        if not resp.get("ok"):
            err = resp.get("error", {}) or {}
            raise TarplayError(err.get("code", "unknown"),
                               err.get("message", "(no message)"))
        return resp.get("result", {}) or {}

    # --- API verbs ------------------------------------------------------

    def ping(self) -> dict[str, Any]:
        return self._request("ping")

    def list_devices(self) -> list[dict[str, Any]]:
        return self._request("list_devices").get("devices", [])

    def screenshot(self, fmt: str = "jpeg", quality: int = 85) -> bytes:
        """Returns raw image bytes (JPEG by default)."""
        result = self._request("take_screenshot",
                               {"format": fmt, "quality": quality})
        return base64.b64decode(result["image_b64"])

    def screenshot_dict(self, fmt: str = "jpeg", quality: int = 85) -> dict[str, Any]:
        """Returns the full result dict (width, height, format, image_b64)."""
        return self._request("take_screenshot",
                             {"format": fmt, "quality": quality})

    def tap(self, x: int, y: int, duration_ms: int = 50) -> dict[str, Any]:
        return self._request("tap", {"x": x, "y": y, "duration_ms": duration_ms})

    # --- helpers --------------------------------------------------------

    def first_device(self) -> dict[str, Any]:
        """Convenience: returns the first connected device, raises if none."""
        devices = self.list_devices()
        if not devices:
            raise TarplayError("device_not_found",
                               "no iPhone connected — start mirroring first")
        return devices[0]
