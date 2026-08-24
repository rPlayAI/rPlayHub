/* lwIP-over-tunnel feasibility spike.
 *
 * Proves lwIP's IPv6 TCP runs in our tree over a RAW-IPv6-PACKET channel with NO kernel routing
 * and NO root -- the exact mechanism that would replace the utun+pump. Two lwIP netifs are bridged
 * by in-memory packet queues (netif A's ip6 output -> netif B's input, and back), standing in for
 * the two ends of the CoreDevice tunnel. One runs a TCP server (raw API), the other connects and
 * sends a request; we verify the bytes cross. If this works, swapping the in-memory bridge for the
 * tunnel socket's raw-IPv6 stream (host-c/cdhost.c pump framing) is the whole integration.
 */
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "lwip/ip6_addr.h"
#include "lwip/ip6.h"
#include "lwip/pbuf.h"
#include <stdio.h>
#include <string.h>

/* A tiny packet queue standing in for one direction of the link. */
#define QN 256
typedef struct { struct pbuf *q[QN]; int head, tail; struct netif *peer; } link_t;
static link_t linkA, linkB;   /* A->B and B->A */

static u32_t now_ms;
u32_t sys_now(void) { return now_ms; }

/* netif output: enqueue the packet for delivery to the peer netif. */
static err_t link_output(struct netif *nif, struct pbuf *p, const ip6_addr_t *ipaddr) {
    (void)ipaddr;
    link_t *L = (link_t *)nif->state;
    struct pbuf *c = pbuf_alloc(PBUF_RAW, p->tot_len, PBUF_POOL);
    if (!c) return ERR_MEM;
    pbuf_copy(c, p);
    int n = (L->head + 1) % QN;
    if (n == L->tail) { pbuf_free(c); return ERR_MEM; }
    L->q[L->head] = c; L->head = n;
    return ERR_OK;
}

static err_t link_init(struct netif *nif) {
    nif->name[0] = 'u'; nif->name[1] = 't';
    nif->output_ip6 = link_output;
    nif->mtu = 15000;
    nif->flags = NETIF_FLAG_UP | NETIF_FLAG_LINK_UP;   /* point-to-point, no L2 */
    return ERR_OK;
}

/* Drain a link's queue into the peer netif's input. */
static void pump(link_t *L) {
    while (L->tail != L->head) {
        struct pbuf *p = L->q[L->tail]; L->tail = (L->tail + 1) % QN;
        if (ip6_input(p, L->peer) != ERR_OK) pbuf_free(p);
    }
}

/* ---- server (netif B side) ---- */
static int got_request = 0, got_reply = 0;
static err_t srv_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg; (void)err;
    if (!p) { tcp_close(pcb); return ERR_OK; }
    char buf[64] = {0}; pbuf_copy_partial(p, buf, p->tot_len < 63 ? p->tot_len : 63, 0);
    printf("  server received: \"%s\"\n", buf);
    got_request = 1;
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    const char *reply = "PONG-over-lwip";
    tcp_write(pcb, reply, strlen(reply), TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    return ERR_OK;
}
static err_t srv_accept(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg; (void)err;
    printf("  server accepted a connection\n");
    tcp_recv(pcb, srv_recv);
    return ERR_OK;
}

/* ---- client (netif A side) ---- */
static err_t cli_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg; (void)err;
    if (!p) return ERR_OK;
    char buf[64] = {0}; pbuf_copy_partial(p, buf, p->tot_len < 63 ? p->tot_len : 63, 0);
    printf("  client received: \"%s\"\n", buf);
    got_reply = 1;
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}
static err_t cli_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg; (void)err;
    printf("  client TCP handshake complete -> sending request\n");
    tcp_recv(pcb, cli_recv);
    const char *req = "PING-over-lwip";
    tcp_write(pcb, req, strlen(req), TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    return ERR_OK;
}

int main(void) {
    lwip_init();

    static struct netif nifA, nifB;
    ip6_addr_t a6, b6;
    ip6addr_aton("fd00::1", &a6);      /* "our" side */
    ip6addr_aton("fd00::2", &b6);      /* "device" side */

    linkA.peer = &nifB; linkB.peer = &nifA;
    netif_add(&nifA, &linkA, link_init, NULL);   /* IPv4 args NULL (LWIP_IPV4=0) */
    netif_add(&nifB, &linkB, link_init, NULL);
    netif_ip6_addr_set(&nifA, 0, &a6); netif_ip6_addr_set_state(&nifA, 0, IP6_ADDR_VALID);
    netif_ip6_addr_set(&nifB, 0, &b6); netif_ip6_addr_set_state(&nifB, 0, IP6_ADDR_VALID);
    netif_set_default(&nifA);
    netif_set_up(&nifA); netif_set_up(&nifB);

    /* server listens on the device side */
    struct tcp_pcb *lp = tcp_new_ip_type(IPADDR_TYPE_V6);
    tcp_bind(lp, &b6, 5555);
    lp = tcp_listen(lp);
    tcp_accept(lp, srv_accept);

    /* client connects from our side to the device side */
    struct tcp_pcb *cp = tcp_new_ip_type(IPADDR_TYPE_V6);
    /* route: default netif is A; but B's address is on a different netif. Add B as a gateway route
     * by binding the client to A and letting ip6_route pick A (its output goes to B via the bridge). */
    ip_addr_t dst; IP_ADDR6_HOST(&dst, 0xfd000000, 0, 0, 2);
    tcp_bind(cp, &a6, 0);
    tcp_connect(cp, &dst, 5555, cli_connected);

    for (int i = 0; i < 2000 && !(got_request && got_reply); i++) {
        now_ms += 5;
        sys_check_timeouts();
        pump(&linkA);
        pump(&linkB);
    }
    printf("\nRESULT: request %s, reply %s -> %s\n",
           got_request ? "delivered" : "LOST", got_reply ? "delivered" : "LOST",
           (got_request && got_reply) ? "lwIP IPv6 TCP over a raw-packet channel WORKS" : "FAILED");
    return (got_request && got_reply) ? 0 : 1;
}
