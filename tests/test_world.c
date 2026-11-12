/* World tests run real fork()ed processes against the shared mapping. */
#include "game.h"
#include "log.h"
#include "test.h"
#include "world.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

static world_t *mk(int w, int h, int lock, int robust, int delay, uint32_t crash)
{
    world_cfg c = { .w = w, .h = h, .seed = 7, .max_players = 64, .use_lock = lock,
                    .robust = robust, .dig_delay_us = delay, .crash_at_dig = crash };
    return world_create(&c);
}

/* N processes each try to dig every cell; returns duplicate claims. */
static uint64_t race(int lock)
{
    world_t *w = mk(4, 4, lock, 1, 2000, 0);
    const int N = 8;
    for (int k = 0; k < N; k++) {
        if (fork() == 0) {
            uint32_t rng = 100 + k;
            int s = world_join(w, getpid(), "racer", &rng);
            for (int i = 0; i < 16; i++) {
                int x, y, ore;
                w->players[s].x = i % 4;
                w->players[s].y = i / 4;
                world_dig(w, s, &x, &y, &ore);
            }
            _exit(0);
        }
    }
    while (wait(NULL) > 0) {}
    uint64_t dup = w->st.dup_claims;
    if (lock) {
        CHECK(w->st.mined_value == w->initial_value);
        CHECK(w->st.dug_cells == 16);
    }
    world_destroy(w);
    return dup;
}

int main(void)
{
    mg_log_level = -1;

    /* generation: initial value equals sum of hidden ores */
    world_t *w = mk(20, 10, 1, 1, 0, 0);
    uint64_t sum = 0;
    for (int i = 0; i < 200; i++)
        sum += mg_ores[w->cells[i].ore].value;
    CHECK(w->initial_value == sum && w->initial_value > 0);

    /* join / move / dig / already dug / leave */
    uint32_t rng = 1;
    int s = world_join(w, getpid(), "alice", &rng);
    CHECK(s == 0 && w->st.online == 1);
    w->players[s].x = 0; w->players[s].y = 0;
    world_move(w, s, DIR_LEFT); /* blocked by edge */
    CHECK(w->players[s].x == 0);
    world_move(w, s, DIR_RIGHT);
    CHECK(w->players[s].x == 1);
    int x, y, ore;
    int st1 = world_dig(w, s, &x, &y, &ore);
    CHECK(x == 1 && y == 0 && (st1 == DIG_FOUND || st1 == DIG_NOTHING));
    CHECK(ore == w->cells[1].ore);
    CHECK(world_dig(w, s, &x, &y, &ore) == DIG_ALREADY && ore == ORE_NONE);

    /* snapshot: 1 player and cell 1 marked dug */
    uint8_t snap[MG_MAX_PAYLOAD];
    size_t n = world_snapshot(w, snap, sizeof snap);
    CHECK(n == 10 + 6 + (200 + 7) / 8);
    CHECK(mg_get_u16(snap + 4) == 20 && mg_get_u16(snap + 8) == 1);
    CHECK(mg_get_u16(snap + 12) == 1 && (snap[16] & 0x02) && !(snap[16] & 0x01));

    /* leave is idempotent across child and parent cleanup paths */
    CHECK(world_release_pid(w, getpid()) == 1 && w->st.online == 0);
    world_leave(w, s);
    CHECK(w->st.online == 0);

    /* server full */
    world_t *small = NULL;
    world_cfg c = { .w = 8, .h = 8, .seed = 1, .max_players = 2, .use_lock = 1, .robust = 1 };
    small = world_create(&c);
    CHECK(world_join(small, 1, "a", &rng) == 0);
    CHECK(world_join(small, 2, "b", &rng) == 1);
    CHECK(world_join(small, 3, "c", &rng) == -1);
    world_destroy(small);
    world_destroy(w);

    /* robustness #1: without the lock ore gets duplicated, with it never */
    CHECK(race(1) == 0);
    CHECK(race(0) > 0);

    /* robustness #5: a process dies holding the lock */
    w = mk(8, 8, 1, 1, 0, 1); /* robust: the next locker recovers */
    if (fork() == 0) {
        int s2 = world_join(w, getpid(), "victim", &rng);
        world_dig(w, s2, &x, &y, &ore); /* SIGKILLs itself inside the lock */
        _exit(0);
    }
    int status;
    wait(&status);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    s = world_join(w, getpid(), "next", &rng);
    w->players[s].x = 7; w->players[s].y = 7;
    CHECK(world_dig(w, s, &x, &y, &ore) != -1);
    CHECK(w->st.lock_recovered == 1);
    world_destroy(w);

    w = mk(8, 8, 1, 0, 0, 1); /* not robust: the next locker hangs forever */
    if (fork() == 0) {
        int s2 = world_join(w, getpid(), "victim", &rng);
        world_dig(w, s2, &x, &y, &ore);
        _exit(0);
    }
    wait(&status);
    if (fork() == 0) {
        alarm(1); /* SIGALRM proves we were stuck in pthread_mutex_lock */
        int s2 = world_join(w, getpid(), "next", &rng);
        world_dig(w, s2, &x, &y, &ore);
        _exit(0);
    }
    wait(&status);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGALRM);
    world_destroy(w);

    return TEST_DONE("test_world");
}
