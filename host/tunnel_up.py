#!/usr/bin/env python3
"""Layer 3a — bring the CoreDevice tunnel up as a real utun interface (macOS, needs root).

Creates a utun, assigns our tunnel address, splices IPv6 packets between utun and the
CoreDeviceProxy socket. Then the device's RSD port is reachable with ORDINARY sockets.

    sudo python3 tunnel_up.py [udid]

Leaves the tunnel up and prints a reachability check to [serverAddress]:serverRSDPort.
Ctrl-C to tear down. This is the fast path; a root-free userspace stack comes later.
"""
import fcntl, os, re, socket, struct, subprocess, sys, threading, time

import tunnel
import usbmux

CTLIOCGINFO = 0xC0644E03            # _IOWR('N', 3, struct ctl_info)
UTUN_CONTROL_NAME = b"com.apple.net.utun_control"
UTUN_OPT_IFNAME = 2
AF_INET6_HDR = struct.pack(">I", socket.AF_INET6)   # macOS utun 4-byte protocol prefix
IPV6_HDR = 40


def open_utun():
    """Create a utun interface; return (socket, ifname)."""
    s = socket.socket(socket.AF_SYSTEM, socket.SOCK_DGRAM, socket.SYSPROTO_CONTROL)
    info = struct.pack("I96s", 0, UTUN_CONTROL_NAME)
    info = fcntl.ioctl(s, CTLIOCGINFO, info)
    ctl_id = struct.unpack("I96s", info)[0]
    # unit 0 => kernel picks a free utun number
    s.connect((ctl_id, 0))
    ifname = s.getsockopt(socket.SYSPROTO_CONTROL, UTUN_OPT_IFNAME, 256)
    ifname = ifname.split(b"\x00", 1)[0].decode()
    return s, ifname


def configure(ifname, our_addr, dev_addr, prefixlen=64):
    subprocess.run(["ifconfig", ifname, "inet6", our_addr, "prefixlen", str(prefixlen)], check=True)
    subprocess.run(["ifconfig", ifname, "up"], check=True)
    # host route to the device tunnel address via this interface
    subprocess.run(["route", "add", "-inet6", dev_addr, "-interface", ifname],
                   check=False, capture_output=True)


def splice(utun, service, stop):
    """Two directions in one selectable loop-ish (threads, blocking reads)."""
    def utun_to_dev():
        while not stop.is_set():
            pkt = utun.recv(65536)            # 4-byte AF header + IPv6 packet
            if len(pkt) <= 4:
                continue
            service.sendall(pkt[4:])          # raw IPv6 to the device
    def dev_to_utun():
        while not stop.is_set():
            hdr = _recvn(service, IPV6_HDR, stop)
            if hdr is None:
                break
            (plen,) = struct.unpack(">H", hdr[4:6])
            body = _recvn(service, plen, stop) if plen else b""
            if body is None:
                break
            utun.send(AF_INET6_HDR + hdr + body)
    t1 = threading.Thread(target=utun_to_dev, daemon=True)
    t2 = threading.Thread(target=dev_to_utun, daemon=True)
    t1.start(); t2.start()
    return t1, t2


def _recvn(sock, n, stop):
    buf = b""
    while len(buf) < n:
        if stop.is_set():
            return None
        try:
            chunk = sock.recv(n - len(buf))
        except OSError:
            return None
        if not chunk:
            return None
        buf += chunk
    return buf


def _dispatch(action, peer, dev_addr, our_addr):
    """Run an optional control action through the live tunnel."""
    if not action or action[0] in ("hold", "enumerate"):
        return
    import rsd
    cmd = action[0]
    if cmd in ("tap", "swipe"):
        import hid, screen
        hid_port = rsd.service_port(peer, rsd.HID_SVC)
        disp_port = rsd.service_port(peer, "com.apple.coredevice.displayservice")
        if not hid_port:
            print("  HID service not advertised"); return
        # touch is only routed to UIKit while a media stream is running (dtuhidd auth gate)
        stream = None
        if disp_port:
            print("  opening video stream (HID auth gate)...")
            try:
                stream = screen.open_stream(dev_addr, our_addr, disp_port)
                time.sleep(1.2)   # let the gate open
            except Exception as e:
                print(f"  (auth-gate stream failed: {e!r} — touch may be ignored)")
        h = hid.UniversalHID(dev_addr, hid_port)
        if cmd == "tap":
            fx, fy = float(action[1]), float(action[2])
            print(f"\n>> tapping phone at ({fx}, {fy}) ...")
            h.tap(fx, fy)
        else:
            fx0, fy0, fx1, fy1 = map(float, action[1:5])
            print(f"\n>> swiping ({fx0},{fy0}) -> ({fx1},{fy1}) ...")
            h.swipe(fx0, fy0, fx1, fy1)
        h.close()
        if stream:
            print(f"   ({screen.close_stream(stream)} RTP packets seen while gate open)")
        print("   sent. Watch the phone.")
    elif cmd == "shot":
        import coredevice
        out = action[1] if len(action) > 1 else "screenshot.png"
        port = rsd.service_port(peer, rsd.SCREEN_SVC)
        if not port:
            print("  screencaptureservice not advertised"); return
        print(f"\n>> capturing screenshot -> {out} ...")
        n, fmt = coredevice.screenshot(dev_addr, port, out)
        print(f"   saved {n} bytes ({fmt}) -> {out}")
    elif cmd == "record":
        import screen
        out = action[1] if len(action) > 1 else "screen.h265"
        secs = float(action[2]) if len(action) > 2 else 10.0
        port = rsd.service_port(peer, "com.apple.coredevice.displayservice")
        if not port:
            print("  displayservice not advertised"); return
        print(f"\n>> recording {secs}s of screen -> {out} ...")
        pkts, nals = screen.record(dev_addr, our_addr, port, out, secs)
        print(f"   captured {pkts} RTP packets, {nals} NAL units -> {out}")
        print(f"   decode:  ffmpeg -i {out} {out.rsplit('.',1)[0]}.mp4")
    else:
        print(f"  unknown action: {cmd}")


def main():
    if os.geteuid() != 0:
        sys.exit("must run as root (sudo) to create a utun interface")
    args = sys.argv[1:]
    udid = None
    if args and re.match(r"^[0-9A-Fa-f]{8}-[0-9A-Fa-f]+$", args[0]):
        udid = args.pop(0)
    action = args   # [] | ["tap",fx,fy] | ["swipe",...] | ["shot",file] | ["record",file,secs]

    # wifi (Network) devices come and go in usbmuxd — wait for it to appear.
    if udid:
        for _ in range(15):
            if any(d.get("SerialNumber") == udid for d in usbmux.list_devices()):
                break
            time.sleep(1)
        else:
            sys.exit(f"device {udid} not visible to usbmuxd (wake it / ensure same LAN + wifi sync)")

    service, params = tunnel.establish_tunnel(udid)
    our_addr = params["clientParameters"]["address"]
    dev_addr = params["serverAddress"]
    rsd_port = params["serverRSDPort"]
    print(f"tunnel: us={our_addr} device={dev_addr} rsdPort={rsd_port} mtu={params['clientParameters']['mtu']}")

    utun, ifname = open_utun()
    print(f"utun: {ifname}")
    configure(ifname, our_addr, dev_addr)
    stop = threading.Event()
    splice(utun, service, stop)
    time.sleep(0.5)

    # Layer 3: enumerate RSD services THROUGH the utun (reveals screen + HID services).
    print(f"\nRSD: handshaking [{dev_addr}]:{rsd_port} ...")
    peer = None
    try:
        import rsd
        peer = rsd.connect(dev_addr, rsd_port)
        rsd.print_services(peer)
    except Exception as e:
        print(f"  ✗ RSD failed: {e!r}")
        # fall back to a bare reachability check
        try:
            socket.create_connection((dev_addr, rsd_port), timeout=5).close()
            print("  (but TCP connect succeeded — routing works; RSD handshake needs a look)")
        except OSError as e2:
            print(f"  TCP connect also failed: {e2}")

    # optional action through the live tunnel
    try:
        _dispatch(action, peer, dev_addr, our_addr)
    except Exception as e:
        print(f"  ✗ action {action} failed: {e!r}")

    print(f"\ntunnel is UP on {ifname}. Try:  ping6 -c3 {dev_addr}   |   Ctrl-C to tear down.")
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        subprocess.run(["ifconfig", ifname, "destroy"], check=False, capture_output=True)
        service.close(); utun.close()
        print("\ntorn down.")


if __name__ == "__main__":
    main()
