#include "log.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

int mg_log_level = MG_LOG_INFO;

static const char *g_tag = "miner";
static int g_fd = STDERR_FILENO;
static const char *const lvl_name[] = { "ERROR", "WARN", "INFO", "DEBUG" };

int mg_log_parse_level(const char *s)
{
    if (!s || !*s)
        return -1;
    if (s[0] >= '0' && s[0] <= '3' && s[1] == '\0')
        return s[0] - '0';
    for (int i = 0; i < 4; i++)
        if (strcasecmp(s, lvl_name[i]) == 0)
            return i;
    return -1;
}

void mg_log_init(const char *tag, int cli_level)
{
    if (tag)
        g_tag = tag;
    int env = mg_log_parse_level(getenv("MINER_LOG"));
    if (cli_level >= 0)
        mg_log_level = cli_level;
    else if (env >= 0)
        mg_log_level = env;
    else
        mg_log_level = MG_LOG_INFO;
}

int mg_log_set_file(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    g_fd = fd;
    return 0;
}

void mg_log_write(int level, const char *file, int line, const char *fmt, ...)
{
    if (level < 0 || level > MG_LOG_DEBUG)
        level = MG_LOG_ERROR;

    struct timeval tv;
    struct tm tm;
    char ts[16];
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm);
    strftime(ts, sizeof ts, "%H:%M:%S", &tm);

    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;

    /* Format the whole line first and emit it with one write(), so lines
     * from many forked processes sharing stderr do not interleave. */
    char buf[1024];
    int n = snprintf(buf, sizeof buf, "%s.%03ld %s[%d] %-5s %s:%d: ",
                     ts, (long)(tv.tv_usec / 1000), g_tag, (int)getpid(),
                     lvl_name[level], base, line);
    va_list ap;
    va_start(ap, fmt);
    if (n >= 0 && (size_t)n < sizeof buf)
        n += vsnprintf(buf + n, sizeof buf - n, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof buf - 1)
        n = sizeof buf - 2;
    buf[n++] = '\n';
    ssize_t w = write(g_fd, buf, n);
    (void)w;
}
