#ifndef MG_SYSINFO_H
#define MG_SYSINFO_H
#include <stddef.h>
#include "world.h"
/* Fill buf with "key=value\n" lines describing host and server. Returns length. */
size_t build_sysinfo(world_t *w, char *buf, size_t cap);
#endif
