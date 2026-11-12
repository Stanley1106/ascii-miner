/* game.h — game rules shared by client (names/values for display) and server. */
#ifndef MG_GAME_H
#define MG_GAME_H

#include <stdint.h>

enum mg_ore { ORE_NONE = 0, ORE_COPPER, ORE_SILVER, ORE_GOLD, ORE_DIAMOND, ORE_COUNT };

typedef struct {
    const char *name;
    uint32_t value;     /* points when dug */
    uint32_t permille;  /* chance per 1000 cells */
} mg_ore_info;

extern const mg_ore_info mg_ores[ORE_COUNT];

/* xorshift32 PRNG: deterministic per seed so a map can be reproduced. */
uint32_t mg_rand(uint32_t *state);
/* Roll the hidden content of one cell. */
int mg_ore_roll(uint32_t *state);
void mg_dir_delta(int dir, int *dx, int *dy);

#endif
