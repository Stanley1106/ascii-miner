#include "world.h"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "game.h"
#include "log.h"

static size_t world_size(int w, int h)
{
    return sizeof(world_t) + (size_t)w * h * sizeof(cell_t);
}

world_t *world_create(const world_cfg *cfg)
{
    size_t sz = world_size(cfg->w, cfg->h);
    /* MAP_SHARED|MAP_ANONYMOUS: inherited by fork(), writes visible to all. */
    world_t *w = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (w == MAP_FAILED)
        return NULL;
    memset(w, 0, sz);
    w->cfg = *cfg;
    if (w->cfg.max_players <= 0 || w->cfg.max_players > MAX_PLAYERS)
        w->cfg.max_players = MAX_PLAYERS;
    w->start_time = time(NULL);

    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_setpshared(&a, PTHREAD_PROCESS_SHARED);
    if (cfg->robust)
        pthread_mutexattr_setrobust(&a, PTHREAD_MUTEX_ROBUST);
    pthread_mutex_init(&w->lock, &a);
    pthread_mutexattr_destroy(&a);

    uint32_t rng = cfg->seed;
    for (int i = 0; i < cfg->w * cfg->h; i++) {
        int ore = mg_ore_roll(&rng);
        w->cells[i].ore = (uint8_t)ore;
        w->initial_value += mg_ores[ore].value;
        if (ore != ORE_NONE)
            w->initial_ores++;
    }
    LOG_INFO("world %dx%d seed=%u ores=%u value=%llu lock=%s robust=%s", cfg->w, cfg->h,
             cfg->seed, w->initial_ores, (unsigned long long)w->initial_value,
             cfg->use_lock ? "on" : "off", cfg->robust ? "on" : "off");
    return w;
}

void world_destroy(world_t *w)
{
    munmap(w, world_size(w->cfg.w, w->cfg.h));
}

static void bump(world_t *w) { __atomic_add_fetch(&w->version, 1, __ATOMIC_RELEASE); }

int world_join(world_t *w, pid_t pid, const char *name, uint32_t *rng)
{
    for (int i = 0; i < w->cfg.max_players; i++) {
        int32_t expect = SLOT_FREE;
        if (!__atomic_compare_exchange_n(&w->players[i].state, &expect, SLOT_JOINING, 0,
                                         __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            continue;
        player_t *p = &w->players[i];
        p->pid = pid;
        p->x = (uint16_t)(mg_rand(rng) % w->cfg.w);
        p->y = (uint16_t)(mg_rand(rng) % w->cfg.h);
        memset(p->name, 0, sizeof p->name);
        strncpy(p->name, name, sizeof p->name - 1);
        __atomic_store_n(&p->state, SLOT_ACTIVE, __ATOMIC_RELEASE);
        STAT_ADD(w->st.online, 1);
        bump(w);
        return i;
    }
    return -1;
}

static int release_slot(world_t *w, int slot)
{
    int32_t expect = SLOT_ACTIVE;
    if (!__atomic_compare_exchange_n(&w->players[slot].state, &expect, SLOT_FREE, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        return 0;
    STAT_ADD(w->st.online, -1);
    bump(w);
    return 1;
}

void world_leave(world_t *w, int slot)
{
    if (slot >= 0 && slot < w->cfg.max_players)
        release_slot(w, slot);
}

int world_release_pid(world_t *w, pid_t pid)
{
    for (int i = 0; i < w->cfg.max_players; i++)
        if (__atomic_load_n(&w->players[i].state, __ATOMIC_ACQUIRE) == SLOT_ACTIVE &&
            w->players[i].pid == pid)
            return release_slot(w, i);
    return 0;
}

void world_move(world_t *w, int slot, int dir)
{
    player_t *p = &w->players[slot];
    int dx, dy;
    mg_dir_delta(dir, &dx, &dy);
    int nx = p->x + dx, ny = p->y + dy;
    if (nx < 0 || ny < 0 || nx >= w->cfg.w || ny >= w->cfg.h)
        return;
    p->x = (uint16_t)nx;
    p->y = (uint16_t)ny;
    bump(w);
}

static int world_lock(world_t *w)
{
    if (!w->cfg.use_lock)
        return 0;
    int rc = pthread_mutex_lock(&w->lock);
    if (rc == EOWNERDEAD) {
        /* The previous owner died inside the critical section. The cell
         * update happens after all checks, so state is still consistent:
         * mark the mutex usable again and carry on. */
        LOG_WARN("world lock owner died; recovering mutex (EOWNERDEAD)");
        pthread_mutex_consistent(&w->lock);
        STAT_ADD(w->st.lock_recovered, 1);
        return 0;
    }
    if (rc != 0)
        LOG_ERROR("pthread_mutex_lock: %s", strerror(rc));
    return rc;
}

static void world_unlock(world_t *w)
{
    if (w->cfg.use_lock)
        pthread_mutex_unlock(&w->lock);
}

int world_dig(world_t *w, int slot, int *x, int *y, int *ore)
{
    player_t *p = &w->players[slot];
    *x = p->x;
    *y = p->y;
    *ore = ORE_NONE;
    cell_t *c = &w->cells[*y * w->cfg.w + *x];
    STAT_ADD(w->st.digs, 1);

    if (world_lock(w) != 0)
        return -1;

    /* ---- critical section: check-then-claim ---- */
    if (c->dug) {
        world_unlock(w);
        return DIG_ALREADY;
    }
    int found = c->ore;
    if (w->cfg.dig_delay_us > 0)
        usleep(w->cfg.dig_delay_us);
    if (w->cfg.crash_at_dig &&
        __atomic_add_fetch(&w->crash_counter, 1, __ATOMIC_RELAXED) == w->cfg.crash_at_dig) {
        LOG_WARN("crash test: process dies while holding the world lock");
        kill(getpid(), SIGKILL);
    }
    c->dug = 1;
    c->owner = (uint16_t)slot;
    world_unlock(w);
    /* ---- end critical section ---- */

    /* Measurement only: atomically count claimers of this cell. Without the
     * lock several processes can pass the `dug` check and all take the ore. */
    uint32_t claims = __atomic_add_fetch(&c->claims, 1, __ATOMIC_RELAXED);
    uint32_t value = mg_ores[found].value;
    if (claims > 1) {
        STAT_ADD(w->st.dup_claims, 1);
        STAT_ADD(w->st.dup_value, value);
        LOG_DEBUG("duplicate claim on (%d,%d) by slot %d", *x, *y, slot);
    } else {
        STAT_ADD(w->st.dug_cells, 1);
    }
    STAT_ADD(w->st.mined_value, value);
    bump(w);
    *ore = found;
    return found ? DIG_FOUND : DIG_NOTHING;
}

size_t world_snapshot(world_t *w, uint8_t *out, size_t cap)
{
    int W = w->cfg.w, H = w->cfg.h;
    size_t bitmap = ((size_t)W * H + 7) / 8;
    size_t need = 10 + (size_t)w->cfg.max_players * 6 + bitmap;
    if (cap < need)
        return 0;
    mg_put_u32(out, __atomic_load_n(&w->version, __ATOMIC_ACQUIRE));
    mg_put_u16(out + 4, (uint16_t)W);
    mg_put_u16(out + 6, (uint16_t)H);
    size_t off = 10;
    uint16_t n = 0;
    for (int i = 0; i < w->cfg.max_players; i++) {
        player_t *p = &w->players[i];
        if (__atomic_load_n(&p->state, __ATOMIC_ACQUIRE) != SLOT_ACTIVE)
            continue;
        mg_put_u16(out + off, (uint16_t)i);
        mg_put_u16(out + off + 2, p->x);
        mg_put_u16(out + off + 4, p->y);
        off += 6;
        n++;
    }
    mg_put_u16(out + 8, n);
    memset(out + off, 0, bitmap);
    for (int i = 0; i < W * H; i++)
        if (w->cells[i].dug)
            out[off + i / 8] |= (uint8_t)(1u << (i % 8));
    return off + bitmap;
}
