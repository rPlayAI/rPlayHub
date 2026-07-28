#include "api_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../core/rp_coredevice.h"
#include "../core/rp_xpc.h"

#define MAX_CLIENTS 16

typedef struct {
    int  fd;
    char in[8192];
    size_t in_len;
} client_t;

static api_session *g_session;

/* ------------------------------------------------------------------ tiny JSON helpers
 *
 * Deliberately minimal rather than a JSON library. The requests this serves are a handful of
 * flat objects with known keys, and every byte of them comes from our own app; a parser that
 * handles exactly that and refuses anything else is less code AND a smaller attack surface than
 * a general one.
 */
static int json_string_field(const char *json, const char *key, char *out, size_t out_cap)
{
    char pattern[64];
    snprintf(pattern, sizeof pattern, "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return -1;
    p = strchr(p + strlen(pattern), ':');
    if (!p) return -1;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_cap) out[i++] = *p++;
    out[i] = 0;
    return 0;
}

static long json_number_field(const char *json, const char *key, long fallback)
{
    char pattern[64];
    snprintf(pattern, sizeof pattern, "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return fallback;
    p = strchr(p + strlen(pattern), ':');
    if (!p) return fallback;
    return strtol(p + 1, NULL, 10);
}

static void send_line(int fd, const char *fmt, ...)
{
    char buf[8192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    buf[n++] = '\n';
    ssize_t off = 0;
    while (off < n) {
        ssize_t w = send(fd, buf + off, (size_t)(n - off), 0);
        if (w <= 0) return;
        off += w;
    }
}

static void reply_error(int fd, long id, const char *code, const char *message)
{
    send_line(fd, "{\"id\":%ld,\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
              id, code, message);
}

/* ------------------------------------------------------------------ methods */

static void method_ping(int fd, long id)
{
    send_line(fd, "{\"id\":%ld,\"ok\":true,\"result\":{\"engine\":\"cdhostd\",\"language\":\"c\"}}", id);
}

static void method_list_devices(int fd, long id)
{
    const api_session *s = g_session;
    send_line(fd,
              "{\"id\":%ld,\"ok\":true,\"result\":{\"devices\":[{"
              "\"udid\":\"%s\",\"name\":\"%s\",\"product_version\":\"%s\","
              "\"screen_width\":%d,\"screen_height\":%d,\"connection\":\"usb\"}]}}",
              id, s->udid, s->device_name, s->product_version, s->screen_w, s->screen_h);
}

static void method_stream_info(int fd, long id)
{
    const api_session *s = g_session;
    /* Honest zeros. The media stream is not implemented in C yet, and reporting plausible
     * numbers for a stream that does not exist would make the app look connected to nothing. */
    send_line(fd,
              "{\"id\":%ld,\"ok\":true,\"result\":{"
              "\"port\":%d,\"codec\":\"hevc\",\"container\":\"annexb\",\"viewers\":0,"
              "\"nals\":0,\"rtp_packets\":0,\"keyframes\":0,\"loss_pct\":0.0,\"mbps\":0.0,"
              "\"engine\":\"cdhostd\",\"streaming\":false,"
              "\"display_service_port\":%ld,\"hid_service_port\":%ld}}",
              id, STREAM_PORT, s->display_port, s->hid_port);
}

/* ------------------------------------------------------------------ server */

static int listen_on(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* local only: this is a developer tool */
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 8) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void dispatch(int fd, const char *line)
{
    long id = json_number_field(line, "id", 0);
    char method[64] = {0};
    if (json_string_field(line, "method", method, sizeof method) != 0) {
        reply_error(fd, id, "bad_request", "no method");
        return;
    }

    if (!strcmp(method, "ping"))              { method_ping(fd, id); return; }
    if (!strcmp(method, "list_devices"))      { method_list_devices(fd, id); return; }
    if (!strcmp(method, "stream_info"))       { method_stream_info(fd, id); return; }

    /* Named explicitly rather than lumped into one message, so the log says which capability is
     * missing rather than that something is. */
    char msg[160];
    snprintf(msg, sizeof msg,
             "%s is not implemented by the C engine yet; run the Python engine for it", method);
    reply_error(fd, id, "not_implemented", msg);
}

static void handle_readable(client_t *c)
{
    ssize_t r = recv(c->fd, c->in + c->in_len, sizeof c->in - c->in_len - 1, 0);
    if (r <= 0) { close(c->fd); c->fd = -1; return; }
    c->in_len += (size_t)r;
    c->in[c->in_len] = 0;

    char *start = c->in;
    for (;;) {
        char *nl = strchr(start, '\n');
        if (!nl) break;
        *nl = 0;
        if (*start) dispatch(c->fd, start);
        start = nl + 1;
    }
    size_t left = c->in_len - (size_t)(start - c->in);
    memmove(c->in, start, left);
    c->in_len = left;

    if (c->in_len == sizeof c->in - 1) {        /* a line longer than the buffer: drop it */
        c->in_len = 0;
    }
}

int api_serve(api_session *session)
{
    g_session = session;

    int api_fd = listen_on(API_PORT);
    int video_fd = listen_on(STREAM_PORT);
    if (api_fd < 0 || video_fd < 0) {
        fprintf(stderr, "  cannot listen on %d/%d: %s\n", API_PORT, STREAM_PORT, strerror(errno));
        return -1;
    }
    printf("  control: 127.0.0.1:%d (JSON lines)\n", API_PORT);
    printf("  video:   127.0.0.1:%d (no stream yet — media negotiation is still Python-only)\n",
           STREAM_PORT);

    client_t clients[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; i++) { clients[i].fd = -1; clients[i].in_len = 0; }

    for (;;) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(api_fd, &rd);
        FD_SET(video_fd, &rd);
        int maxfd = api_fd > video_fd ? api_fd : video_fd;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd >= 0) {
                FD_SET(clients[i].fd, &rd);
                if (clients[i].fd > maxfd) maxfd = clients[i].fd;
            }
        }
        if (select(maxfd + 1, &rd, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (FD_ISSET(api_fd, &rd)) {
            int fd = accept(api_fd, NULL, NULL);
            if (fd >= 0) {
                int one = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                int slot = -1;
                for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i].fd < 0) { slot = i; break; }
                if (slot < 0) close(fd);
                else { clients[slot].fd = fd; clients[slot].in_len = 0; printf("  client connected\n"); }
            }
        }
        if (FD_ISSET(video_fd, &rd)) {
            /* Accept and hold. The app treats a closed video port as a fatal disconnect and
             * retries in a loop, so refusing outright would look like a broken engine rather
             * than one whose streaming is not written yet. */
            int fd = accept(video_fd, NULL, NULL);
            if (fd >= 0) printf("  video viewer connected (nothing to send yet)\n");
        }
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (clients[i].fd >= 0 && FD_ISSET(clients[i].fd, &rd))
                handle_readable(&clients[i]);
    }
    close(api_fd);
    close(video_fd);
    return 0;
}
