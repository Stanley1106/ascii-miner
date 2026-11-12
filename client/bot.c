/* bot.c — N automated players in one process, multiplexed with poll(). */
#include "client.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "game.h"
#include "log.h"
#include "net.h"

#define STALL_MS 2000

typedef struct {
    int fd;
    int alive;
    int busy;          /* waiting for the reply to our last request */
    int last_was_dig;
    long next_ms;
    long sent_ms;
    uint32_t seq;
    mg_reader rd;
} bot_t;

typedef struct {
    unsigned long sent, recv, errors, lost, value, found;
} bot_stats;

static volatile sig_atomic_t g_stop;
static void on_sigint(int s) { (void)s; g_stop = 1; }

static void kill_bot(bot_t *b, int *alive)
{
    close(b->fd);
    b->alive = 0;
    (*alive)--;
}

static void act(bot_t *b, long now, int interval)
{
    int r = rand() % 100;
    uint8_t dir = (uint8_t)(rand() % 4);
    int ok;
    b->seq++;
    if (!b->last_was_dig) {
        ok = mg_send_msg(b->fd, MSG_DIG, b->seq, NULL, 0);
        b->last_was_dig = 1;
    } else if (r < 4) {
        ok = mg_send_msg(b->fd, MSG_INV_REQ, b->seq, NULL, 0);
    } else if (r < 5) {
        ok = mg_send_msg(b->fd, MSG_SYSINFO_REQ, b->seq, NULL, 0);
    } else {
        ok = mg_send_msg(b->fd, MSG_MOVE, b->seq, &dir, 1);
        b->last_was_dig = 0;
    }
    (void)ok;
    b->busy = 1;
    b->sent_ms = now;
    b->next_ms = now + interval;
}

/* Returns -1 if the connection must be dropped. */
static int on_msg(bot_t *b, const mg_msg *m, bot_stats *st)
{
    switch (m->type) {
    case MSG_DIG_RESULT:
        if (m->len >= 14 && m->payload[4] == DIG_FOUND) {
            st->value += mg_get_u32(m->payload + 6);
            st->found++;
        }
        break;
    case MSG_ERROR:
        st->errors++;
        LOG_WARN("server error %u: %.*s", m->len ? m->payload[0] : 0,
                 m->len > 1 ? (int)m->len - 1 : 0, (const char *)m->payload + 1);
        if (m->len && (m->payload[0] == ERR_TIMEOUT || m->payload[0] == ERR_PROTOCOL))
            return -1;
        break;
    case MSG_SNAPSHOT:
        return 0; /* unsolicited, not a reply */
    default:
        break;
    }
    if (b->busy) {
        b->busy = 0;
        st->recv++;
    }
    return 0;
}

int bot_run(const client_opts *o)
{
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    signal(SIGPIPE, SIG_IGN);
    srand((unsigned)getpid());
    mg_set_send_fragment(o->fragment_us);

    bot_t *bots = calloc((size_t)o->count, sizeof *bots);
    struct pollfd *pfd = calloc((size_t)o->count, sizeof *pfd);
    int *idx = calloc((size_t)o->count, sizeof *idx);
    if (!bots || !pfd || !idx) {
        perror("calloc");
        return 1;
    }
    static mg_msg m;
    bot_stats st = { 0 };
    int alive = 0;
    long t0 = mg_now_ms();
    for (int i = 0; i < o->count && !g_stop; i++) {
        char name[MG_NAME_LEN];
        snprintf(name, sizeof name, "%s%d", o->name, i);
        welcome_t wel;
        bots[i].fd = client_join(o, name, 0, &bots[i].rd, &wel);
        if (bots[i].fd < 0) {
            st.errors++;
            continue;
        }
        bots[i].alive = 1;
        bots[i].next_ms = mg_now_ms() + rand() % (o->interval_ms + 1);
        alive++;
    }
    printf("[bot] connected %d/%d in %ld ms\n", alive, o->count, mg_now_ms() - t0);
    fflush(stdout);

    long start = mg_now_ms(), next_report = start + 1000;
    unsigned long last_recv = 0;
    while (!g_stop && alive > 0 && (o->duration == 0 || mg_now_ms() - start < o->duration * 1000L)) {
        int n = 0;
        for (int i = 0; i < o->count; i++)
            if (bots[i].alive) {
                pfd[n] = (struct pollfd){ .fd = bots[i].fd, .events = POLLIN };
                idx[n++] = i;
            }
        if (poll(pfd, (nfds_t)n, 10) < 0 && errno != EINTR)
            break;
        long now = mg_now_ms();
        for (int k = 0; k < n; k++) {
            if (!pfd[k].revents)
                continue;
            bot_t *b = &bots[idx[k]];
            int r = mg_reader_fill(&b->rd, b->fd);
            if (r <= 0) {
                LOG_WARN("bot %d: connection lost (%s)", idx[k], r == 0 ? "closed" : strerror(errno));
                st.lost++;
                kill_bot(b, &alive);
                continue;
            }
            int rc;
            while ((rc = mg_reader_next(&b->rd, &m)) == 1)
                if (on_msg(b, &m, &st) < 0)
                    break;
            if (rc < 0 || (rc == 1 && b->alive)) {
                if (rc < 0)
                    st.errors++;
                st.lost++;
                kill_bot(b, &alive);
            }
        }
        int stalled = 0;
        for (int i = 0; i < o->count; i++) {
            bot_t *b = &bots[i];
            if (!b->alive)
                continue;
            if (b->busy) {
                stalled += now - b->sent_ms > STALL_MS;
                continue;
            }
            if (now >= b->next_ms) {
                act(b, now, o->interval_ms);
                st.sent++;
            }
        }
        if (now >= next_report) {
            printf("[bot] t=%lds alive=%d/%d sent=%lu recv=%lu rps=%lu err=%lu lost=%lu stalled=%d "
                   "found=%lu value=%lu\n",
                   (now - start) / 1000, alive, o->count, st.sent, st.recv, st.recv - last_recv,
                   st.errors, st.lost, stalled, st.found, st.value);
            fflush(stdout);
            last_recv = st.recv;
            next_report += 1000;
        }
    }
    for (int i = 0; i < o->count; i++)
        if (bots[i].alive) {
            mg_send_msg(bots[i].fd, MSG_BYE, ++bots[i].seq, NULL, 0);
            close(bots[i].fd);
        }
    printf("[bot] done: bots=%d sent=%lu recv=%lu err=%lu lost=%lu found=%lu value=%lu\n",
           o->count, st.sent, st.recv, st.errors, st.lost, st.found, st.value);
    free(bots);
    free(pfd);
    free(idx);
    return st.errors || st.lost ? 1 : 0;
}
