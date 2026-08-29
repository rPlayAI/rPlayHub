#!/usr/bin/env python3
"""Device Hub's Settings tab — read and write the device's appearance/accessibility settings.

    python3 host/settings.py probe          # which RSD service hosts these actions
    python3 host/settings.py get            # read every setting
    python3 host/settings.py set <key> <value>

Reverse-engineered from a tcpdump of Device Hub's own traffic on the CoreDevice tunnel
(host/capture-settings.sh + host/decode_settings.py, capture settings-20260828-223037.pcap).

NOT usbmux. Checked live: DeviceHub.app holds zero connections to /var/run/usbmuxd — every
device connection is TCP over the tunnel. usbmux only brings the tunnel up.

WIRE FORMAT — the same CoreDevice envelope screenshots and media already use (coredevice.py),
but with ONLY an actionIdentifier, no featureIdentifier:

    request   CoreDevice.actionIdentifier = com.apple.coredevice.action.setshowborders
              CoreDevice.input.showBorders.enabled = true
    response  CoreDevice.output.showBorders.enabled = true

Each setting has its own action pair and its own input/output key, listed in SETTINGS below.
The action names come from the capture AND from the full catalog in Apple's own
CoreDeviceUtilities.framework, which carries every com.apple.coredevice.action.* string — that
is where the ones Device Hub did not happen to call during the capture (setuserinterfacestyle,
setcolorfilter, setreducemotion, setreducetransparency, setdevicetextsize) were confirmed to
exist rather than guessed at.
"""
import json
import sys
import uuid

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import rsd
from coredevice import CoreDeviceService
from diagnostics_relay import rpc

ACTION = "com.apple.coredevice.action."

# name -> (get action, set action, payload key, kind)
#
# `payload key` is the single key inside CoreDevice.input / CoreDevice.output. It is NOT derivable
# from the action name (setshowborders -> showBorders, but setdeviceincreasecontrast ->
# increaseContrast and setvoiceover -> voiceOverConfiguration), so each one is recorded as
# observed rather than computed.
SETTINGS = {
    "showBorders":       ("getshowborders", "setshowborders", "showBorders", "enabled"),
    "increaseContrast":  ("getdeviceincreasecontrast", "setdeviceincreasecontrast",
                          "increaseContrast", "enabled"),
    "reduceMotion":      ("getreducemotion", "setreducemotion", "reduceMotion", "enabled"),
    "reduceTransparency": ("getreducetransparency", "setreducetransparency",
                           "reduceTransparency", "enabled"),
    "voiceOver":         ("getvoiceover", "setvoiceover", "voiceOverConfiguration", "enabled"),
    "colorFilter":       ("getcolorfilter", "setcolorfilter", "colorFilter", "enabled"),
    # Confirmed in the capture: input is {configuration: {opacity: <double 0..1>}}.
    "liquidGlass":       ("getliquidglassconfiguration", "setliquidglassconfiguration",
                          "configuration", "opacity"),
    # Present in the framework's action catalog; their payload shapes are NOT yet confirmed on
    # the wire (Device Hub did not touch them during the capture). get first, then mirror the
    # shape it returns — do not assume it matches the boolean ones.
    "textSize":          ("getdevicetextsize", "setdevicetextsize", None, None),
    "appearance":        ("getuserinterfacestyle", "setuserinterfacestyle", None, None),
    "lookAndFeel":       ("getdevicelookandfeel", "setdevicelookandfeel", None, None),
    "largerAccessibilitySizes": ("getlargeraccessibilitysizesenabled",
                                 "setlargeraccessibilitysizesenabled", None, None),
}

# Services worth probing. The capture proves these actions are NOT on the same port as location
# (com.apple.coredevice.locationservice, which answers setsimulatedlocation) or pasteboard, but
# the RSD catalog is per-session so the port alone does not name the service — hence probe().
CANDIDATES = ("com.apple.coredevice.configuration",
              "com.apple.coredevice.devicecontrol",
              "com.apple.coredevice.deviceinfo",
              "com.apple.coredevice.appservice",
              "com.apple.coredevice.diagnosticsservice")


def tunnel():
    info = rpc("tunnel_info").get("result", {})
    addr, port = info.get("device_addr"), info.get("rsd_port")
    if not addr or not port:
        sys.exit(f"daemon has no live tunnel: {info}")
    return addr, port


def probe():
    """Find which service answers the settings actions, read-only.

    getreducemotion takes no input and changes nothing, so this is safe to run against every
    candidate: the wrong service simply fails to produce CoreDevice.output.
    """
    addr, rsd_port = tunnel()
    peer = rsd.connect(addr, rsd_port)
    for name in CANDIDATES:
        port = rsd.service_port(peer, name)
        if not port:
            print(f"  {name}: not advertised")
            continue
        try:
            svc = CoreDeviceService(addr, port)
            out = svc.invoke(action_identifier=ACTION + "getreducemotion", input_={})
            print(f"  {name}: ANSWERS -> {out}")
            return name
        except Exception as e:
            print(f"  {name}: {type(e).__name__} {e}")
    print("none of the candidates answered; run with the full RSD list to widen the search")
    return None


def open_service(name):
    addr, rsd_port = tunnel()
    port = rsd.service_port(rsd.connect(addr, rsd_port), name)
    if not port:
        sys.exit(f"{name} not advertised")
    return CoreDeviceService(addr, port)


def get_all(service_name):
    svc = open_service(service_name)
    out = {}
    for key, (get_action, _set, _pk, _kind) in SETTINGS.items():
        try:
            out[key] = svc.invoke(action_identifier=ACTION + get_action, input_={})
        except Exception as e:
            out[key] = f"<{type(e).__name__}: {e}>"
    return out


def set_one(service_name, key, value):
    if key not in SETTINGS:
        sys.exit(f"unknown setting {key}; known: {', '.join(sorted(SETTINGS))}")
    _get, set_action, payload_key, kind = SETTINGS[key]
    if payload_key is None:
        sys.exit(f"{key}'s payload shape is not confirmed yet — run `get` first and mirror it")
    if kind == "enabled":
        val = value.lower() in ("1", "true", "yes", "on")
    else:
        val = float(value)
    svc = open_service(service_name)
    return svc.invoke(action_identifier=ACTION + set_action,
                      input_={payload_key: {kind: val}})


def main(argv):
    cmd = argv[1] if len(argv) > 1 else "probe"
    if cmd == "probe":
        probe()
        return 0
    service = CANDIDATES[0]
    for i, a in enumerate(argv):
        if a == "--service" and i + 1 < len(argv):
            service = argv[i + 1]
    if cmd == "get":
        print(json.dumps(get_all(service), indent=2, default=str))
    elif cmd == "set" and len(argv) >= 4:
        print(json.dumps(set_one(service, argv[2], argv[3]), indent=2, default=str))
    else:
        sys.exit(__doc__)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
