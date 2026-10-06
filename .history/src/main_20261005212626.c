#include "gardener/ingest.h"
#include "gardener/packet.h"
#include "gardener/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

/* ---------------------------------------------------------------------
 * Gardener entry point, phase A4.
 *
 * Reads JSONL packets from stdin, feeds them to ingest, prints stats on
 * shutdown. Phase B will add a file log; phase F will add a TCP link to
 * the dashboard. For now, stdin and stderr are the whole world.
 * ------------------------------------------------------------------- */

#define LINE_MAX 8192

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
        "usage: %s [options]\n"
        "\n"
        "Reads JSONL packets from stdin, one packet per line, and feeds\n"
        "them into the ingest engine. Prints stats on exit.\n"
        "\n"
        "Options:\n"
        "  --channels <path>   channel registry file (default: configs/channels.toml)\n"
        "  --stats-every <n>   print counters every n packets (default: 0 = off)\n"
        "  --verbose           log at DEBUG level\n"
        "  --quiet             log at WARN level and above\n"
        "  --version           print version and exit\n"
        "  --help              print this message\n"
        "\n"
        "Example:\n"
        "  echo '{\"t\":1000,\"v\":{\"1\":20.5,\"3\":true}}' | %s\n",
        prog, prog);
}

static void print_stats(const ingest_t *ing) {
    fprintf(stderr,
        "[stats] packets=%llu samples=%llu dropped=%llu unknown=%llu "
        "channels_in_registry=%zu\n",
        (unsigned long long)ing->packets_seen,
        (unsigned long long)ing->samples_seen,
        (unsigned long long)ing->samples_dropped,
        (unsigned long long)ing->unknown_channels,
        ing->registry.count);
}

int main(int argc, char **argv) {
    const char *channels = "configs/channels.toml";
    int verbose = 0;
    int quiet   = 0;
    unsigned long stats_every = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if      (!strcmp(a, "--version"))    { puts("gardener 0.1.0 (phase A4)"); return 0; }
        else if (!strcmp(a, "--verbose"))    { verbose = 1; }
        else if (!strcmp(a, "--quiet"))      { quiet = 1; }
        else if (!strcmp(a, "--channels") && i + 1 < argc) { channels = argv[++i]; }
        else if (!strcmp(a, "--stats-every") && i + 1 < argc) {
            stats_every = strtoul(argv[++i], NULL, 10);
        }
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { print_usage(argv[0]); return 0; }
        else {
            fprintf(stderr, "unknown argument: %s\n", a);
            print_usage(argv[0]);
            return 2;
        }
    }

    if (verbose) plat_log_set_level(LOG_DEBUG);
    if (quiet)   plat_log_set_level(LOG_WARN);

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    static ingest_t ing;
    if (ingest_init(&ing, channels) != 0) {
        plat_log(LOG_ERROR, "ingest_init failed");
        return 1;
    }

    plat_log(LOG_INFO, "gardener 0.1.0 (phase A4): reading JSONL packets from stdin");

    /* Line-oriented read. fgets handles the common case; a line longer
     * than LINE_MAX is truncated and will fail to parse, which is the
     * right behavior — we don't want to silently lose data. */
    char line[LINE_MAX];
    unsigned long long lines = 0;
    unsigned long long bad_lines = 0;

    while (!g_stop && fgets(line, sizeof(line), stdin)) {
        lines++;

        /* Strip trailing newline(s). fgets always null-terminates. */
        size_t len = strlen(line);
        while (len && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = 0;
        }
        if (len == 0) continue;         /* blank line: skip silently */
        if (line[0] == '#') continue;    /* comment: skip silently */

        packet_t p;
        if (packet_from_json(line, &p) != 0) {
            bad_lines++;
            plat_log(LOG_WARN, "bad packet on line %llu: %.80s%s",
                     lines, line, len > 80 ? "..." : "");
            continue;
        }

        int n = ingest_feed(&ing, &p);
        if (n < 0) {
            bad_lines++;
            plat_log(LOG_WARN, "ingest_feed rejected packet on line %llu", lines);
            continue;
        }

        if (stats_every && (ing.packets_seen % stats_every) == 0) {
            print_stats(&ing);
        }
    }

    plat_log(LOG_INFO, "stdin closed or interrupted after %llu lines "
                       "(%llu bad)", lines, bad_lines);
    print_stats(&ing);

    return 0;
}