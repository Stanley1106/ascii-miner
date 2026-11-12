#include "log.h"
#include "net.h"
#include "test.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static mg_reader r;
static mg_msg m;
static uint8_t big[MG_MAX_PAYLOAD];

int main(void)
{
    mg_log_level = -1;
    int sv[2];

    /* basic send/recv of a framed message */
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    CHECK(mg_send_msg(sv[0], MSG_SYSINFO_REQ, 42, NULL, 0) == 0);
    mg_reader_init(&r);
    CHECK(mg_recv_msg(sv[1], &r, &m, 1000) == 1 && m.type == MSG_SYSINFO_REQ && m.seq == 42);

    /* timeout when nothing arrives */
    CHECK(mg_recv_msg(sv[1], &r, &m, 50) == -2);

    /* partial send: tiny non-blocking send buffer, a child drains slowly;
     * mg_send_all must still deliver every byte of a max-size payload */
    int sz = 4096;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
    fcntl(sv[0], F_SETFL, O_NONBLOCK);
    for (size_t i = 0; i < sizeof big; i++)
        big[i] = (uint8_t)(i * 31);
    pid_t pid = fork();
    if (pid == 0) {
        mg_reader_init(&r);
        int ok = mg_recv_msg(sv[1], &r, &m, 3000) == 1 && m.len == sizeof big &&
                 memcmp(m.payload, big, sizeof big) == 0;
        _exit(ok ? 0 : 1);
    }
    CHECK(mg_send_msg(sv[0], MSG_SNAPSHOT, 1, big, sizeof big) == 0);
    int st;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);

    close(sv[0]);
    close(sv[1]);

    /* fragment mode: bytes trickle in one at a time while a concurrent
     * reader counts how many recv() calls it took; it must still get one
     * correct frame */
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    pid = fork();
    if (pid == 0) {
        mg_reader_init(&r);
        int ok = mg_recv_msg(sv[1], &r, &m, 3000) == 1 && m.type == MSG_MOVE &&
                 m.len == 1 && m.payload[0] == 3;
        _exit(ok ? 0 : 1);
    }
    mg_set_send_fragment(1000);
    CHECK(mg_send_msg(sv[0], MSG_MOVE, 9, "\x03", 1) == 0);
    mg_set_send_fragment(0);
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);

    /* peer closed -> 0; send to closed peer -> -1 without SIGPIPE */
    close(sv[0]);
    mg_reader_init(&r);
    CHECK(mg_recv_msg(sv[1], &r, &m, 1000) == 0);
    CHECK(mg_send_msg(sv[1], MSG_PING, 1, NULL, 0) == -1);
    close(sv[1]);

    /* real TCP listen/connect on an ephemeral-ish port */
    int lfd = mg_tcp_listen(0, 4);
    CHECK(lfd >= 0);
    struct sockaddr_storage ss; socklen_t sl = sizeof ss;
    getsockname(lfd, (struct sockaddr *)&ss, &sl);
    uint16_t port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
    int cfd = mg_tcp_connect("127.0.0.1", port);
    int afd = accept(lfd, NULL, NULL);
    CHECK(cfd >= 0 && afd >= 0);
    CHECK(mg_send_msg(cfd, MSG_PING, 5, NULL, 0) == 0);
    mg_reader_init(&r);
    CHECK(mg_recv_msg(afd, &r, &m, 1000) == 1 && m.type == MSG_PING && m.seq == 5);
    close(cfd); close(afd); close(lfd);

    return TEST_DONE("test_net");
}
