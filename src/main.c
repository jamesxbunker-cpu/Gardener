#include "gardener/ingest.h"
#include "gardener/packet.h"
#include "gardener/platform.h"
#include "gardener/replay.h"
#include "gardener/log_writer.h"
#include "gardener/log.h"
#include "gardener/baseline.h"
#include "gardener/detect.h"
#include "gardener/event.h"
#include "gardener/policy.h"

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
 * Optional persistent state:
 *   --log <path>       write every accepted packet to a binary log
 *   --baseline <path>  maintain a model of "normal" and save it
 *
 * Optional anomaly detection:
 *   --detect           run detectors on every packet
 *   --events <path>    write events to this JSONL file
 *
 * Exit codes:
 *   0   ran to completion
 *   1   runtime error
 *   2   usage error
 * ------------------------------------------------------------------- */

#define LINE_MAX 8192

#define BASELINE_DECAY_INTERVAL_MS  30000u

const char *policy_path = NULL;
bool policy_dry_run = false;

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
        "usage: %s [options]\n"
        "       %s --replay <log.bin> [options]\n"
        "\n"
        "Common options:\n"
        "  --channels <path>       channel registry (default: configs/channels.toml)\n"
        "  --stats-every <n>       print counters every n packets\n"
        "  --verbose               log at DEBUG level\n"
        "  --quiet                 log at WARN level and above\n"
        "  --version               print version and exit\n"
        "  --help                  print this message\n"
        "\n"
        "Logging:\n"
        "  --log <path>            write ingested packets to a binary log\n"
        "  --log-max <bytes>       log size, accepts K/M/G suffix (default: 100M)\n"
        "  --log-fresh             truncate an existing log instead of appending\n"
        "\n"
        "Baseline:\n"
        "  --baseline <path>       load or create a baseline model\n"
        "  --baseline-fresh        start from empty, ignore existing file\n"
        "  --baseline-halflife <s> decay half-life in seconds (default: 300)\n"
        "  --baseline-save-every <s> periodic save interval (default: 0 = exit only)\n"
        "\n"
        "Detection:\n"
        "  --detect                run detectors on every packet\n"
        "  --events <path>         write events to this JSONL file\n"
        "  --detect-pair <a,b>     correlation pair (repeatable)\n"
        "  --detect-mismatch <c,a> mismatch pair (ctrl_out,actuator; repeatable)\n"
        "  --detect-zscore <n>     z-score threshold in sigma (default: 5.0)\n"
        "  --detect-roc <rate>     ROC threshold in units/second (default: 1000)\n"
        "  --detect-corr <r>       correlation threshold (default: 0.5)\n"
        "  --detect-mismatch-thresh <d> mismatch threshold (default: 5.0)\n"
        "\n"
        "Replay:\n"
        "  --replay <path>         replay a binary log instead of stdin\n"
        "  --replay-speed <mode>   max (default), real, or a positive factor\n"
        "  --replay-from <ts_ns>   start at this timestamp\n"
        "  --replay-to <ts_ns>     stop before this timestamp\n"
        "  --replay-loop           replay until interrupted\n"
        "  --replay-progress <n>   print progress every n packets\n",
        prog, prog);
}

/* ---- size parsing ---- */

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
        if (*end) return -1;
    }
    if (v * (double)mult > 1e15) return -1;
    *out = (uint64_t)(v * (double)mult);
    return 0;
}

/* ---- pair parsing ---- */

/* Parse "a,b" into two uint16s. Returns 0 on success. */
static int parse_pair(const char *s, uint16_t *a, uint16_t *b) {
    if (!s) return -1;
    char *comma = strchr(s, ',');
    if (!comma) return -1;
    char buf[16];
    size_t n = (size_t)(comma - s);
    if (n == 0 || n >= sizeof(buf)) return -1;
    memcpy(buf, s, n);
    buf[n] = 0;
    char *end = NULL;
    long va = strtol(buf, &end, 10);
    if (end == buf || *end != 0 || va < 0 || va > 0xFFFF) return -1;
    long vb = strtol(comma + 1, &end, 10);
    if (end == comma + 1 || *end != 0 || vb < 0 || vb > 0xFFFF) return -1;
    *a = (uint16_t)va;
    *b = (uint16_t)vb;
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

static void print_baseline_stats(const baseline_t *b, const char *path) {
    if (!b) return;
    uint16_t total = baseline_channel_count(b);
    uint16_t ready = 0;
    for (uint16_t slot = 0; slot < total; slot++) {
        int any_ready = 0;
        for (uint16_t r = 0; r < BASELINE_MAX_REGIMES; r++) {
            const baseline_stat_t *s = &b->stats[slot][r];
            if (s->state == BASELINE_READY) { any_ready = 1; break; }
        }
        if (any_ready) ready++;
    }
    fprintf(stderr,
        "[baseline] path=%s channels=%u ready=%u regimes=%u\n",
        path ? path : "(memory)",
        (unsigned)total,
        (unsigned)ready,
        (unsigned)baseline_regime_count(b));
}

/* Print per-detector event counts from the emitted counter. Since we
 * don't track per-detector counts in detect_t, we track them in main
 * and pass them in. */
typedef struct {
    uint64_t range;
    uint64_t zscore;
    uint64_t flatline;
    uint64_t roc;
    uint64_t correlation;
    uint64_t mismatch;
    uint64_t unknown;
    uint64_t total;
} event_counts_t;

static void count_event(event_counts_t *c, detector_t d) {
    switch (d) {
        case DETECTOR_RANGE:       c->range++;       break;
        case DETECTOR_ZSCORE:      c->zscore++;      break;
        case DETECTOR_FLATLINE:    c->flatline++;    break;
        case DETECTOR_ROC:         c->roc++;         break;
        case DETECTOR_CORRELATION: c->correlation++; break;
        case DETECTOR_MISMATCH:    c->mismatch++;    break;
        default:                   c->unknown++;     break;
    }
    c->total++;
}

static void print_detect_stats(const event_counts_t *c,
                               const detect_t *det) {
    fprintf(stderr,
        "[detect] events=%llu range=%llu zscore=%llu flatline=%llu "
        "roc=%llu correlation=%llu mismatch=%llu dropped=%llu\n",
        (unsigned long long)c->total,
        (unsigned long long)c->range,
        (unsigned long long)c->zscore,
        (unsigned long long)c->flatline,
        (unsigned long long)c->roc,
        (unsigned long long)c->correlation,
        (unsigned long long)c->mismatch,
        (unsigned long long)detect_events_dropped(det));
}

/* ---- baseline runtime state ---- */

typedef struct {
    baseline_t *model;
    char        path[256];
    uint64_t    last_decay_ns;
    uint64_t    last_save_ns;
    uint64_t    save_interval_ns;
} baseline_runtime_t;

static void maybe_update_baseline(baseline_runtime_t *br,
                                  const packet_t *p) {
    if (!br->model) return;
    baseline_update(br->model, p);
}

static void tick_baseline(baseline_runtime_t *br, uint64_t now_ns) {
    if (!br->model) return;

    uint64_t decay_interval_ns =
        (uint64_t)BASELINE_DECAY_INTERVAL_MS * 1000000ull;
    if (now_ns >= br->last_decay_ns &&
        now_ns - br->last_decay_ns >= decay_interval_ns) {
        baseline_decay(br->model, now_ns);
        br->last_decay_ns = now_ns;
    }

    if (br->save_interval_ns > 0 &&
        now_ns >= br->last_save_ns &&
        now_ns - br->last_save_ns >= br->save_interval_ns) {
        if (baseline_save(br->model, br->path) != 0) {
            plat_log(LOG_WARN, "baseline: periodic save failed");
        } else {
            plat_log(LOG_DEBUG, "baseline: periodic save ok");
        }
        br->last_save_ns = now_ns;
    }
}

/* ---- events sink ---- */

typedef struct {
    plat_file_t *fp;      /* NULL if --events not given or open failed */
    char         path[256];
    uint64_t     written;
    uint64_t     failed;
} events_sink_t;

static void events_write(events_sink_t *sink,
                         const event_t *e,
                         event_counts_t *counts) {
    count_event(counts, (detector_t)e->detector);

    /* Always echo a compact one-liner to stderr so the user sees
     * events even without --events. Only echo STARTED (CLEARED is
     * noise). */
    if (e->state == EVENT_STATE_STARTED) {
        fprintf(stderr,
            "[event] t=%llu ch=%u det=%s sev=%s obs=%g thresh=%g\n",
            (unsigned long long)event_get_timestamp(e),
            (unsigned)e->channel_id,
            detector_name((detector_t)e->detector),
            severity_name((severity_t)e->severity),
            (double)e->observed,
            (double)e->threshold);
    }

    if (!sink->fp) return;

    /* Build a human-readable message for the JSONL line. */
    char msg[128];
    if (e->state == EVENT_STATE_STARTED) {
        snprintf(msg, sizeof(msg),
                 "%s detector fired on channel %u (observed=%g threshold=%g)",
                 detector_name((detector_t)e->detector),
                 (unsigned)e->channel_id,
                 (double)e->observed,
                 (double)e->threshold);
    } else {
        snprintf(msg, sizeof(msg),
                 "%s detector cleared on channel %u",
                 detector_name((detector_t)e->detector),
                 (unsigned)e->channel_id);
    }

    char line[512];
    int n = event_to_json(e, msg, line, sizeof(line));
    if (n <= 0 || n >= (int)sizeof(line)) {
        sink->failed++;
        return;
    }
    if (plat_file_write(sink->fp, line, (size_t)n) != n ||
        plat_file_write(sink->fp, "\n", 1) != 1) {
        sink->failed++;
        return;
    }
    sink->written++;
}

static void events_flush(events_sink_t *sink) {
    if (sink->fp) plat_file_sync(sink->fp);
}

static void events_close(events_sink_t *sink) {
    if (sink->fp) {
        plat_file_sync(sink->fp);
        plat_file_close(sink->fp);
        sink->fp = NULL;
    }
}

/* ---- detector runtime ---- */

typedef struct {
    detect_t *det;
    policy_engine_t policy;
    bool policy_enabled;
    bool policy_dry_run;
    event_counts_t counts;
} detect_runtime_t;

/* Run detectors on one packet, write events to sink, update counts. */
static void maybe_run_detectors(detect_runtime_t *dr,
                                const packet_t *p,
                                const ingest_t *ing,
                                const baseline_t *baseline,
                                const registry_t *reg,
                                events_sink_t *sink) {
    if (!dr->det) return;

    event_t events[DETECT_EVENT_BUF];
    int n = detect_update(dr->det, p, ing, baseline, reg, events, DETECT_EVENT_BUF);
    if (n <= 0) return;
    
    for (int i = 0; i < n; i++) {
        events_write(sink, &events[i], &dr->counts);

        /* Evaluate policies if enabled */
        if (dr->policy_enabled) {
            const policy_rule_t *fired[POLICY_MAX_RULES];
            int num_fired = policy_evaluate(&dr->policy, &events[i], event_get_timestamp(&events[i]), fired, POLICY_MAX_RULES);
            
            for (int j = 0; j < num_fired; j++) {
                policy_execute(fired[j], &events[i], dr->policy_dry_run);
            }
        }
    }
}

/* ---- packet handler ---- */

static int handle_packet(ingest_t *ing, log_writer_t *lw,
                         baseline_runtime_t *br,
                         detect_runtime_t *dr,
                         events_sink_t *sink,
                         const packet_t *p,
                         bool *log_failed_once) {
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

    maybe_update_baseline(br, p);

    maybe_run_detectors(dr, p, ing,
                        br->model,
                        &ing->registry,
                        sink);
    return 0;
}

/* ---- stdin JSONL mode ---- */

static int run_stdin_mode(ingest_t *ing, log_writer_t *lw,
                          baseline_runtime_t *br,
                          detect_runtime_t *dr,
                          events_sink_t *sink,
                          unsigned long stats_every) {
    plat_log(LOG_INFO,
             "gardener 0.1.0 (D5): reading JSONL packets from stdin%s%s%s",
             lw ? " (log)" : "",
             br->model ? " (baseline)" : "",
             dr->det ? " (detect)" : "");

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
        if (handle_packet(ing, lw, br, dr, sink, &p, &log_failed) < 0) {
            bad++;
            plat_log(LOG_WARN, "ingest rejected packet on line %llu", lines);
            continue;
        }

        tick_baseline(br, p.timestamp_ns);

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

static int run_replay_mode(ingest_t *ing, log_writer_t *lw,
                           baseline_runtime_t *br,
                           detect_runtime_t *dr,
                           events_sink_t *sink,
                           const replay_opts_t *opts,
                           unsigned long stats_every) {
    plat_log(LOG_INFO,
             "gardener 0.1.0 (D5): replaying %s%s%s%s",
             opts->log_path,
             lw ? " (log)" : "",
             br->model ? " (baseline)" : "",
             dr->det ? " (detect)" : "");

    /* If nothing is wired to ingest except ingest itself, use the
     * plain replay_run. Otherwise we need our own loop so we can
     * interpose log/baseline/detect on each packet. */
    if (!lw && !br->model && !dr->det) {
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

        if (handle_packet(ing, lw, br, dr, sink, &p, &log_failed) < 0) {
            log_reader_close(r);
            return 1;
        }
        packets_fed++;

        tick_baseline(br, p.timestamp_ns);

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
    const char *baseline_path = NULL;
    const char *events_path = NULL;
    uint64_t    log_max = LOG_DEFAULT_MAX;
    bool        log_fresh = false;
    bool        baseline_fresh = false;
    uint64_t    baseline_halflife_s = 300;
    uint64_t    baseline_save_every_s = 0;
    int verbose = 0;
    int quiet = 0;
    int detect_enabled = 0;
    unsigned long stats_every = 0;

    /* Detector config to build from flags. */
    detect_config_t dcfg;
    memset(&dcfg, 0, sizeof(dcfg));
    dcfg.enable_range = true;
    dcfg.enable_zscore = true;
    dcfg.enable_flatline = true;
    dcfg.enable_roc = true;
    dcfg.enable_correlation = true;
    dcfg.enable_mismatch = true;
    dcfg.zscore_threshold = DETECT_DEFAULT_ZSCORE_THRESHOLD;
    dcfg.roc_absolute_max = DETECT_DEFAULT_ROC_ABSOLUTE_MAX;
    dcfg.roc_factor = DETECT_DEFAULT_ROC_FACTOR;
    dcfg.correlation_threshold = DETECT_DEFAULT_CORR_THRESHOLD;
    dcfg.mismatch_sustained = DETECT_DEFAULT_MISMATCH_SUSTAINED;
    dcfg.correlation_pair_count = 0;
    dcfg.mismatch_pair_count = 0;

    replay_opts_t replay_opts;
    replay_opts_default(&replay_opts);

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if      (!strcmp(a, "--version")) { puts("gardener 0.1.0 (phase E3)"); return 0; }        else if (!strcmp(a, "--verbose")) { verbose = 1; }
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
        else if (!strcmp(a, "--log-fresh")) { log_fresh = true; }
        else if (!strcmp(a, "--baseline") && i + 1 < argc) {
            baseline_path = argv[++i];
        }
        else if (!strcmp(a, "--baseline-fresh")) { baseline_fresh = true; }
        else if (!strcmp(a, "--baseline-halflife") && i + 1 < argc) {
            baseline_halflife_s = strtoull(argv[++i], NULL, 10);
        }
        else if (!strcmp(a, "--baseline-save-every") && i + 1 < argc) {
            baseline_save_every_s = strtoull(argv[++i], NULL, 10);
        }
        else if (!strcmp(a, "--detect")) { detect_enabled = 1; }
        else if (!strcmp(a, "--events") && i + 1 < argc) {
            events_path = argv[++i];
        }
        else if (!strcmp(a, "--detect-pair") && i + 1 < argc) {
            uint16_t ca, cb;
            if (parse_pair(argv[++i], &ca, &cb) != 0) {
                fprintf(stderr, "bad --detect-pair: %s\n", argv[i]);
                return 2;
            }
            if (dcfg.correlation_pair_count >= DETECT_MAX_PAIRS) {
                fprintf(stderr, "too many --detect-pair (max %d)\n",
                        DETECT_MAX_PAIRS);
                return 2;
            }
            dcfg.correlation_pairs[dcfg.correlation_pair_count].ch_a = ca;
            dcfg.correlation_pairs[dcfg.correlation_pair_count].ch_b = cb;
            dcfg.correlation_pair_count++;
        }
        else if (!strcmp(a, "--detect-mismatch") && i + 1 < argc) {
            uint16_t cc, aa;
            if (parse_pair(argv[++i], &cc, &aa) != 0) {
                fprintf(stderr, "bad --detect-mismatch: %s\n", argv[i]);
                return 2;
            }
            if (dcfg.mismatch_pair_count >= DETECT_MAX_MISMATCH_PAIRS) {
                fprintf(stderr, "too many --detect-mismatch (max %d)\n",
                        DETECT_MAX_MISMATCH_PAIRS);
                return 2;
            }
            dcfg.mismatch_pairs[dcfg.mismatch_pair_count].ctrl_out = cc;
            dcfg.mismatch_pairs[dcfg.mismatch_pair_count].actuator = aa;
            dcfg.mismatch_pairs[dcfg.mismatch_pair_count].threshold =
                DETECT_DEFAULT_MISMATCH_THRESHOLD;
            dcfg.mismatch_pair_count++;
        }
        else if (!strcmp(a, "--detect-zscore") && i + 1 < argc) {
            dcfg.zscore_threshold = (float)atof(argv[++i]);
        }
        else if (!strcmp(a, "--detect-roc") && i + 1 < argc) {
            dcfg.roc_absolute_max = (float)atof(argv[++i]);
        }
        else if (!strcmp(a, "--detect-corr") && i + 1 < argc) {
            dcfg.correlation_threshold = (float)atof(argv[++i]);
        }
        else if (!strcmp(a, "--detect-mismatch-thresh") && i + 1 < argc) {
            /* Applies to all mismatch pairs already added. */
            float t = (float)atof(argv[++i]);
            for (uint8_t k = 0; k < dcfg.mismatch_pair_count; k++) {
                dcfg.mismatch_pairs[k].threshold = t;
            }
        }
        else if (!strcmp(a, "--replay") && i + 1 < argc) {
            replay_path = argv[++i];
            replay_opts.log_path = replay_path;
        }
        else if (!strcmp(a, "--replay-speed") && i + 1 < argc) {
            const char *s = argv[++i];
            if (!strcmp(s, "max"))  { replay_opts.speed_mode = REPLAY_SPEED_MAX; replay_opts.speed_factor = 1.0; }
            else if (!strcmp(s, "real")) { replay_opts.speed_mode = REPLAY_SPEED_REAL; replay_opts.speed_factor = 1.0; }
            else {
                double f = atof(s);
                if (f <= 0.0) {
                    fprintf(stderr, "bad --replay-speed: %s\n", s);
                    return 2;
                }
                replay_opts.speed_mode = REPLAY_SPEED_SCALED;
                replay_opts.speed_factor = f;
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
        else if (!strcmp(a, "--policy") && i + 1 < argc) {
            policy_path = argv[++i];
        }
        else if (!strcmp(a, "--policy-dry-run")) {
            policy_dry_run = true;
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

    /* Log writer, optional. */
    log_writer_t *lw = NULL;
    if (log_path) {
        lw = log_writer_open(log_path, log_max, &ing.registry, log_fresh);
        if (!lw) {
            plat_log(LOG_WARN,
                     "log writer failed to open %s; continuing without logging",
                     log_path);
        }
    }

    /* Baseline, optional. */
    static baseline_t baseline_model;
    baseline_runtime_t br;
    memset(&br, 0, sizeof(br));

    if (baseline_path) {
        uint64_t halflife_ns = baseline_halflife_s * 1000000000ull;

        if (!baseline_fresh) {
            if (baseline_load(&baseline_model, baseline_path,
                              &ing.registry) == 0) {
                plat_log(LOG_INFO, "baseline: loaded %s", baseline_path);
            } else {
                plat_log(LOG_INFO,
                         "baseline: could not load %s; starting fresh",
                         baseline_path);
                if (baseline_init(&baseline_model, &ing.registry,
                                  halflife_ns) != 0) {
                    plat_log(LOG_ERROR, "baseline_init failed");
                    if (lw) log_writer_close(lw);
                    return 1;
                }
            }
        } else {
            if (baseline_init(&baseline_model, &ing.registry,
                              halflife_ns) != 0) {
                plat_log(LOG_ERROR, "baseline_init failed");
                if (lw) log_writer_close(lw);
                return 1;
            }
        }

        br.model = &baseline_model;
        snprintf(br.path, sizeof(br.path), "%s", baseline_path);
        br.last_decay_ns = 0;
        br.last_save_ns  = plat_wall_ns();
        br.save_interval_ns = baseline_save_every_s * 1000000000ull;

        plat_log(LOG_INFO,
                 "baseline: halflife=%llus channels=%u regimes=%u",
                 (unsigned long long)baseline_halflife_s,
                 (unsigned)baseline_channel_count(&baseline_model),
                 (unsigned)baseline_regime_count(&baseline_model));
    }

    /* Detector runtime, optional. */
    static detect_t detect_model;
    detect_runtime_t dr;
    memset(&dr, 0, sizeof(dr));
    if (detect_enabled) {
        if (detect_init(&detect_model, &dcfg) != 0) {
            plat_log(LOG_ERROR, "detect_init failed");
            if (lw) log_writer_close(lw);
            return 1;
        }
        dr.det = &detect_model;

        plat_log(LOG_INFO,
                 "detect: pairs=%u mismatch_pairs=%u z=%g roc=%g corr=%g",
                 (unsigned)dcfg.correlation_pair_count,
                 (unsigned)dcfg.mismatch_pair_count,
                 (double)dcfg.zscore_threshold,
                 (double)dcfg.roc_absolute_max,
                 (double)dcfg.correlation_threshold);
    }

    /* --- ADD THIS BLOCK --- */
    /* Policy engine, optional. */
    if (detect_enabled && policy_path) {
        if (policy_load_toml(&dr.policy, policy_path) != 0) {
            plat_log(LOG_ERROR, "failed to load policy %s", policy_path);
            if (lw) log_writer_close(lw);
            return 1;
        }
        dr.policy_enabled = true;
        dr.policy_dry_run = policy_dry_run;

        plat_log(LOG_INFO, "policy: loaded %d rules from %s%s",
                 dr.policy.rule_count, policy_path,
                 dr.policy_dry_run ? " (DRY-RUN MODE)" : "");
    }
    /* ---------------------- */

    /* Events sink, optional. */
    static events_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    if (events_path) {
        sink.fp = plat_file_open(events_path, PLAT_FILE_WRITE);
        if (!sink.fp) {
            plat_log(LOG_WARN,
                     "events: cannot open %s; events will not be written to file",
                     events_path);
        } else {
            snprintf(sink.path, sizeof(sink.path), "%s", events_path);
            plat_log(LOG_INFO, "events: writing to %s", events_path);
        }
    }

    int rc;
    if (replay_path) {
        rc = run_replay_mode(&ing, lw, &br, &dr, &sink, &replay_opts,
                             stats_every);
    } else {
        rc = run_stdin_mode(&ing, lw, &br, &dr, &sink, stats_every);
    }

    print_ingest_stats(&ing);
    if (lw) {
        print_log_stats(lw);
    } else if (log_path) {
        fprintf(stderr, "[log] disabled (open failed)\n");
    }
    if (br.model) {
        if (baseline_save(br.model, br.path) != 0) {
            plat_log(LOG_WARN, "baseline: final save failed");
            fprintf(stderr, "[baseline] final save FAILED\n");
        }
        print_baseline_stats(br.model, br.path);
    }
    if (dr.det) {
        print_detect_stats(&dr.counts, dr.det);
    }
    if (sink.fp) {
        events_flush(&sink);
        fprintf(stderr, "[events] written=%llu failed=%llu path=%s\n",
                (unsigned long long)sink.written,
                (unsigned long long)sink.failed,
                sink.path);
        events_close(&sink);
    }

    if (lw) log_writer_close(lw);
    return rc;
}