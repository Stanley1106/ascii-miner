#include "client.h"

#include <string.h>
#include <unistd.h>

#include "log.h"
#include "net.h"

int client_join(const client_opts *o, const char *name, int want_snapshot, mg_reader *r, welcome_t *wel)
{
    static mg_msg m;
    int fd = mg_tcp_connect(o->host, o->port);
    if (fd < 0) {
        LOG_ERROR("cannot connect to %s:%u", o->host, o->port);
        return -1;
    }
    uint8_t hello[1 + MG_NAME_LEN] = { 0 };
    hello[0] = want_snapshot ? HELLO_WANT_SNAPSHOT : 0;
    strncpy((char *)hello + 1, name, MG_NAME_LEN - 1);
    mg_reader_init(r);
    if (mg_send_msg(fd, MSG_HELLO, 1, hello, sizeof hello) < 0)
        goto fail;
    int rc = mg_recv_msg(fd, r, &m, 5000);
    if (rc == 1 && m.type == MSG_WELCOME && m.len >= 10) {
        wel->id = mg_get_u16(m.payload);
        wel->map_w = mg_get_u16(m.payload + 2);
        wel->map_h = mg_get_u16(m.payload + 4);
        wel->x = mg_get_u16(m.payload + 6);
        wel->y = mg_get_u16(m.payload + 8);
        LOG_DEBUG("joined as id %u map %ux%u", wel->id, wel->map_w, wel->map_h);
        return fd;
    }
    if (rc == 1 && m.type == MSG_ERROR && m.len >= 1)
        LOG_ERROR("server refused: %.*s", (int)m.len - 1, (const char *)m.payload + 1);
    else
        LOG_ERROR("handshake failed (rc=%d)", rc);
fail:
    close(fd);
    return -1;
}
