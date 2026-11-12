/*
 * world.h — game state shared by all server processes.
 *
 * The whole world_t lives in one anonymous MAP_SHARED mapping created by
 * the parent before any fork(), so every child sees the same memory.
 * Synchronisation:
 *   - dig (check-then-claim a cell) is protected by a process-shared,
 *     optionally robust, pthread mutex stored inside the mapping
 *     (no threads are created — it is purely an inter-process lock);
 *   - counters use __atomic builtins;
 *   - player slots are claimed with compare-and-swap.
 */
#ifndef MG_WORLD_H
#define MG_WORLD_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#include "protocol.h"

#define MAX_PLAYERS 512
#define MAP_MIN_W 4
#define MAP_MIN_H 4
#define MAP_MAX_W 200
#define MAP_MAX_H 100

#define STAT_ADD(field, v) __atomic_add_fetch(&(field), (v), __ATOMIC_RELAXED)
#define STAT_GET(field)    __atomic_load_n(&(field), __ATOMIC_RELAXED)

typedef struct {
    uint8_t  ore;    /* hidden content, fixed at map generation */
    uint8_t  dug;
    uint16_t owner;  /* slot of the first digger */
    uint32_t claims; /* how many diggers claimed it (>1 = duplicated ore) */
} cell_t;

enum { SLOT_FREE = 0, SLOT_ACTIVE = 1, SLOT_JOINING = 2 };

typedef struct {
    int32_t  state;
    int32_t  pid;
    uint16_t x, y;
    char     name[MG_NAME_LEN];
} player_t;

typedef struct {
    uint64_t requests, errors, digs, dug_cells, mined_value;
    uint64_t dup_claims, dup_value, conns_total, rejected, lock_recovered, timeouts;
    int32_t  online;
} world_stats;

typedef struct {
    int w, h;
    uint32_t seed;
    int max_players;
    int use_lock;       /* robustness #1: serialize digs */
    int robust;         /* robustness #5: recover lock from a dead owner */
    int dig_delay_us;   /* simulated dig work; widens the race window */
    uint32_t crash_at_dig; /* test hook: the Nth dig kills its process inside the lock */
} world_cfg;

typedef struct {
    world_cfg cfg;
    pthread_mutex_t lock;
    uint32_t version;       /* bumped on any visible change */
    uint32_t crash_counter;
    uint64_t initial_value; /* sum of all ore values on the map */
    uint32_t initial_ores;
    time_t   start_time;
    world_stats st;
    player_t players[MAX_PLAYERS];
    cell_t   cells[];
} world_t;

world_t *world_create(const world_cfg *cfg);
void     world_destroy(world_t *w);

/* Returns slot index, or -1 if the server is full. */
int  world_join(world_t *w, pid_t pid, const char *name, uint32_t *rng);
/* Free a slot (child on exit). Safe to race with world_release_pid. */
void world_leave(world_t *w, int slot);
/* Parent-side cleanup after reaping a child that could not clean up. */
int  world_release_pid(world_t *w, pid_t pid);

void world_move(world_t *w, int slot, int dir);
/* Dig the cell under player `slot`. Returns enum mg_dig_status or -1. */
int  world_dig(world_t *w, int slot, int *x, int *y, int *ore);

/* Build a MSG_SNAPSHOT payload. Returns its length. */
size_t world_snapshot(world_t *w, uint8_t *out, size_t cap);

#endif
