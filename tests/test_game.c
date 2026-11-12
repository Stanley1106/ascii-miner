#include "game.h"
#include "protocol.h"
#include "test.h"

int main(void)
{
    /* probabilities sum to 1000 and rarer ores are worth more */
    uint32_t sum = 0;
    for (int i = 0; i < ORE_COUNT; i++)
        sum += mg_ores[i].permille;
    CHECK(sum == 1000);
    for (int i = ORE_COPPER + 1; i < ORE_COUNT; i++) {
        CHECK(mg_ores[i].value > mg_ores[i - 1].value);
        CHECK(mg_ores[i].permille < mg_ores[i - 1].permille);
    }

    /* same seed -> same map */
    uint32_t a = 1234, b = 1234;
    int same = 1;
    for (int i = 0; i < 1000; i++)
        same &= mg_ore_roll(&a) == mg_ore_roll(&b);
    CHECK(same);

    /* empirical distribution roughly matches the table (200k rolls) */
    int count[ORE_COUNT] = { 0 };
    uint32_t s = 42;
    const int N = 200000;
    for (int i = 0; i < N; i++)
        count[mg_ore_roll(&s)]++;
    for (int i = 0; i < ORE_COUNT; i++) {
        double expect = N * mg_ores[i].permille / 1000.0;
        CHECK(count[i] > expect * 0.85 && count[i] < expect * 1.15);
    }

    int dx, dy;
    mg_dir_delta(DIR_UP, &dx, &dy);    CHECK(dx == 0 && dy == -1);
    mg_dir_delta(DIR_RIGHT, &dx, &dy); CHECK(dx == 1 && dy == 0);
    mg_dir_delta(99, &dx, &dy);        CHECK(dx == 0 && dy == 0);

    return TEST_DONE("test_game");
}
