#include "log.h"
#include "sysinfo.h"
#include "test.h"
#include <string.h>

int main(void)
{
    mg_log_level = -1;
    world_cfg c = { .w = 10, .h = 5, .seed = 3, .max_players = 8, .use_lock = 1, .robust = 1 };
    world_t *w = world_create(&c);
    char buf[2048];
    size_t n = build_sysinfo(w, buf, sizeof buf);
    CHECK(n > 0 && n < sizeof buf && strlen(buf) == n);
    const char *keys[] = { "hostname=", "os=Linux", "loadavg=", "mem_total_mb=", "server_pid=",
                           "handler_pid=", "online=0/8", "map=10x5", "lock=on robust=on" };
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++)
        CHECK(strstr(buf, keys[i]) != NULL);

    /* tiny buffer: truncated but still NUL-terminated, never overflows */
    char small[32];
    memset(small, 'X', sizeof small);
    n = build_sysinfo(w, small, sizeof small);
    CHECK(n < sizeof small && small[n] == '\0');
    world_destroy(w);
    return TEST_DONE("test_sysinfo");
}
