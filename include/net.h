/* net.h — TCP helpers shared by client and server. */
#ifndef MG_NET_H
#define MG_NET_H

#include <stdint.h>
#include <sys/types.h>
#include "protocol.h"

int mg_tcp_listen(uint16_t port, int backlog);
int mg_tcp_connect(const char *host, uint16_t port);

/* Loops until all n bytes are sent (handles partial send, EINTR, EAGAIN).
 * Returns n on success, -1 on error. Never raises SIGPIPE. */
ssize_t mg_send_all(int fd, const void *buf, size_t n);

/* Encode header + payload and send it as a single frame. 0 ok, -1 error. */
int mg_send_msg(int fd, uint8_t type, uint32_t seq, const void *payload, uint32_t len);

/* Test/demo knob: when > 0, every send is split into 1-byte writes with
 * this many microseconds between them (forces partial recv on the peer). */
void mg_set_send_fragment(int delay_us);

/* Blocking receive of one frame using reader r.
 * 1 ok, 0 peer closed, -1 socket error, -2 timeout, -3 protocol error. */
int mg_recv_msg(int fd, mg_reader *r, mg_msg *m, int timeout_ms);

#endif
