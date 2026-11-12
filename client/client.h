#ifndef MG_CLIENT_H
#define MG_CLIENT_H

#include <stdint.h>
#include "protocol.h"

typedef struct {
    const char *host;
    uint16_t port;
    const char *name;
    int bot;
    int count;        /* bot connections */
    int duration;     /* bot run time in seconds, 0 = until Ctrl-C */
    int interval_ms;  /* delay between actions of one bot */
    int fragment_us;  /* send every byte separately (partial-recv demo) */
} client_opts;

typedef struct {
    uint16_t id, map_w, map_h, x, y;
} welcome_t;

/* Connect, send HELLO, wait for WELCOME. Returns fd or -1 (reason logged). */
int client_join(const client_opts *o, const char *name, int want_snapshot, mg_reader *r, welcome_t *wel);

int bot_run(const client_opts *o);
int ui_run(const client_opts *o);

#endif
