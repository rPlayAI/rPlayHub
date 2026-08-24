/* tunio.c -- the kernel-vs-lwIP routing for tunnel connections. Kept apart from usernet.c because
 * lwIP's <lwip/sockets.h> and the system <sys/socket.h> both define struct sockaddr et al. and
 * cannot share a translation unit. This file uses the system headers and calls into usernet.c
 * (lwIP) only through the plain functions in usernet.h. */
#include "usernet.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int g_on;
void usernet_enable(int on) { g_on = on; }
int usernet_is_on(void) { return g_on; }

int tun_connect(const char *addr, int port, int timeout_s)
{
    if (g_on) return usernet_connect(addr, port);
    struct sockaddr_in6 sa;
    memset(&sa, 0, sizeof sa);
    sa.sin6_family = AF_INET6;
    sa.sin6_port = htons((uint16_t)port);
    if (inet_pton(AF_INET6, addr, &sa.sin6_addr) != 1) return -1;
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = { .tv_sec = timeout_s > 0 ? timeout_s : 10, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return -1; }
    return fd;
}

long tun_read(int fd, void *buf, size_t n)  { return usernet_owns(fd) ? usernet_read(fd, buf, n) : (long)recv(fd, buf, n, 0); }
long tun_write(int fd, const void *buf, size_t n) { return usernet_owns(fd) ? usernet_write(fd, buf, n) : (long)send(fd, buf, n, 0); }
void tun_close(int fd) { if (usernet_owns(fd)) usernet_close(fd); else close(fd); }
