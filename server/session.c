/* session.c — one forked child process serving one client connection. */
#include "server.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "game.h"
#include "log.h"
#include "net.h"
#include "sysinfo.h"

#define TICK_MS 100
#define HELLO_TIMEOUT_MS 5000

typedef struct {
    int fd;
    world_t *w;
    const server_opts *opt;
    int slot;
    int want_snapshot;
    uint32_t inv[ORE_COUNT]; /* per-connection inventory, lives in this child only */
    uint32_t total_value;
    uint32_t digs;
    uint32_t sent_version;   /* world version of the last snapshot pushed */
    long last_snapshot_ms;
    int snapshot_sent;
    long last_recv_ms;       /* for the idle timeout */
} session_t;

static mg_reader g_reader;
static mg_msg g_msg;

static void send_error(session_t *s, uint32_t seq, uint8_t code, const char *text)
{
    uint8_t buf[128];
    size_t n = strlen(text);
    if (n > sizeof buf - 1)
        n = sizeof buf - 1;
    buf[0] = code;
    memcpy(buf + 1, text, n);
    mg_send_msg(s->fd, MSG_ERROR, seq, buf, (uint32_t)(n + 1));
}

static int do_move(session_t *s, const mg_msg *m)
{
    if (m->len < 1 || m->payload[0] > DIR_RIGHT) {
        STAT_ADD(s->w->st.errors, 1);
        send_error(s, m->seq, ERR_BAD_REQUEST, "bad direction");
        return 0;
    }
    world_move(s->w, s->slot, m->payload[0]);
    uint8_t out[4];
    mg_put_u16(out, s->w->players[s->slot].x);
    mg_put_u16(out + 2, s->w->players[s->slot].y);
    return mg_send_msg(s->fd, MSG_POS, m->seq, out, sizeof out);
}

static int do_dig(session_t *s, const mg_msg *m)
{
    int x, y, ore;
    int st = world_dig(s->w, s->slot, &x, &y, &ore);
    if (st < 0) {
        STAT_ADD(s->w->st.errors, 1);
        send_error(s, m->seq, ERR_BAD_REQUEST, "dig failed");
        return 0;
    }
    s->digs++;
    if (st == DIG_FOUND) {
        s->inv[ore]++;
        s->total_value += mg_ores[ore].value;
    }
    LOG_DEBUG("slot %d dig (%d,%d) -> status %d ore %s", s->slot, x, y, st, mg_ores[ore].name);
    uint8_t out[14];
    mg_put_u16(out, (uint16_t)x);
    mg_put_u16(out + 2, (uint16_t)y);
    out[4] = (uint8_t)st;
    out[5] = (uint8_t)ore;
    mg_put_u32(out + 6, mg_ores[ore].value);
    mg_put_u32(out + 10, s->total_value);
    return mg_send_msg(s->fd, MSG_DIG_RESULT, m->seq, out, sizeof out);
}

static int do_inventory(session_t *s, const mg_msg *m)
{
    uint8_t out[4 * ORE_COUNT + 8];
    for (int i = 0; i < ORE_COUNT; i++)
        mg_put_u32(out + 4 * i, s->inv[i]);
    mg_put_u32(out + 4 * ORE_COUNT, s->total_value);
    mg_put_u32(out + 4 * ORE_COUNT + 4, s->digs);
    return mg_send_msg(s->fd, MSG_INV_RESP, m->seq, out, sizeof out);
}

static int do_sysinfo(session_t *s, const mg_msg *m)
{
    char text[2048];
    size_t n = build_sysinfo(s->w, text, sizeof text);
    return mg_send_msg(s->fd, MSG_SYSINFO_RESP, m->seq, text, (uint32_t)n);
}

/* Returns 0 to keep going, 1 to close the session normally, -1 on I/O error. */
static int handle(session_t *s, const mg_msg *m)
{
    STAT_ADD(s->w->st.requests, 1);
    switch (m->type) {
    case MSG_MOVE:    return do_move(s, m) < 0 ? -1 : 0;
    case MSG_DIG:     return do_dig(s, m) < 0 ? -1 : 0;
    case MSG_INV_REQ: return do_inventory(s, m) < 0 ? -1 : 0;
    case MSG_SYSINFO_REQ: return do_sysinfo(s, m) < 0 ? -1 : 0;
    case MSG_PING:    return mg_send_msg(s->fd, MSG_PONG, m->seq, NULL, 0) < 0 ? -1 : 0;
    case MSG_BYE:     return 1;
    default:
        STAT_ADD(s->w->st.errors, 1);
        LOG_WARN("slot %d: unexpected message type 0x%02x", s->slot, m->type);
        send_error(s, m->seq, ERR_BAD_REQUEST, "unexpected message type");
        return 0;
    }
}

/* Push the world view when it changed, at most once per tick. */
static int maybe_snapshot(session_t *s)
{
    static uint8_t buf[MG_MAX_PAYLOAD];
    if (!s->want_snapshot)
        return 0;
    long now = mg_now_ms();
    uint32_t v = __atomic_load_n(&s->w->version, __ATOMIC_ACQUIRE);
    if (s->snapshot_sent && (v == s->sent_version || now - s->last_snapshot_ms < TICK_MS))
        return 0;
    size_t n = world_snapshot(s->w, buf, sizeof buf);
    s->sent_version = v;
    s->last_snapshot_ms = now;
    s->snapshot_sent = 1;
    return mg_send_msg(s->fd, MSG_SNAPSHOT, 0, buf, (uint32_t)n);
}

/* Robustness #3 OFF (--naive-recv): the classic bug of assuming that one
 * recv() returns exactly one complete frame. A frame split across several
 * TCP segments is parsed from a half-filled buffer, and extra frames that
 * arrive together are silently dropped.
 * 1 ok, 0 closed, -1 error, -3 protocol error. */
static int naive_recv(int fd, mg_msg *m)
{
    static uint8_t buf[MG_HDR_LEN + MG_MAX_PAYLOAD];
    memset(buf, 0, MG_HDR_LEN);
    ssize_t n = recv(fd, buf, sizeof buf, 0);
    if (n <= 0)
        return n == 0 ? 0 : -1;
    LOG_DEBUG("naive recv got %zd bytes", n);
    if (mg_decode_header(buf, &m->type, &m->seq, &m->len) < 0)
        return -3;
    memcpy(m->payload, buf + MG_HDR_LEN, m->len);
    return 1;
}

/* Robust path: buffer bytes and dispatch every complete frame.
 * Returns 1 when the session should end. */
static int read_framed(session_t *s, const char **reason)
{
    int n = mg_reader_fill(&g_reader, s->fd);
    if (n == 0) {
        *reason = "connection closed by peer";
        return 1;
    }
    if (n < 0) {
        *reason = strerror(errno);
        return 1;
    }
    int rc;
    while ((rc = mg_reader_next(&g_reader, &g_msg)) == 1) {
        int h = handle(s, &g_msg);
        if (h != 0) {
            *reason = h < 0 ? "send failed" : "client quit";
            return 1;
        }
    }
    if (rc < 0) {
        STAT_ADD(s->w->st.errors, 1);
        send_error(s, 0, ERR_PROTOCOL, "malformed frame");
        *reason = "protocol error";
        return 1;
    }
    return 0;
}

static int read_naive(session_t *s, const char **reason)
{
    int rc = naive_recv(s->fd, &g_msg);
    if (rc == 0 || rc == -1) {
        *reason = rc == 0 ? "connection closed by peer" : strerror(errno);
        return 1;
    }
    if (rc == -3) {
        STAT_ADD(s->w->st.errors, 1);
        send_error(s, 0, ERR_PROTOCOL, "malformed frame");
        *reason = "protocol error (naive recv)";
        return 1;
    }
    int h = handle(s, &g_msg);
    if (h != 0) {
        *reason = h < 0 ? "send failed" : "client quit";
        return 1;
    }
    return 0;
}

static int handshake(session_t *s)
{
    int rc = s->opt->naive_recv ? naive_recv(s->fd, &g_msg)
                                : mg_recv_msg(s->fd, &g_reader, &g_msg, HELLO_TIMEOUT_MS);
    if (rc != 1 || g_msg.type != MSG_HELLO || g_msg.len < 1 + MG_NAME_LEN) {
        LOG_WARN("handshake failed (rc=%d type=0x%02x)", rc, rc == 1 ? g_msg.type : 0);
        STAT_ADD(s->w->st.errors, 1);
        send_error(s, 0, ERR_PROTOCOL, "expected HELLO");
        return -1;
    }
    s->want_snapshot = g_msg.payload[0] & HELLO_WANT_SNAPSHOT;
    char name[MG_NAME_LEN];
    for (int i = 0; i < MG_NAME_LEN; i++) {
        char c = (char)g_msg.payload[1 + i];
        name[i] = (c == 0 || (c >= 32 && c < 127)) ? c : '?';
    }
    name[MG_NAME_LEN - 1] = '\0';
    if (!name[0])
        strcpy(name, "anon");

    uint32_t rng = (uint32_t)getpid() ^ (uint32_t)time(NULL);
    s->slot = world_join(s->w, getpid(), name, &rng);
    if (s->slot < 0) {
        LOG_WARN("server full, rejecting %s", name);
        STAT_ADD(s->w->st.rejected, 1);
        send_error(s, g_msg.seq, ERR_SERVER_FULL, "server full");
        return -1;
    }
    player_t *p = &s->w->players[s->slot];
    uint8_t out[10];
    mg_put_u16(out, (uint16_t)s->slot);
    mg_put_u16(out + 2, (uint16_t)s->w->cfg.w);
    mg_put_u16(out + 4, (uint16_t)s->w->cfg.h);
    mg_put_u16(out + 6, p->x);
    mg_put_u16(out + 8, p->y);
    LOG_INFO("player '%s' joined as slot %d at (%u,%u)", name, s->slot, p->x, p->y);
    return mg_send_msg(s->fd, MSG_WELCOME, g_msg.seq, out, sizeof out);
}

void session_run(int fd, world_t *w, const server_opts *opt)
{
    session_t s = { .fd = fd, .w = w, .opt = opt, .slot = -1 };
    mg_reader_init(&g_reader);
    if (handshake(&s) < 0)
        goto out;
    s.last_recv_ms = mg_now_ms();

    const char *reason = "client quit";
    for (;;) {
        struct pollfd pf = { .fd = fd, .events = POLLIN };
        int pr = poll(&pf, 1, TICK_MS);
        if (pr < 0 && errno != EINTR) {
            reason = "poll error";
            break;
        }
        if (pr > 0) {
            s.last_recv_ms = mg_now_ms();
            int stop = opt->naive_recv ? read_naive(&s, &reason) : read_framed(&s, &reason);
            if (stop)
                break;
        }
        /* Robustness #4: a frozen/vanished client (no FIN ever arrives)
         * would otherwise hold this process and its slot forever. */
        if (opt->idle_timeout > 0 && mg_now_ms() - s.last_recv_ms > opt->idle_timeout * 1000L) {
            STAT_ADD(w->st.timeouts, 1);
            send_error(&s, 0, ERR_TIMEOUT, "idle timeout");
            reason = "idle timeout";
            break;
        }
        if (maybe_snapshot(&s) < 0) {
            reason = "send failed";
            break;
        }
    }
    LOG_INFO("slot %d leaving: %s (digs=%u value=%u)", s.slot, reason, s.digs, s.total_value);
out:
    world_leave(w, s.slot);
    close(fd);
    _exit(0);
}
