#ifndef RP_COMPAT_H
#define RP_COMPAT_H

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#include <direct.h>
#include <process.h>
#include <stdint.h>
#include <sys/stat.h>

typedef intptr_t ssize_t;
#define sleep(x) Sleep((DWORD)((x)*1000))
#define usleep(x) Sleep((DWORD)((x)/1000))
#define close(x) closesocket(x)
#define mkdir(p, m) _mkdir(p)

static inline int set_nonblocking(int fd) {
    u_long mode = 1;
    return ioctlsocket((SOCKET)fd, FIONBIO, &mode);
}

static inline int is_sock_wouldblock(void) {
    int err = WSAGetLastError();
    return err == WSAEWOULDBLOCK || err == WSAEINTR;
}

#else

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

static inline int set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static inline int is_sock_wouldblock(void) {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

#endif

#endif /* RP_COMPAT_H */
