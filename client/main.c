/* main.c — miner_client: interactive terminal player or --bot load generator. */
#include "client.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>

#include "log.h"

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [options]\n"
        "  -s, --server HOST      server host (default 127.0.0.1)\n"
        "  -p, --port N           server port (default %d)\n"
        "  -n, --name NAME        player name (bots: name prefix)\n"
        "  -v, --verbose          more logs (repeatable)\n"
        "      --log-level L      error|warn|info|debug (or env MINER_LOG)\n"
        "      --log-file PATH    log file (interactive default: miner_client.log)\n"
        " bot mode:\n"
        "      --bot              run automated players instead of the UI\n"
        "  -c, --count N          number of bot connections (default 1)\n"
        "  -d, --duration S       stop after S seconds (default: until Ctrl-C)\n"
        "  -i, --interval-ms N    delay between a bot's actions (default 100)\n"
        "      --fragment-us N    send each byte separately, N us apart\n",
        prog, MG_DEFAULT_PORT);
}

int main(int argc, char **argv)
{
    client_opts o = { .host = "127.0.0.1", .port = MG_DEFAULT_PORT, .name = NULL,
                      .count = 1, .interval_ms = 100 };
    int cli_level = -1, verbose = 0;
    const char *log_file = NULL;
    enum { O_LOGLEVEL = 256, O_LOGFILE, O_BOT, O_FRAG };
    static const struct option longopts[] = {
        { "server", required_argument, 0, 's' },
        { "port", required_argument, 0, 'p' },
        { "name", required_argument, 0, 'n' },
        { "verbose", no_argument, 0, 'v' },
        { "help", no_argument, 0, 'h' },
        { "count", required_argument, 0, 'c' },
        { "duration", required_argument, 0, 'd' },
        { "interval-ms", required_argument, 0, 'i' },
        { "log-level", required_argument, 0, O_LOGLEVEL },
        { "log-file", required_argument, 0, O_LOGFILE },
        { "bot", no_argument, 0, O_BOT },
        { "fragment-us", required_argument, 0, O_FRAG },
        { 0, 0, 0, 0 },
    };
    int c;
    while ((c = getopt_long(argc, argv, "s:p:n:vhc:d:i:", longopts, NULL)) != -1) {
        switch (c) {
        case 's': o.host = optarg; break;
        case 'p': o.port = (uint16_t)atoi(optarg); break;
        case 'n': o.name = optarg; break;
        case 'v': verbose++; break;
        case 'c': o.count = atoi(optarg); break;
        case 'd': o.duration = atoi(optarg); break;
        case 'i': o.interval_ms = atoi(optarg); break;
        case O_LOGLEVEL:
            if ((cli_level = mg_log_parse_level(optarg)) < 0) {
                fprintf(stderr, "bad log level: %s\n", optarg);
                return 2;
            }
            break;
        case O_LOGFILE: log_file = optarg; break;
        case O_BOT: o.bot = 1; break;
        case O_FRAG: o.fragment_us = atoi(optarg); break;
        default: usage(argv[0]); return c == 'h' ? 0 : 2;
        }
    }
    if (o.count < 1 || o.interval_ms < 0) {
        usage(argv[0]);
        return 2;
    }
    if (verbose && cli_level < 0)
        cli_level = MG_LOG_INFO + verbose > MG_LOG_DEBUG ? MG_LOG_DEBUG : MG_LOG_INFO + verbose;
    mg_log_init(o.bot ? "bot" : "client", cli_level);
    if (log_file && mg_log_set_file(log_file) < 0) {
        perror(log_file);
        return 1;
    }
    if (!o.name)
        o.name = o.bot ? "bot" : getenv("USER") ? getenv("USER") : "player";
    if (o.bot)
        return bot_run(&o);
    /* the terminal belongs to the game screen, so logs go to a file */
    if (!log_file && mg_log_set_file("miner_client.log") < 0)
        perror("miner_client.log");
    return ui_run(&o);
}
