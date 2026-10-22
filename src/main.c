#include "gardener/ingest.h"
#include "gardener/packet.h"
#include "gardener/platform.h"
#include "gardener/replay.h"
#include "gardener/log_writer.h"
#include "gardener/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

/* ---------------------------------------------------------------------
 * Gardener entry point.
 *
 * Modes:
 *   (default)      read JSONL packets from stdin, feed to ingest
 *   --replay FILE  replay a binary log through ingest
 *
 * In both modes, the same ingest_t is used. If --log is passed, every
 * successfully ingested packet is also written to a binary log. The
 * log is optional and its failure does not stop ingest.
 *
 * Exit:
 *   0   ran to completion
 *   1   runtime error (failed to open replay log, ingest init, etc.)
 *   2   usage error
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
        "       %s --replay <log.bin> [replay options] [log options]\n"
        "\n"
        "Default mode reads JSONL packets from stdin.\n"
        "\n"
        "Common options:\n"
        "  --channels <path>       channel registry (default: configs/channels.toml)\n"
        "  --stats-every <n>       print counters every n packets (default: 0 = off)\n"
        "  --verbose               log at DEBUG level\n"
        "  --quiet                 log at WARN level and above\n"
        "  --version               print version and exit\n"
        "  --help                  print this message\n"
        "\n"
        "Logging (optional, both modes):\n"
        "  --log <path>            write ingested packets to a binary log\n"
        "  --log-max <bytes>       log size, accepts K/M/G suffix\n"
        "                          (default: 100M, minimum: 1M)\n"
        "  --log-fresh             truncate an existing log instead of appending\n"
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

/* ---- size parsing ---- */

/* Parse a size with optional K/M/G suffix. "100M" = 100 * 1024 * 1024.
 * Returns 0 on success, -1 on parse error. */
static int parse_size(const char *s, uint64_t *out) {
    if (!s || !out) return -1;
    char *end = NULL;
    double v = strtod(s, &end);
    if (!end || end == s || v < 0) return -1;
    uint64_t mult = 1;
    if (*end) {
        switch (*end) {
            case 'k': case 'K': mult = 1024ull; break;
            case 'm': case 'M': mult = 1024ull * 1024ull; break;
            case 'g': case 'G': mult = 1024ull * 1024ull * 1024ull; break;
            default: return -1;
        }
        end++;
        if (*end) return -1;   /* trailing garbage */
    }
    /* Guard overflow: reject sizes that would wrap. */
    if (v * (double)mult > 1e15) return -1;
    *out = (uint64_t)(v * (double)mult);
    return 0;
}

/* ---- stats helpers ---- */

static void print_ingest_stats(const ingest_t *ing) {
    fprintf(stderr,
        "[stats] packets=%llu samples=%llu dropped=%llu unknown=%llu "
        "channels_in_registry=%zu\n",
        (unsigned long long)ing->packets_seen,
        (unsigned long long)ing->samples_seen,
        (unsigned long long)ing->samples_dropped,
        (unsigned long long)ing->unknown_channels,
        ing->registry.count);
}

static void print_log_stats(const log_writer_t *lw) {
    if (!lw) return;
    fprintf(stderr,
        "[log] path=%s records=%llu offset=%llu wraps=%u\n",
        log_writer_path(lw),
        (unsigned long long)log_writer_record_count(lw),
        (unsigned long long)log_writer_write_offset(lw),
        (unsigned)log_writer_wrap_count(lw));
}

/* ---- replay-speed parser (unchanged from B4) ---- */

static int parse_speed(const char *s, replay_speed_t *mode, double *factor) {
    if (!strcmp(s, "max"))  { *mode = REPLAY_SPEED_MAX;    *factor = 1.0; return 0; }
    if (!strcmp(s, "real")) { *mode = REPLAY_SPEED_REAL;   *factor = 1.0; return 0; }
    char *end = NULL;
    double f = strtod(s, &end);
    if (!end || *end != 0 || f <= 0.0) return -1;
    *mode = REPLAY_SPEED_SCALED;
    *factor = f;
    return 0;
}

/* ---- shared run: feed a packet to ingest and (optionally) log it ---- */

/* Return 0 on success (packet handled), -1 on hard error.
 * Note: a log write failure does *not* fail the packet; it is logged
 * once and subsequent writes are suppressed to avoid log spam. */
static int handle_packet(ingest_t *ing, log_writer_t *lw,
                         const packet_t *p, bool *log_failed_once) {
    int n = ingest_feed(ing, p);
    if (n < 0) return -1;

    if (lw && !*log_failed_once) {
        int r = log_writer_append(lw, p);
        if (r < 0) {
            plat_log(LOG_WARN,
                     "log write failed; continuing without logging");
            *log_failed_once = true;
        }
    }
    return 0;
}

/* ---- stdin JSONL mode ---- */

static int run_stdin_mode(ingest_t *ing, log_writer_t *lw,
                          unsigned long stats_every) {
    plat_log(LOG_INFO,
             "gardener 0.1.0 (B5): reading JSONL packets from stdin%s",
             lw ? " (logging)" : "");

    char line[LINE_MAX];
    unsigned long long lines = 0;
    unsigned long long bad = 0;
    bool log_failed = false;

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
        if (handle_packet(ing, lw, &p, &log_failed) < 0) {
            bad++;
            plat_log(LOG_WARN, "ingest rejected packet on line %llu", lines);
            continue;
        }
        if (stats_every && (ing->packets_seen % stats_every) == 0) {
            print_ingest_stats(ing);
        }
    }

    plat_log(LOG_INFO,
             "stdin closed or interrupted after %llu lines (%llu bad)",
             lines, bad);
    return 0;
}

/* ---- replay mode ---- */

/* Replay with logging: feed through ingest, log each packet.
 * We cannot use replay_run() directly because it feeds via ingest_feed
 * internally and does not call log_writer_append. Instead we replicate
 * its loop here with both hooks. */

static int run_replay_mode(ingest_t *ing, log_writer_t *lw,
                           const replay_opts_t *opts,
                           unsigned long stats_every) {
    plat_log(LOG_INFO,
             "gardener 0.1.0 (B5): replaying %s%s",
             opts->log_path,
             lw ? " (logging)" : "");

    /* We reuse the replay_run() infrastructure but interpose log
     * writing by running replay through ingest and reading the
     * packet stream ourselves. Simpler: use replay_run() when there is
     * no log writer, and a custom loop with the writer otherwise. */

    if (!lw) {
        replay_stats_t rs;
        int rc = replay_run(opts, ing, &rs);
        if (rc != 0) return 1;
        double wall_s = (double)rs.duration_ns / 1e9;
        double pps = (wall_s > 0.0) ? (double)rs.packets_fed / wall_s : 0.0;
        fprintf(stderr,
                "[replay] fed=%llu skipped=%llu loops=%llu wall=%.3fs rate=%.1f pkt/s\n",
                (unsigned long long)rs.packets_fed,
                (unsigned long long)rs.packets_skipped,
                (unsigned long long)rs.total_loops,
                wall_s, pps);
        return 0;
    }

    /* Replay + log: replicate the reader loop, feeding and logging. */
    log_reader_t *r = log_reader_open(opts->log_path);
    if (!r) return 1;

    if (opts->from_ts_ns) {
        if (log_reader_seek_ns(r, opts->from_ts_ns) != 0) {
            log_reader_close(r);
            return 1;
        }
    }

    uint64_t start_ns = plat_now_ns();
    uint64_t packets_fed = 0, packets_skipped = 0, loops = 0;
    bool log_failed = false;

    for (;;) {
        if (g_stop) break;

        packet_t p;
        int rc = log_reader_next(r, &p);
        if (rc < 0) { log_reader_close(r); return 1; }
        if (rc == 0) {
            if (!opts->loop) break;
            loops++;
            log_reader_rewind(r);
            if (opts->from_ts_ns) {
                if (log_reader_seek_ns(r, opts->from_ts_ns) != 0) {
                    log_reader_close(r);
                    return 1;
                }
            }
            continue;
        }

        if (opts->from_ts_ns && p.timestamp_ns < opts->from_ts_ns) {
            packets_skipped++;
            continue;
        }
        if (opts->to_ts_ns && p.timestamp_ns >= opts->to_ts_ns) {
            if (!opts->loop) break;
            loops++;
            log_reader_rewind(r);
            if (opts->from_ts_ns) {
                if (log_reader_seek_ns(r, opts->from_ts_ns) != 0) {
                    log_reader_close(r);
                    return 1;
                }
            }
            continue;
        }

        if (handle_packet(ing, lw, &p, &log_failed) < 0) {
            log_reader_close(r);
            return 1;
        }
        packets_fed++;
        if (stats_every && (packets_fed % stats_every) == 0) {
            print_ingest_stats(ing);
        }
    }

    uint64_t duration_ns = plat_now_ns() - start_ns;
    log_reader_close(r);

    double wall_s = (double)duration_ns / 1e9;
    double pps = (wall_s > 0.0) ? (double)packets_fed / wall_s : 0.0;
    fprintf(stderr,
            "[replay] fed=%llu skipped=%llu loops=%llu wall=%.3fs rate=%.1f pkt/s\n",
            (unsigned long long)packets_fed,
            (unsigned long long)packets_skipped,
            (unsigned long long)loops,
            wall_s, pps);
    return 0;
}

/* ---- main ---- */

int main(int argc, char **argv) {
    const char *channels = "configs/channels.toml";
    const char *replay_path = NULL;
    const char *log_path = NULL;
    uint64_t    log_max = LOG_DEFAULT_MAX;
    bool        log_fresh = false;
    int verbose = 0;
    int quiet = 0;
    unsigned long stats_every = 0;

    replay_opts_t replay_opts;
    replay_opts_default(&replay_opts);

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if      (!strcmp(a, "--version")) { puts("gardener 0.1.0 (phase B5)"); return 0; }
        else if (!strcmp(a, "--verbose")) { verbose = 1; }
        else if (!strcmp(a, "--quiet"))   { quiet = 1; }
        else if (!strcmp(a, "--channels") && i + 1 < argc) {
            channels = argv[++i];
        }
        else if (!strcmp(a, "--stats-every") && i + 1 < argc) {
            stats_every = strtoul(argv[++i], NULL, 10);
        }
        else if (!strcmp(a, "--log") && i + 1 < argc) {
            log_path = argv[++i];
        }
        else if (!strcmp(a, "--log-max") && i + 1 < argc) {
            if (parse_size(argv[++i], &log_max) != 0) {
                fprintf(stderr, "bad --log-max: %s\n", argv[i]);
                return 2;
            }
        }
        else if (!strcmp(a, "--log-fresh")) {
            log_fresh = true;
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

    /* Ingest init. */
    static ingest_t ing;
    if (ingest_init(&ing, channels) != 0) {
        plat_log(LOG_ERROR, "ingest_init failed");
        return 1;
    }

    /* Log writer, optional. Failure to open is non-fatal: log a warning
     * and continue without a log. The user gets a clear stderr message
     * and the run proceeds. */
    log_writer_t *lw = NULL;
    if (log_path) {
        lw = log_writer_open(log_path, log_max, &ing.registry, log_fresh);
        if (!lw) {
            plat_log(LOG_WARN,
                     "log writer failed to open %s; continuing without logging",
                     log_path);
        }
    }

    int rc;
    if (replay_path) {
        rc = run_replay_mode(&ing, lw, &replay_opts, stats_every);
    } else {
        rc = run_stdin_mode(&ing, lw, stats_every);
    }

    print_ingest_stats(&ing);
    if (lw) {
        print_log_stats(lw);
    } else if (log_path) {
        fprintf(stderr, "[log] disabled (open failed)\n");
    }

    if (lw) log_writer_close(lw);
    return rc;
}