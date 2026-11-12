#ifndef MG_SERVER_H
#define MG_SERVER_H

#include <stdint.h>
#include "world.h"

typedef struct {
    uint16_t  port;
    world_cfg world;
    int reap;          /* robustness #2: SIGCHLD + waitpid */
    int naive_recv;    /* robustness #3 OFF: assume one recv() == one frame */
    int idle_timeout;  /* robustness #4: seconds, 0 = disabled */
    int quiet;         /* suppress the per-second status line */
} server_opts;

/* Serve one client in a forked child. Never returns. */
void session_run(int fd, world_t *w, const server_opts *opt);

#endif
