/* lwIP options for the userspace-tunnel spike: single-threaded raw API (NO_SYS), IPv6 only, TCP.
 * The engine's tunnel is a point-to-point link carrying raw IPv6 packets, so no IPv4, no ARP, no
 * DHCP, no link-layer -- lwIP just needs to run IPv6 + TCP over a packet channel we drive. */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS                      1
#define LWIP_TIMERS                 1
#define SYS_LIGHTWEIGHT_PROT        0

#define LWIP_IPV4                   0
#define LWIP_IPV6                   1
#define LWIP_ICMP6                  1
#define LWIP_IPV6_MLD               0
#define LWIP_IPV6_ND                1
#define LWIP_ND6_NUM_NEIGHBORS      4
#define LWIP_IPV6_AUTOCONFIG        0
#define LWIP_IPV6_DHCP6             0
#define LWIP_IPV6_SEND_ROUTER_SOLICIT 0

#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_RAW                    0
#define LWIP_DNS                    0
#define LWIP_NETCONN                0
#define LWIP_SOCKET                 0
#define LWIP_NETIF_API              0

#define LWIP_ARP                    0
#define LWIP_ETHERNET               0
#define LWIP_NETIF_HWADDRLEN        0

/* The device tunnel MTU is 16000; give pbufs and windows room. */
#define TCP_MSS                     1420
#define TCP_WND                     (32 * TCP_MSS)
#define TCP_SND_BUF                 (32 * TCP_MSS)
#define TCP_SND_QUEUELEN            (4 * (TCP_SND_BUF / TCP_MSS))
#define MEM_SIZE                    (2 * 1024 * 1024)
#define MEMP_NUM_TCP_PCB            16
#define MEMP_NUM_TCP_SEG            (4 * TCP_SND_QUEUELEN)
#define PBUF_POOL_SIZE              64
#define PBUF_POOL_BUFSIZE           1600
#define IP_REASS_MAX_PBUFS         0
#define LWIP_IPV6_REASS            0

#define MEM_ALIGNMENT               8
#define LWIP_STATS                  0
#define LWIP_NETIF_STATUS_CALLBACK  0
#define LWIP_NETIF_LINK_CALLBACK    0
#define LWIP_DEBUG                  0

#define LWIP_RAND()                 ((u32_t)rand())

#endif
