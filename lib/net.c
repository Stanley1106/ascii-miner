#include "net.h"
#include "log.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int g_fragment_us;

void mg_set_send_fragment(int delay_us) { g_fragment_us = delay_us; }

int mg_tcp_listen(uint16_t port, int backlog)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, backlog) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

int mg_tcp_connect(const char *host, uint16_t port)
{
    char ps[8];
    snprintf(ps, sizeof ps, "%u", port);
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *res, *ai;
    int rc = getaddrinfo(host, ps, &hints, &res);
    if (rc != 0) {
        LOG_ERROR("resolve %s: %s", host, gai_strerror(rc));
        return -1;
    }
    int fd = -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0 && g_fragment_us > 0) {
        /* Disable Nagle so each 1-byte write leaves as its own segment. */
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
    return fd;
}

ssize_t mg_send_all(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    size_t off = 0;
    while (off < n) {
        size_t chunk = g_fragment_us > 0 ? 1 : n - off;
        ssize_t k = send(fd, p + off, chunk, MSG_NOSIGNAL);
        if (k < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pf = { .fd = fd, .events = POLLOUT };
                int pr = poll(&pf, 1, 5000);
                if (pr < 0 && errno == EINTR)
                    continue;
                if (pr <= 0)
                    return -1;
                continue;
            }
            return -1;
        }
        if ((size_t)k < chunk)
            LOG_DEBUG("partial send: %zd of %zu bytes", k, chunk);
        off += (size_t)k;
        if (g_fragment_us > 0 && off < n)
            usleep(g_fragment_us);
    }
    return (ssize_t)off;
}

int mg_send_msg(int fd, uint8_t type, uint32_t seq, const void *payload, uint32_t len)
{
    static uint8_t frame[MG_HDR_LEN + MG_MAX_PAYLOAD];
    if (len > MG_MAX_PAYLOAD)
        return -1;
    mg_encode_header(frame, type, seq, len);
    if (len)
        memcpy(frame + MG_HDR_LEN, payload, len);
    LOG_DEBUG("send %s seq=%u len=%u fd=%d", mg_type_name(type), seq, len, fd);
    return mg_send_all(fd, frame, MG_HDR_LEN + len) < 0 ? -1 : 0;
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

int mg_recv_msg(int fd, mg_reader *r, mg_msg *m, int timeout_ms)
{
    long deadline = now_ms() + timeout_ms;
    for (;;) {
        int rc = mg_reader_next(r, m);
        if (rc == 1)
            return 1;
        if (rc < 0)
            return -3;
        long left = deadline - now_ms();
        if (left <= 0)
            return -2;
        struct pollfd pf = { .fd = fd, .events = POLLIN };
        int pr = poll(&pf, 1, (int)left);
        if (pr < 0 && errno == EINTR)
            continue;
        if (pr < 0)
            return -1;
        if (pr == 0)
            return -2;
        int n = mg_reader_fill(r, fd);
        if (n == 0)
            return 0;
        if (n < 0)
            return -1;
    }
}
