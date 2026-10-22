#include "gardener/ingest.h"
#include "gardener/packet.h"
#include "gardener/platform.h"
#include "gardener/replay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

/* ---------------------------------------------------------------------
 * Gardener entry point.
 *
 * Two modes:
 *   (default)      read JSONL packets from stdin, feed to ingest
 *   --replay FILE  replay a binary log through ingest
 *
 * In both modes, the same ingest_t is used, the same stats are
 * printed at the end, and the same --channels option selects the
 * registry. Log writing (B5) will be a third layer on top.
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
        "       %s --replay <log.bin> [replay options]\n"
        "\n"
        "Default mode reads JSONL packets from stdin.\n"
        "\n"
        "Options:\n"
        "  --channels <path>       channel registry (default: configs/channels.toml)\n"
        "  --stats-every <n>       print counters every n packets (default: 0 = off)\n"
        "  --verbose               log at DEBUG level\n"
        "  --quiet                 log at WARN level and above\n"
        "  --version               print version and exit\n"
        "  --help                  print this message\n"
        "\n"
        "Replay options (require --replay):\n"
        "  --replay <path>         replay a binary log instead of stdin\n"
        "  --replay-speed <mode>   max (default), real, or a positive factor\n"
        "  --replay-from <ts_ns>   start at this timestamp\n"
        "  --replay-to <ts_ns>     stop before this timestamp\n"
        "  --replay-loop           replay until interrupted\n"
        "  --replay-progress <n>   print progress every n packets\n",
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

/* Parse "--replay-speed" argument: "max", "real", or a positive float. */
static int parse_speed(const char *s, replay_speed_t *mode, double *factor) {
    if (!strcmp(s, "max")) {
        *mode = REPLAY_SPEED_MAX;
        *factor = 1.0;
        return 0;
    }
    if (!strcmp(s, "real")) {
        *mode = REPLAY_SPEED_REAL;
        *factor = 1.0;
        return 0;
    }
    char *end = NULL;
    double f = strtod(s, &end);
    if (!end || *end != 0 || f <= 0.0) return -1;
    *mode = REPLAY_SPEED_SCALED;
    *factor = f;
    return 0;
}

/* Run the default (stdin JSONL) loop. */
static int run_stdin_mode(const char *channels, unsigned long stats_every) {
    static ingest_t ing;
    if (ingest_init(&ing, channels) != 0) {
        plat_log(LOG_ERROR, "ingest_init failed");
        return 1;
    }

    plat_log(LOG_INFO, "gardener 0.1.0 (B4): reading JSONL packets from stdin");

    char line[LINE_MAX];
    unsigned long long lines = 0;
    unsigned long long bad = 0;

    while (!g_stop && fgets(line, sizeof(line), stdin)) {
        lines++;
        size_t len = strlen(line);
        while (len && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = 0;
        }
        if (len == 0) continue;
        if (line[0] == '#') continue;

        packet_t p;
        if (packet_from_json(line, &p) != 0) {
            bad++;
            plat_log(LOG_WARN, "bad packet on line %llu: %.80s%s",
                     lines, line, len > 80 ? "..." : "");
            continue;
        }
        if (ingest_feed(&ing, &p) < 0) {
            bad++;
            plat_log(LOG_WARN, "ingest_feed rejected packet on line %llu", lines);
            continue;
        }
        if (stats_every && (ing.packets_seen % stats_every) == 0) {
            print_stats(&ing);
        }
    }

    plat_log(LOG_INFO, "stdin closed or interrupted after %llu lines (%llu bad)",
             lines, bad);
    print_stats(&ing);
    return 0;
}

/* Run replay mode. */
static int run_replay_mode(const char *channels,
                           const replay_opts_t *replay_opts,
                           unsigned long stats_every) {
    static ingest_t ing;
    if (ingest_init(&ing, channels) != 0) {
        plat_log(LOG_ERROR, "ingest_init failed");
        return 1;
    }

    plat_log(LOG_INFO, "gardener 0.1.0 (B4): replaying %s", replay_opts->log_path);

    replay_stats_t rs;
    int rc = replay_run(replay_opts, &ing, &rs);
    if (rc != 0) return 1;

    print_stats(&ing);

    double wall_s = (double)rs.duration_ns / 1e9;
    double pps = (wall_s > 0.0) ? (double)rs.packets_fed / wall_s : 0.0;

    fprintf(stderr,
        "[replay] fed=%llu skipped=%llu loops=%llu wall=%.3fs rate=%.1f pkt/s\n",
        (unsigned long long)rs.packets_fed,
        (unsigned long long)rs.packets_skipped,
        (unsigned long long)rs.total_loops,
        wall_s, pps);

    (void)stats_every;   /* replay_run prints its own progress */
    return 0;
}

int main(int argc, char **argv) {
    const char *channels = "configs/channels.toml";
    const char *replay_path = NULL;
    int verbose = 0;
    int quiet   = 0;
    unsigned long stats_every = 0;

    replay_opts_t replay_opts;
    replay_opts_default(&replay_opts);

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if      (!strcmp(a, "--version")) { puts("gardener 0.1.0 (phase B4)"); return 0; }
        else if (!strcmp(a, "--verbose")) { verbose = 1; }
        else if (!strcmp(a, "--quiet"))   { quiet = 1; }
        else if (!strcmp(a, "--channels") && i + 1 < argc) {
            channels = argv[++i];
        }
        else if (!strcmp(a, "--stats-every") && i + 1 < argc) {
            stats_every = strtoul(argv[++i], NULL, 10);
        }
        else if (!strcmp(a, "--replay") && i + 1 < argc) {
            replay_path = argv[++i];
            replay_opts.log_path = replay_path;
        }
        else if (!strcmp(a, "--replay-speed") && i + 1 < argc) {
            if (parse_speed(argv[++i], &replay_opts.speed_mode,
                            &replay_opts.speed_factor) != 0) {
                fprintf(stderr, "bad --replay-speed: %s\n", argv[i]);
                return 2;
            }
        }
        else if (!strcmp(a, "--replay-from") && i + 1 < argc) {
            replay_opts.from_ts_ns = strtoull(argv[++i], NULL, 10);
        }
        else if (!strcmp(a, "--replay-to") && i + 1 < argc) {
            replay_opts.to_ts_ns = strtoull(argv[++i], NULL, 10);
        }
        else if (!strcmp(a, "--replay-loop")) {
            replay_opts.loop = true;
        }
        else if (!strcmp(a, "--replay-progress") && i + 1 < argc) {
            replay_opts.progress_every = strtoull(argv[++i], NULL, 10);
        }
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            print_usage(argv[0]); return 0;
        }
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

    if (replay_path) {
        return run_replay_mode(channels, &replay_opts, stats_every);
    }
    return run_stdin_mode(channels, stats_every);
}