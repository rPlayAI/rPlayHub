"""Exception hierarchy for the mini-host.

Everything raised on purpose derives from HubError so a supervisor can catch one type.
The distinction that matters to callers:

  TransportError  — the bottom (usbmux / RemotePairing / relay) failed; retry the transport.
  LinkDown        — the tunnel was up and died; the session supervisor reconnects.
  ServiceMissing  — the device did not advertise a service we need; not retryable.
"""


class HubError(Exception):
    pass


class MuxError(HubError):
    """usbmuxd refused or closed."""


class LockdownError(HubError):
    """lockdownd returned an Error, or the pipe died."""


class TransportError(HubError):
    """Could not bring the CoreDevice tunnel up."""


class LinkDown(HubError):
    """The tunnel byte stream died while in use."""


class ServiceMissing(HubError):
    """A required coredevice.* service is not in the RSD catalog."""


class SessionClosed(HubError):
    """Operation attempted on a session that has been closed."""
