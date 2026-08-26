/* usernet.h -- userspace TCP/IP over the CoreDevice tunnel (lwIP), replacing the kernel utun.
 *
 * The utun needs root on macOS and a TUN device on Linux; running the IP stack in-process removes
 * both. After the CoreDeviceProxy tunnel handshake, call usernet_start with the connection and the
 * negotiated addresses; from then on usernet_connect() opens TCP connections to the device over
 * lwIP instead of kernel sockets. The connection carries raw IPv6 packets (same framing the pump
 * used), which the reader thread feeds to lwIP and lwIP's output feeds back. */
#ifndef RP_USERNET_H
#define RP_USERNET_H

#include <stddef.h>

/* Bring up lwIP with a point-to-point netif over `idev_conn` (an idevice_connection_t). `our_addr`
 * and `dev_addr` are the tunnel-handshake IPv6 addresses. Returns 0 on success. Idempotent-ish:
 * call once per daemon. */
int usernet_start(void *idev_conn, const char *our_addr, const char *dev_addr);

/* Open a TCP connection to [addr]:port over the userspace stack. Returns an lwIP socket fd
 * (>= USERNET_FD_BASE) or -1. Use usernet_read/write/close on it (NOT kernel recv/send). */
int usernet_connect(const char *addr, int port);
long usernet_read(int fd, void *buf, size_t n);
long usernet_write(int fd, const void *buf, size_t n);
void usernet_close(int fd);

/* UDP over the userspace stack, for media.c's RTP/RTCP. The peer address is an opaque blob
 * because lwIP's struct sockaddr and the system's cannot share a translation unit (see tunio.c's
 * header comment) -- the caller records where packets came from and echoes it back on send,
 * never looking inside. */
typedef struct { unsigned char raw[32]; unsigned int len; } usernet_addr;

/* Bind an ephemeral IPv6 UDP port; returns the fd and fills *bound_port (it goes in the offer). */
int  usernet_udp_socket(int *bound_port);
/* Returns -1 with errno EAGAIN on the receive timeout, like a kernel socket with SO_RCVTIMEO. */
long usernet_recvfrom(int fd, void *buf, size_t n, usernet_addr *from);
long usernet_sendto(int fd, const void *buf, size_t n, const usernet_addr *to);
int  usernet_set_recv_timeout_ms(int fd, int ms);

/* Whether userspace mode is on (set by cdhost from RPLAY_USERSPACE_NET). */
void usernet_enable(int on);
int  usernet_is_on(void);

/* Unified tunnel I/O: use these everywhere a tunnel connection is opened/read/written, and the
 * kernel-vs-lwIP choice is made centrally. tun_connect blocks with a connect timeout. */
int  tun_connect(const char *addr, int port, int timeout_s);
long tun_read(int fd, void *buf, size_t n);
long tun_write(int fd, const void *buf, size_t n);
void tun_close(int fd);

/* lwIP fds are offset above kernel fds; a fd >= this is a usernet (lwIP) socket. */
#define USERNET_FD_BASE 768
static inline int usernet_owns(int fd) { return fd >= USERNET_FD_BASE; }

#endif
