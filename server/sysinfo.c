#include "sysinfo.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>

static size_t add(char *buf, size_t cap, size_t off, const char *fmt, ...)
{
    if (off >= cap)
        return off;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + off, cap - off, fmt, ap);
    va_end(ap);
    if (n < 0)
        return off;
    return off + (size_t)n >= cap ? cap - 1 : off + (size_t)n;
}

size_t build_sysinfo(world_t *w, char *buf, size_t cap)
{
    size_t o = 0;
    char host[64] = "?";
    gethostname(host, sizeof host - 1);
    struct utsname u;
    uname(&u);
    struct sysinfo si;
    sysinfo(&si);
    double load[3] = { 0 };
    getloadavg(load, 3);
    unsigned long mb = 1024UL * 1024UL;

    o = add(buf, cap, o, "hostname=%s\n", host);
    o = add(buf, cap, o, "os=%s %s %s\n", u.sysname, u.release, u.machine);
    o = add(buf, cap, o, "cpus=%ld\n", sysconf(_SC_NPROCESSORS_ONLN));
    o = add(buf, cap, o, "loadavg=%.2f %.2f %.2f\n", load[0], load[1], load[2]);
    o = add(buf, cap, o, "mem_total_mb=%lu\n", (unsigned long)(si.totalram * si.mem_unit / mb));
    o = add(buf, cap, o, "mem_free_mb=%lu\n", (unsigned long)(si.freeram * si.mem_unit / mb));
    o = add(buf, cap, o, "host_uptime_s=%ld\n", si.uptime);
    o = add(buf, cap, o, "processes=%u\n", si.procs);
    o = add(buf, cap, o, "server_pid=%d\n", (int)getppid());
    o = add(buf, cap, o, "handler_pid=%d\n", (int)getpid());
    o = add(buf, cap, o, "server_uptime_s=%ld\n", (long)(time(NULL) - w->start_time));
    o = add(buf, cap, o, "online=%d/%d\n", STAT_GET(w->st.online), w->cfg.max_players);
    o = add(buf, cap, o, "connections_total=%llu\n", (unsigned long long)STAT_GET(w->st.conns_total));
    o = add(buf, cap, o, "requests_total=%llu\n", (unsigned long long)STAT_GET(w->st.requests));
    o = add(buf, cap, o, "map=%dx%d dug=%llu\n", w->cfg.w, w->cfg.h,
            (unsigned long long)STAT_GET(w->st.dug_cells));
    o = add(buf, cap, o, "mined_value=%llu/%llu\n", (unsigned long long)STAT_GET(w->st.mined_value),
            (unsigned long long)w->initial_value);
    o = add(buf, cap, o, "lock=%s robust=%s\n", w->cfg.use_lock ? "on" : "off",
            w->cfg.robust ? "on" : "off");
    return o;
}
