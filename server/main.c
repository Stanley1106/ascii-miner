/* main.c — miner_server: accept loop that fork()s one child per client. */
#include "server.h"

#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "net.h"

static volatile sig_atomic_t g_stop;
static volatile sig_atomic_t g_child_exited;
static long g_forked, g_reaped;

static void on_stop(int sig) { (void)sig; g_stop = 1; }
static void on_child(int sig) { (void)sig; g_child_exited = 1; }

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [options]\n"
        "  -p, --port N           listen port (default %d)\n"
        "  -W, --width N          map width  (%d-%d, default 120)\n"
        "  -H, --height N         map height (%d-%d, default 40)\n"
        "  -s, --seed N           map seed (default: time)\n"
        "  -m, --max-players N    player slots (default %d)\n"
        "  -v, --verbose          more logs (repeatable)\n"
        "      --log-level L      error|warn|info|debug (or env MINER_LOG)\n"
        "  -q, --quiet            no per-second status line\n"
        " robustness switches (for with/without demos):\n"
        "      --no-lock          do not lock digs (ore can be duplicated)\n"
        "      --no-robust        plain mutex (dead owner deadlocks everyone)\n"
        "      --no-reap          never waitpid() children (zombies pile up)\n"
        " test hooks:\n"
        "      --dig-delay-us N   simulated dig work inside the critical section\n"
        "      --crash-at-dig N   the Nth dig SIGKILLs its child inside the lock\n",
        prog, MG_DEFAULT_PORT, MAP_MIN_W, MAP_MAX_W, MAP_MIN_H, MAP_MAX_H, MAX_PLAYERS);
}

static void reap_children(world_t *w)
{
    g_child_exited = 0;
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        g_reaped++;
        if (WIFSIGNALED(status))
            LOG_WARN("child %d killed by signal %d", pid, WTERMSIG(status));
        /* A killed child could not free its player slot; do it here. */
        if (world_release_pid(w, pid))
            LOG_WARN("released player slot of dead child %d", pid);
        LOG_DEBUG("reaped child %d", pid);
    }
}

static void print_status(world_t *w, uint64_t *last_req)
{
    world_stats *st = &w->st;
    uint64_t req = STAT_GET(st->requests);
    printf("[stat] up=%lds online=%d children=%ld req/s=%llu err=%llu "
           "cells=%llu/%d mined=%llu/%llu dup=%llu(+%llu) lockfix=%llu timeout=%llu rejected=%llu\n",
           (long)(time(NULL) - w->start_time), STAT_GET(st->online), g_forked - g_reaped,
           (unsigned long long)(req - *last_req), (unsigned long long)STAT_GET(st->errors),
           (unsigned long long)STAT_GET(st->dug_cells), w->cfg.w * w->cfg.h,
           (unsigned long long)STAT_GET(st->mined_value), (unsigned long long)w->initial_value,
           (unsigned long long)STAT_GET(st->dup_claims), (unsigned long long)STAT_GET(st->dup_value),
           (unsigned long long)STAT_GET(st->lock_recovered),
           (unsigned long long)STAT_GET(st->timeouts), (unsigned long long)STAT_GET(st->rejected));
    fflush(stdout);
    *last_req = req;
}

int main(int argc, char **argv)
{
    server_opts opt = {
        .port = MG_DEFAULT_PORT,
        .world = { .w = 120, .h = 40, .seed = (uint32_t)time(NULL), .max_players = MAX_PLAYERS,
                   .use_lock = 1, .robust = 1 },
        .reap = 1,
    };
    int cli_level = -1, verbose = 0;
    enum { O_LOGLEVEL = 256, O_NOLOCK, O_NOROBUST, O_NOREAP, O_DIGDELAY, O_CRASH };
    static const struct option longopts[] = {
        { "port", required_argument, 0, 'p' },
        { "width", required_argument, 0, 'W' },
        { "height", required_argument, 0, 'H' },
        { "seed", required_argument, 0, 's' },
        { "max-players", required_argument, 0, 'm' },
        { "verbose", no_argument, 0, 'v' },
        { "quiet", no_argument, 0, 'q' },
        { "help", no_argument, 0, 'h' },
        { "log-level", required_argument, 0, O_LOGLEVEL },
        { "no-lock", no_argument, 0, O_NOLOCK },
        { "no-robust", no_argument, 0, O_NOROBUST },
        { "no-reap", no_argument, 0, O_NOREAP },
        { "dig-delay-us", required_argument, 0, O_DIGDELAY },
        { "crash-at-dig", required_argument, 0, O_CRASH },
        { 0, 0, 0, 0 },
    };
    int c;
    while ((c = getopt_long(argc, argv, "p:W:H:s:m:vqh", longopts, NULL)) != -1) {
        switch (c) {
        case 'p': opt.port = (uint16_t)atoi(optarg); break;
        case 'W': opt.world.w = atoi(optarg); break;
        case 'H': opt.world.h = atoi(optarg); break;
        case 's': opt.world.seed = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'm': opt.world.max_players = atoi(optarg); break;
        case 'v': verbose++; break;
        case 'q': opt.quiet = 1; break;
        case O_LOGLEVEL:
            if ((cli_level = mg_log_parse_level(optarg)) < 0) {
                fprintf(stderr, "bad log level: %s\n", optarg);
                return 2;
            }
            break;
        case O_NOLOCK:   opt.world.use_lock = 0; break;
        case O_NOROBUST: opt.world.robust = 0; break;
        case O_NOREAP:   opt.reap = 0; break;
        case O_DIGDELAY: opt.world.dig_delay_us = atoi(optarg); break;
        case O_CRASH:    opt.world.crash_at_dig = (uint32_t)strtoul(optarg, NULL, 10); break;
        default: usage(argv[0]); return c == 'h' ? 0 : 2;
        }
    }
    if (verbose && cli_level < 0)
        cli_level = MG_LOG_INFO + verbose > MG_LOG_DEBUG ? MG_LOG_DEBUG : MG_LOG_INFO + verbose;
    mg_log_init("server", cli_level);

    if (opt.world.w < MAP_MIN_W || opt.world.w > MAP_MAX_W ||
        opt.world.h < MAP_MIN_H || opt.world.h > MAP_MAX_H) {
        fprintf(stderr, "map size must be %d-%d x %d-%d\n", MAP_MIN_W, MAP_MAX_W, MAP_MIN_H, MAP_MAX_H);
        return 2;
    }
    if (opt.world.max_players < 1 || opt.world.max_players > MAX_PLAYERS) {
        fprintf(stderr, "max players must be 1-%d\n", MAX_PLAYERS);
        return 2;
    }

    world_t *w = world_create(&opt.world);
    if (!w) {
        perror("mmap");
        return 1;
    }
    int lfd = mg_tcp_listen(opt.port, 1024);
    if (lfd < 0) {
        LOG_ERROR("listen on port %u: %s", opt.port, strerror(errno));
        return 1;
    }

    struct sigaction sa = { 0 };
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_stop; /* no SA_RESTART: poll() returns EINTR */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    if (opt.reap) {
        sa.sa_handler = on_child;
        sa.sa_flags = SA_NOCLDSTOP;
        sigaction(SIGCHLD, &sa, NULL);
    } else {
        /* SIG_DFL (not SIG_IGN, which would auto-reap): exited children
         * stay as zombies because nobody calls waitpid(). */
        signal(SIGCHLD, SIG_DFL);
        LOG_WARN("robustness OFF: children will not be reaped");
    }
    LOG_INFO("listening on port %u (pid %d) reap=%s", opt.port, getpid(), opt.reap ? "on" : "off");

    pid_t parent = getpid();
    uint64_t last_req = 0;
    long next_stat = mg_now_ms() + 1000;
    while (!g_stop) {
        long wait_ms = next_stat - mg_now_ms();
        struct pollfd pf = { .fd = lfd, .events = POLLIN };
        int pr = poll(&pf, 1, wait_ms > 0 ? (int)wait_ms : 0);
        if (g_child_exited)
            reap_children(w);
        if (pr > 0) {
            int cfd = accept(lfd, NULL, NULL);
            if (cfd < 0) {
                if (errno != EINTR && errno != EAGAIN)
                    LOG_WARN("accept: %s", strerror(errno));
            } else {
                pid_t pid = fork();
                if (pid == 0) {
                    close(lfd);
                    signal(SIGINT, SIG_DFL);
                    signal(SIGTERM, SIG_DFL);
                    signal(SIGCHLD, SIG_DFL);
                    /* Die with the parent so no orphan keeps the shared world. */
                    prctl(PR_SET_PDEATHSIG, SIGTERM);
                    if (getppid() != parent)
                        _exit(0);
                    session_run(cfd, w, &opt);
                }
                if (pid < 0) {
                    LOG_ERROR("fork: %s", strerror(errno));
                    STAT_ADD(w->st.errors, 1);
                } else {
                    g_forked++;
                    STAT_ADD(w->st.conns_total, 1);
                    LOG_DEBUG("forked child %d for fd %d", pid, cfd);
                }
                close(cfd);
            }
        }
        if (mg_now_ms() >= next_stat) {
            if (!opt.quiet)
                print_status(w, &last_req);
            next_stat += 1000;
        }
    }
    LOG_INFO("shutting down");
    close(lfd);
    print_status(w, &last_req);
    return 0;
}
