# Userspace TCP/IP over the tunnel (lwIP) — spike

Proves the no-root path for TestFlight and the Linux/Windows ports: run a userspace TCP/IP stack
over the CoreDevice tunnel socket instead of a kernel `utun`, so the engine needs no root and no
TUN driver.

## Result (2026-08-23)

`spike.c` builds lwIP 2.2.1 (`deps/lwip`, ~19-file core subset, IPv6 + TCP, `NO_SYS=1`) and runs a
TCP client and server across two in-memory-bridged IPv6 netifs — a full handshake and data exchange
with **no kernel routing and no root**. Build + run:

    L=../../deps/lwip/src
    cc -I . -I ../../deps/lwip/src/include -o /tmp/spike spike.c \
       $L/core/init.c $L/core/def.c $L/core/inet_chksum.c $L/core/ip.c $L/core/mem.c \
       $L/core/memp.c $L/core/netif.c $L/core/pbuf.c $L/core/tcp.c $L/core/tcp_in.c \
       $L/core/tcp_out.c $L/core/timeouts.c $L/core/udp.c $L/core/ipv6/ip6.c \
       $L/core/ipv6/ip6_addr.c $L/core/ipv6/icmp6.c $L/core/ipv6/nd6.c \
       $L/core/ipv6/ip6_frag.c $L/core/ipv6/inet6.c && /tmp/spike

Prints `lwIP IPv6 TCP over a raw-packet channel WORKS`.

## Why this is the whole integration

`host-c/cdhost.c`'s pump shows the tunnel socket is already a **raw-IPv6-packet stream** (read the
40-byte IPv6 header, take Payload Length, read the rest; write packets back verbatim). So replacing
the utun is mechanical:

1. A receive loop reads each IPv6 packet from the tunnel socket into a `pbuf` and calls
   `ip6_input(p, netif)` — exactly what `spike.c`'s `pump()` does, but sourced from the socket.
2. The netif's `output_ip6` writes the packet to the tunnel socket instead of an in-memory queue.
3. Every `svc_open` / media connect stops using a kernel `AF_INET6` socket and instead opens an
   lwIP TCP PCB (`tcp_new_ip_type(IPADDR_TYPE_V6)` + `tcp_connect`) to the device address. The RSD
   and per-service byte streams then flow through lwIP.
4. `sys_check_timeouts()` runs on the engine's event loop (or its own thread); the raw API needs no
   locking under `NO_SYS=1`.

No root, no `utun`, no `route add` — which is what the App Store sandbox requires, and what the
Linux (no `tun` device) and Windows (no signed TUN driver) ports want too.

## What remains for TestFlight

This removes root. The *other* App Store blocker is usbmuxd: a sandboxed app cannot reach
`/var/run/usbmuxd` (tested — even the absolute-path temporary exception gives EPERM). That needs the
RemotePairing transport (`doc/REMOTEPAIRING-PROTOCOL.md`) — direct wifi to the phone, no usbmuxd —
which is the remaining large piece. See `app/DISTRIBUTION.md`.
