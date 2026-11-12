#include "game.h"
#include "protocol.h"

const mg_ore_info mg_ores[ORE_COUNT] = {
    [ORE_NONE]    = { "nothing", 0,   850 },
    [ORE_COPPER]  = { "copper",  5,   100 },
    [ORE_SILVER]  = { "silver",  15,  35 },
    [ORE_GOLD]    = { "gold",    50,  12 },
    [ORE_DIAMOND] = { "diamond", 200, 3 },
};

uint32_t mg_rand(uint32_t *state)
{
    uint32_t x = *state ? *state : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

int mg_ore_roll(uint32_t *state)
{
    uint32_t r = mg_rand(state) % 1000;
    /* walk from rarest to commonest so the cumulative ranges are clear */
    uint32_t acc = 0;
    for (int ore = ORE_DIAMOND; ore >= ORE_COPPER; ore--) {
        acc += mg_ores[ore].permille;
        if (r < acc)
            return ore;
    }
    return ORE_NONE;
}

void mg_dir_delta(int dir, int *dx, int *dy)
{
    *dx = *dy = 0;
    switch (dir) {
    case DIR_UP:    *dy = -1; break;
    case DIR_DOWN:  *dy = 1;  break;
    case DIR_LEFT:  *dx = -1; break;
    case DIR_RIGHT: *dx = 1;  break;
    }
}
