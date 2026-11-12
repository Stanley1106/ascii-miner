#include "protocol.h"
#include "log.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>

void mg_encode_header(uint8_t out[MG_HDR_LEN], uint8_t type, uint32_t seq, uint32_t len)
{
    out[0] = MG_MAGIC0;
    out[1] = MG_MAGIC1;
    out[2] = MG_PROTO_VERSION;
    out[3] = type;
    mg_put_u32(out + 4, seq);
    mg_put_u32(out + 8, len);
}

int mg_decode_header(const uint8_t in[MG_HDR_LEN], uint8_t *type, uint32_t *seq, uint32_t *len)
{
    if (in[0] != MG_MAGIC0 || in[1] != MG_MAGIC1) {
        LOG_WARN("bad magic %02x %02x", in[0], in[1]);
        return -1;
    }
    if (in[2] != MG_PROTO_VERSION) {
        LOG_WARN("unsupported protocol version %u", in[2]);
        return -1;
    }
    uint32_t l = mg_get_u32(in + 8);
    if (l > MG_MAX_PAYLOAD) {
        LOG_WARN("payload length %u exceeds limit %u", l, MG_MAX_PAYLOAD);
        return -1;
    }
    *type = in[3];
    *seq = mg_get_u32(in + 4);
    *len = l;
    return 0;
}

void mg_reader_init(mg_reader *r) { r->len = 0; }

int mg_reader_push(mg_reader *r, const void *data, size_t n)
{
    if (n > sizeof r->buf - r->len)
        return -1;
    memcpy(r->buf + r->len, data, n);
    r->len += n;
    return 0;
}

int mg_reader_fill(mg_reader *r, int fd)
{
    size_t room = sizeof r->buf - r->len;
    if (room == 0) {
        errno = ENOBUFS;
        return -1;
    }
    for (;;) {
        ssize_t n = recv(fd, r->buf + r->len, room, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n > 0) {
            r->len += (size_t)n;
            LOG_DEBUG("recv %zd bytes on fd %d (buffered %zu)", n, fd, r->len);
        }
        return (int)n;
    }
}

int mg_reader_next(mg_reader *r, mg_msg *m)
{
    if (r->len < MG_HDR_LEN)
        return 0;
    uint32_t len;
    if (mg_decode_header(r->buf, &m->type, &m->seq, &len) < 0)
        return -1;
    size_t total = MG_HDR_LEN + (size_t)len;
    if (r->len < total)
        return 0;
    m->len = len;
    memcpy(m->payload, r->buf + MG_HDR_LEN, len);
    memmove(r->buf, r->buf + total, r->len - total);
    r->len -= total;
    LOG_DEBUG("frame %s seq=%u len=%u", mg_type_name(m->type), m->seq, m->len);
    return 1;
}

const char *mg_type_name(uint8_t type)
{
    switch (type) {
    case MSG_HELLO:        return "HELLO";
    case MSG_WELCOME:      return "WELCOME";
    case MSG_MOVE:         return "MOVE";
    case MSG_POS:          return "POS";
    case MSG_DIG:          return "DIG";
    case MSG_DIG_RESULT:   return "DIG_RESULT";
    case MSG_INV_REQ:      return "INV_REQ";
    case MSG_INV_RESP:     return "INV_RESP";
    case MSG_SYSINFO_REQ:  return "SYSINFO_REQ";
    case MSG_SYSINFO_RESP: return "SYSINFO_RESP";
    case MSG_SNAPSHOT:     return "SNAPSHOT";
    case MSG_PING:         return "PING";
    case MSG_PONG:         return "PONG";
    case MSG_BYE:          return "BYE";
    case MSG_ERROR:        return "ERROR";
    default:               return "UNKNOWN";
    }
}
