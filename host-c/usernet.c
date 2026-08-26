/* usernet.c -- see usernet.h. lwIP netif over the CoreDeviceProxy connection. */
#include "usernet.h"
#include "api_server.h"          /* imd_conn_send / imd_conn_recv */

#include <string.h>
#include <stdio.h>
#include <pthread.h>

#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"
#include "lwip/ip6_addr.h"
#include "lwip/ip6.h"
#include "lwip/pbuf.h"
#include "lwip/inet.h"

static void *g_conn;             /* the idevice_connection_t */
static struct netif g_netif;

/* lwIP wants to send an IPv6 packet: write its raw bytes to the tunnel connection. Called on the
 * tcpip thread. pbufs may be chained. */
static err_t tun_output(struct netif *nif, struct pbuf *p, const ip6_addr_t *ip)
{
    (void)nif; (void)ip;
    /* Coalesce the chain into one contiguous packet for a single framed write. */
    uint8_t stackbuf[2048];
    uint8_t *buf = p->tot_len <= sizeof stackbuf ? stackbuf : malloc(p->tot_len);
    if (!buf) return ERR_MEM;
    pbuf_copy_partial(p, buf, p->tot_len, 0);
    int rc = imd_conn_send(g_conn, buf, p->tot_len);
    if (buf != stackbuf) free(buf);
    return rc == 0 ? ERR_OK : ERR_IF;
}

static err_t tun_netif_init(struct netif *nif)
{
    nif->name[0] = 't'; nif->name[1] = 'n';
    nif->output_ip6 = tun_output;
    nif->mtu = 15000;
    nif->flags = NETIF_FLAG_UP | NETIF_FLAG_LINK_UP;   /* point-to-point, no L2 */
    return ERR_OK;
}

/* Read raw IPv6 packets off the tunnel connection and hand them to lwIP. The device end writes a
 * byte stream of back-to-back IPv6 packets; each is a 40-byte header whose bytes 4..5 give the
 * payload length, then that many payload bytes (same reframing the old utun pump did). */
static void *reader_thread(void *arg)
{
    (void)arg;
    for (;;) {
        uint8_t hdr[40];
        if (imd_conn_recv(g_conn, hdr, sizeof hdr) != 0) break;
        uint16_t plen = (uint16_t)((hdr[4] << 8) | hdr[5]);
        size_t total = 40u + plen;
        struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)total, PBUF_POOL);
        if (!p) { /* drop; read+discard the payload to stay in frame */
            uint8_t skip[2048];
            size_t left = plen;
            while (left) { size_t c = left < sizeof skip ? left : sizeof skip;
                           if (imd_conn_recv(g_conn, skip, c) != 0) return NULL; left -= c; }
            continue;
        }
        memcpy(p->payload, hdr, 40);
        if (plen) {
            /* payload may span pbuf chain; read into a temp then copy in */
            uint8_t tmp[16384];
            size_t got = 0;
            int ok = 1;
            while (got < plen) {
                size_t c = plen - got < sizeof tmp ? plen - got : sizeof tmp;
                if (imd_conn_recv(g_conn, tmp, c) != 0) { ok = 0; break; }
                pbuf_take_at(p, tmp, (u16_t)c, (u16_t)(40 + got));
                got += c;
            }
            if (!ok) { pbuf_free(p); break; }
        }
        if (g_netif.input(p, &g_netif) != ERR_OK)   /* tcpip_input: thread-safe hand-off */
            pbuf_free(p);
    }
    return NULL;
}

static volatile int g_ready;
static void on_tcpip_init(void *arg) { (void)arg; g_ready = 1; }

int usernet_start(void *idev_conn, const char *our_addr, const char *dev_addr)
{
    g_conn = idev_conn;
    tcpip_init(on_tcpip_init, NULL);
    while (!g_ready) { struct timespec ts = { 0, 1000000 }; nanosleep(&ts, NULL); }

    ip6_addr_t ours, dev;
    if (!ip6addr_aton(our_addr, &ours) || !ip6addr_aton(dev_addr, &dev)) return -1;

    /* Add the netif on the tcpip thread. netif_add + address set, then default + up. */
    netif_add_noaddr(&g_netif, NULL, tun_netif_init, tcpip_input);
    netif_ip6_addr_set(&g_netif, 0, &ours);
    netif_ip6_addr_set_state(&g_netif, 0, IP6_ADDR_VALID);   /* skip DAD: point-to-point, no collision */
    (void)dev;   /* the device is the same /64 as `ours`, so ip6_route sends it out this netif */
    netif_set_default(&g_netif);
    netif_set_up(&g_netif);
    netif_set_link_up(&g_netif);

    pthread_t t;
    if (pthread_create(&t, NULL, reader_thread, NULL) != 0) return -1;
    pthread_detach(t);
    return 0;
}

int usernet_connect(const char *addr, int port)
{
    int s = lwip_socket(AF_INET6, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in6 sa;
    memset(&sa, 0, sizeof sa);
    sa.sin6_family = AF_INET6;
    sa.sin6_port = lwip_htons((u16_t)port);
    if (lwip_inet_pton(AF_INET6, addr, &sa.sin6_addr) != 1) { lwip_close(s); return -1; }
    if (lwip_connect(s, (struct sockaddr *)&sa, sizeof sa) != 0) { lwip_close(s); return -1; }
    return s;
}

long usernet_read(int fd, void *buf, size_t n)  { return lwip_recv(fd, buf, n, 0); }
long usernet_write(int fd, const void *buf, size_t n) { return lwip_send(fd, buf, n, 0); }
void usernet_close(int fd) { lwip_close(fd); }

int usernet_udp_socket(int *bound_port)
{
    int s = lwip_socket(AF_INET6, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in6 sa;
    memset(&sa, 0, sizeof sa);
    sa.sin6_family = AF_INET6;
    socklen_t slen = sizeof sa;
    if (lwip_bind(s, (struct sockaddr *)&sa, sizeof sa) != 0 ||
        lwip_getsockname(s, (struct sockaddr *)&sa, &slen) != 0) {
        lwip_close(s);
        return -1;
    }
    if (bound_port) *bound_port = lwip_ntohs(sa.sin6_port);
    return s;
}

long usernet_recvfrom(int fd, void *buf, size_t n, usernet_addr *from)
{
    struct sockaddr_storage ss;
    socklen_t slen = sizeof ss;
    long r = lwip_recvfrom(fd, buf, n, 0, (struct sockaddr *)&ss, &slen);
    if (r >= 0 && from) {
        if ((size_t)slen > sizeof from->raw) slen = sizeof from->raw;
        memcpy(from->raw, &ss, slen);
        from->len = (unsigned int)slen;
    }
    return r;
}

long usernet_sendto(int fd, const void *buf, size_t n, const usernet_addr *to)
{
    return lwip_sendto(fd, buf, n, 0, (const struct sockaddr *)to->raw, (socklen_t)to->len);
}

int usernet_set_recv_timeout_ms(int fd, int ms)
{
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    return lwip_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}
