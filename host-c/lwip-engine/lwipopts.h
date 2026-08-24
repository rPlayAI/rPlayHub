/* lwIP config for the engine's userspace TCP/IP over the CoreDevice tunnel (removes the root utun,
 * so the engine runs unprivileged on macOS and needs no TUN device on Linux). OS mode (NO_SYS=0)
 * with the BSD socket API and lwIP's own tcpip thread, so the engine's existing blocking-socket,
 * multi-threaded code maps onto lwip_socket/connect/recv/send almost unchanged. IPv6 only -- the
 * tunnel is a point-to-point IPv6 link carrying raw packets. */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS                      0
#define SYS_LIGHTWEIGHT_PROT        1
#define LWIP_TCPIP_CORE_LOCKING     1

#define LWIP_SOCKET                 1
#define LWIP_NETCONN                1
#define LWIP_COMPAT_SOCKETS         0     /* call lwip_socket() explicitly; don't shadow socket() */
#define LWIP_SOCKET_OFFSET          768   /* lwIP fds 768..800, clear of the daemon's kernel fds; offset+NETCONN < FD_SETSIZE */
#define LWIP_POSIX_SOCKETS_IO_NAMES 0
#define LWIP_SOCKET_SELECT          0     /* the engine uses blocking I/O per thread, not lwIP select */
#define LWIP_SOCKET_POLL            0

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
#define LWIP_ARP                    0
#define LWIP_ETHERNET              0
#define LWIP_NETIF_HWADDRLEN        0

/* Screenshots are multi-MB TCP transfers; give TCP room. The device MTU is 16000. */
#define LWIP_WND_SCALE              1     /* windows > 64 KB for the multi-MB screenshot transfers */
#define TCP_RCV_SCALE               2
#define TCP_MSS                     1420
#define TCP_WND                     (64 * TCP_MSS)
#define TCP_SND_BUF                 (64 * TCP_MSS)
#define TCP_SND_QUEUELEN            (4 * (TCP_SND_BUF / TCP_MSS))
#define MEM_SIZE                    (8 * 1024 * 1024)
#define MEMP_NUM_TCP_PCB            32
#define MEMP_NUM_TCP_SEG           (8 * TCP_SND_QUEUELEN)
#define MEMP_NUM_NETCONN            32
#define PBUF_POOL_SIZE              256
#define PBUF_POOL_BUFSIZE           1600
#define LWIP_IPV6_REASS            0
#define IPV6_FRAG_COPYHEADER        1

#define MEM_ALIGNMENT               8
#define LWIP_STATS                  0
#define LWIP_NETIF_STATUS_CALLBACK  0
#define LWIP_NETIF_LINK_CALLBACK    0
#define LWIP_DEBUG                  0

/* pthread port needs these thread settings. */
#define TCPIP_THREAD_STACKSIZE      65536
#define TCPIP_THREAD_PRIO           1
#define TCPIP_MBOX_SIZE             64
#define DEFAULT_THREAD_STACKSIZE    65536
#define DEFAULT_RAW_RECVMBOX_SIZE   64
#define DEFAULT_UDP_RECVMBOX_SIZE   64
#define DEFAULT_TCP_RECVMBOX_SIZE   64
#define DEFAULT_ACCEPTMBOX_SIZE     64


#endif
