/*
 * log.h — two-layer debug log control.
 *
 * Layer 1 (compile time): LOG_DEBUG() only exists when built with
 *   -DMINER_DEBUG (`make DEBUG=1`). Otherwise the call is dead code and
 *   the compiler removes it (and its format string) from the binary.
 * Layer 2 (run time): mg_log_level filters ERROR/WARN/INFO/DEBUG.
 *   Set by CLI (--log-level / -v) or the MINER_LOG environment variable.
 */
#ifndef MG_LOG_H
#define MG_LOG_H

enum { MG_LOG_ERROR = 0, MG_LOG_WARN = 1, MG_LOG_INFO = 2, MG_LOG_DEBUG = 3 };

extern int mg_log_level;

/* cli_level < 0 means "not given": fall back to $MINER_LOG, then INFO. */
void mg_log_init(const char *tag, int cli_level);
int  mg_log_set_file(const char *path);
int  mg_log_parse_level(const char *s);
void mg_log_write(int level, const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

#define MG_LOG_AT(lvl, ...)                                              \
    do {                                                                 \
        if ((lvl) <= mg_log_level)                                       \
            mg_log_write((lvl), __FILE__, __LINE__, __VA_ARGS__);        \
    } while (0)

#define LOG_ERROR(...) MG_LOG_AT(MG_LOG_ERROR, __VA_ARGS__)
#define LOG_WARN(...)  MG_LOG_AT(MG_LOG_WARN, __VA_ARGS__)
#define LOG_INFO(...)  MG_LOG_AT(MG_LOG_INFO, __VA_ARGS__)

#ifdef MINER_DEBUG
#define LOG_DEBUG(...) MG_LOG_AT(MG_LOG_DEBUG, __VA_ARGS__)
#else
/* if (0) keeps arguments type-checked but generates no code. */
#define LOG_DEBUG(...) do { if (0) mg_log_write(MG_LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__); } while (0)
#endif

#endif
