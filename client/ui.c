/* ui.c — interactive terminal client drawn with raw termios + ANSI escapes. */
#include "client.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "game.h"
#include "log.h"
#include "net.h"

#define MAX_SEEN_PLAYERS 1024
#define PING_EVERY_MS 5000
#define VIEW_MAX (200 * 100)

typedef struct { uint16_t id, x, y; } seen_player;

typedef struct {
    int fd;
    mg_reader rd;
    welcome_t wel;
    uint16_t px, py;
    uint8_t *dug;                 /* map_w * map_h, 1 = dug */
    seen_player players[MAX_SEEN_PLAYERS];
    int nplayers;
    uint32_t total, digs;
    uint32_t seq;
    const char *name;
    char msg[160];
    char panel[2048];
} ui_t;

static struct termios g_saved;
static int g_raw;
static volatile sig_atomic_t g_quit, g_resized;

static void on_quit(int s) { (void)s; g_quit = 1; }
static void on_winch(int s) { (void)s; g_resized = 1; }

static void term_restore(void)
{
    if (!g_raw)
        return;
    /* show cursor, reset colours, leave the alternate screen */
    const char *seq = "\x1b[0m\x1b[?25h\x1b[?1049l";
    ssize_t w = write(STDOUT_FILENO, seq, strlen(seq));
    (void)w;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved);
    g_raw = 0;
}

static int term_raw(void)
{
    if (tcgetattr(STDIN_FILENO, &g_saved) < 0)
        return -1;
    struct termios t = g_saved;
    t.c_lflag &= ~(ICANON | ECHO); /* keep ISIG so Ctrl-C still works */
    t.c_iflag &= ~(IXON | ICRNL);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) < 0)
        return -1;
    g_raw = 1;
    atexit(term_restore);
    const char *seq = "\x1b[?1049h\x1b[?25l\x1b[2J";
    ssize_t w = write(STDOUT_FILENO, seq, strlen(seq));
    (void)w;
    return 0;
}

/* ---------- frame buffer ---------- */

static char g_out[512 * 1024];
static size_t g_len;

static void out(const char *s, size_t n)
{
    if (g_len + n < sizeof g_out) {
        memcpy(g_out + g_len, s, n);
        g_len += n;
    }
}
static void outs(const char *s) { out(s, strlen(s)); }

static void eol(void) { outs("\x1b[K\r\n"); }

/* Plain text truncated to the terminal width (no line ending). */
static void text(int cols, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0)
        n = 0;
    if (n > cols)
        n = cols;
    if ((size_t)n >= sizeof buf)
        n = sizeof buf - 1;
    out(buf, (size_t)n);
}

static void render(ui_t *u)
{
    struct winsize ws = { .ws_row = 24, .ws_col = 80 };
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    int rows = ws.ws_row ? ws.ws_row : 24, cols = ws.ws_col ? ws.ws_col : 80;
    int W = u->wel.map_w, H = u->wel.map_h;

    int panel_lines = 0;
    for (const char *p = u->panel; *p; p++)
        panel_lines += *p == '\n';
    if (u->panel[0] && u->panel[strlen(u->panel) - 1] != '\n')
        panel_lines++;
    int view_h = rows - 3 - panel_lines;
    if (view_h < 3)
        view_h = 3;
    if (view_h > H)
        view_h = H;
    int view_w = cols < W ? cols : W;
    int ox = u->px - view_w / 2, oy = u->py - view_h / 2;
    if (ox > W - view_w) ox = W - view_w;
    if (oy > H - view_h) oy = H - view_h;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;

    static uint8_t others[VIEW_MAX];
    memset(others, 0, (size_t)view_w * view_h);
    for (int i = 0; i < u->nplayers; i++) {
        seen_player *p = &u->players[i];
        if (p->id == u->wel.id)
            continue;
        int vx = p->x - ox, vy = p->y - oy;
        if (vx >= 0 && vy >= 0 && vx < view_w && vy < view_h)
            others[vy * view_w + vx] = 1;
    }

    g_len = 0;
    outs("\x1b[H\x1b[7m");
    text(cols, " MINER | %s (#%u) | pos (%u,%u) | online %d | value %u | digs %u ",
         u->name, u->wel.id, u->px, u->py, u->nplayers, u->total, u->digs);
    outs("\x1b[0m");
    eol();

    for (int vy = 0; vy < view_h; vy++) {
        int color = -1; /* 0 rock, 1 dug, 2 me, 3 other */
        for (int vx = 0; vx < view_w; vx++) {
            int x = ox + vx, y = oy + vy, c;
            char ch;
            if (x == u->px && y == u->py)      { c = 2; ch = '@'; }
            else if (others[vy * view_w + vx]) { c = 3; ch = 'P'; }
            else if (u->dug[y * W + x])        { c = 1; ch = '.'; }
            else                               { c = 0; ch = '#'; }
            if (c != color) {
                static const char *const esc[] = { "\x1b[0;33m", "\x1b[0;90m", "\x1b[1;93m", "\x1b[1;96m" };
                outs(esc[c]);
                color = c;
            }
            out(&ch, 1);
        }
        outs("\x1b[0m");
        eol();
    }
    text(cols, "%s", u->msg);
    eol();
    const char *p = u->panel;
    while (*p) {
        const char *nl = strchr(p, '\n');
        int n = nl ? (int)(nl - p) : (int)strlen(p);
        text(cols, "%.*s", n, p);
        eol();
        p += n + (nl ? 1 : 0);
    }
    outs("\x1b[7m");
    text(cols, " WASD/arrows move  SPACE dig  i inventory  y sysinfo  c close  q quit | @ you P player # rock . dug ");
    outs("\x1b[0m\x1b[K\x1b[J"); /* last row: no newline, so the screen never scrolls */
    ssize_t w = write(STDOUT_FILENO, g_out, g_len);
    (void)w;
}

/* ---------- protocol handling ---------- */

static void set_msg(ui_t *u, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(u->msg, sizeof u->msg, fmt, ap);
    va_end(ap);
}

static void apply_snapshot(ui_t *u, const mg_msg *m)
{
    if (m->len < 10)
        return;
    int W = mg_get_u16(m->payload + 4), H = mg_get_u16(m->payload + 6);
    int n = mg_get_u16(m->payload + 8);
    size_t need = 10 + (size_t)n * 6 + ((size_t)W * H + 7) / 8;
    if (W != u->wel.map_w || H != u->wel.map_h || m->len < need) {
        LOG_WARN("bad snapshot (len %u, need %zu)", m->len, need);
        return;
    }
    const uint8_t *p = m->payload + 10;
    u->nplayers = n < MAX_SEEN_PLAYERS ? n : MAX_SEEN_PLAYERS;
    for (int i = 0; i < u->nplayers; i++, p += 6) {
        u->players[i] = (seen_player){ mg_get_u16(p), mg_get_u16(p + 2), mg_get_u16(p + 4) };
        if (u->players[i].id == u->wel.id) {
            u->px = u->players[i].x;
            u->py = u->players[i].y;
        }
    }
    p = m->payload + 10 + (size_t)n * 6;
    for (int i = 0; i < W * H; i++)
        u->dug[i] = (p[i / 8] >> (i % 8)) & 1;
}

static void show_inventory(ui_t *u, const mg_msg *m)
{
    if (m->len < 4 * ORE_COUNT + 8)
        return;
    size_t o = (size_t)snprintf(u->panel, sizeof u->panel, "-- Inventory --\n");
    for (int i = ORE_COPPER; i < ORE_COUNT; i++)
        o += (size_t)snprintf(u->panel + o, sizeof u->panel - o, "  %-8s x%-4u (%u each)\n",
                              mg_ores[i].name, mg_get_u32(m->payload + 4 * i), mg_ores[i].value);
    snprintf(u->panel + o, sizeof u->panel - o, "  total value %u, digs %u\n",
             mg_get_u32(m->payload + 4 * ORE_COUNT), mg_get_u32(m->payload + 4 * ORE_COUNT + 4));
}

/* Returns -1 when the session is over. */
static int on_msg(ui_t *u, const mg_msg *m)
{
    switch (m->type) {
    case MSG_POS:
        if (m->len >= 4) {
            u->px = mg_get_u16(m->payload);
            u->py = mg_get_u16(m->payload + 2);
        }
        break;
    case MSG_DIG_RESULT: {
        if (m->len < 14)
            break;
        int x = mg_get_u16(m->payload), y = mg_get_u16(m->payload + 2);
        int st = m->payload[4], ore = m->payload[5] < ORE_COUNT ? m->payload[5] : 0;
        u->total = mg_get_u32(m->payload + 10);
        u->dug[y * u->wel.map_w + x] = 1;
        if (st == DIG_ALREADY) {
            set_msg(u, "(%d,%d) was already dug by someone. Try elsewhere.", x, y);
        } else {
            u->digs++;
            if (st == DIG_FOUND)
                set_msg(u, "(%d,%d): found %s! +%u  (total %u)", x, y, mg_ores[ore].name,
                        mg_get_u32(m->payload + 6), u->total);
            else
                set_msg(u, "(%d,%d): nothing here.", x, y);
        }
        break;
    }
    case MSG_INV_RESP:
        show_inventory(u, m);
        break;
    case MSG_SYSINFO_RESP:
        snprintf(u->panel, sizeof u->panel, "-- Server system info --\n%.*s", (int)m->len,
                 (const char *)m->payload);
        break;
    case MSG_SNAPSHOT:
        apply_snapshot(u, m);
        break;
    case MSG_ERROR:
        set_msg(u, "server error: %.*s", m->len > 1 ? (int)m->len - 1 : 0, (const char *)m->payload + 1);
        LOG_WARN("%s", u->msg);
        break;
    default:
        break;
    }
    return 0;
}

static void send_req(ui_t *u, uint8_t type, const void *p, uint32_t n)
{
    if (mg_send_msg(u->fd, type, ++u->seq, p, n) < 0)
        set_msg(u, "send failed: %s", strerror(errno));
}

/* Returns 1 when the user asked to quit. */
static int on_keys(ui_t *u)
{
    char buf[64];
    ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
    for (ssize_t i = 0; i < n; i++) {
        int dir = -1;
        char c = buf[i];
        if (c == 0x1b && i + 2 < n && buf[i + 1] == '[') {
            switch (buf[i + 2]) {
            case 'A': dir = DIR_UP; break;
            case 'B': dir = DIR_DOWN; break;
            case 'C': dir = DIR_RIGHT; break;
            case 'D': dir = DIR_LEFT; break;
            }
            i += 2;
        } else {
            switch (c) {
            case 'w': case 'W': dir = DIR_UP; break;
            case 's': case 'S': dir = DIR_DOWN; break;
            case 'a': case 'A': dir = DIR_LEFT; break;
            case 'd': case 'D': dir = DIR_RIGHT; break;
            case ' ': send_req(u, MSG_DIG, NULL, 0); break;
            case 'i': case 'I': send_req(u, MSG_INV_REQ, NULL, 0); break;
            case 'y': case 'Y': send_req(u, MSG_SYSINFO_REQ, NULL, 0); break;
            case 'c': case 'C': u->panel[0] = '\0'; break;
            case 'q': case 'Q': return 1;
            }
        }
        if (dir >= 0) {
            uint8_t d = (uint8_t)dir;
            send_req(u, MSG_MOVE, &d, 1);
        }
    }
    return 0;
}

int ui_run(const client_opts *o)
{
    static ui_t u;
    static mg_msg m;
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "interactive mode needs a terminal (use --bot otherwise)\n");
        return 2;
    }
    u.name = o->name;
    u.fd = client_join(o, o->name, 1, &u.rd, &u.wel);
    if (u.fd < 0) {
        fprintf(stderr, "could not join %s:%u (see log)\n", o->host, o->port);
        return 1;
    }
    u.px = u.wel.x;
    u.py = u.wel.y;
    u.dug = calloc((size_t)u.wel.map_w * u.wel.map_h, 1);
    if (!u.dug || term_raw() < 0) {
        perror("terminal");
        return 1;
    }
    signal(SIGINT, on_quit);
    signal(SIGTERM, on_quit);
    signal(SIGWINCH, on_winch);
    signal(SIGPIPE, SIG_IGN);
    set_msg(&u, "Welcome, %s! Every rock looks the same - pick a spot and dig.", o->name);

    const char *bye = "bye";
    long last_ping = mg_now_ms();
    render(&u);
    while (!g_quit) {
        struct pollfd pf[2] = { { .fd = STDIN_FILENO, .events = POLLIN },
                                { .fd = u.fd, .events = POLLIN } };
        int pr = poll(pf, 2, 1000);
        if (pr < 0 && errno != EINTR)
            break;
        if (pr > 0 && (pf[0].revents & POLLIN) && on_keys(&u))
            break;
        if (pr > 0 && (pf[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            int n = mg_reader_fill(&u.rd, u.fd);
            if (n <= 0) {
                bye = n == 0 ? "connection closed by server" : "connection error";
                break;
            }
            int rc;
            while ((rc = mg_reader_next(&u.rd, &m)) == 1)
                on_msg(&u, &m);
            if (rc < 0) {
                bye = "protocol error from server";
                break;
            }
        }
        if (mg_now_ms() - last_ping >= PING_EVERY_MS) {
            send_req(&u, MSG_PING, NULL, 0);
            last_ping = mg_now_ms();
        }
        render(&u);
    }
    mg_send_msg(u.fd, MSG_BYE, ++u.seq, NULL, 0);
    close(u.fd);
    term_restore();
    printf("%s. You dug %u cells, total value %u.\n", bye, u.digs, u.total);
    free(u.dug);
    return 0;
}
