#include "gardener/replay.h"
#include "gardener/platform.h"
#include "gardener/packet.h"

#include <signal.h>
#include <string.h>

/* ---------------------------------------------------------------------
 * Replay engine.
 *
 * Timing model:
 *   - MAX: as fast as possible, no sleep.
 *   - REAL: sleep between packets to match recorded intervals.
 *   - SCALED: sleep scaled by 1/factor.
 *
 * For REAL and SCALED, the first packet is emitted immediately, then
 * the engine waits (t[i] - t[i-1]) / speed before emitting packet i.
 * The reader's timestamps are monotonic within a single loop but reset
 * on loop iteration; the pacer handles that by treating each loop as a
 * fresh sequence.
 *
 * Time management:
 *   - plat_now_ns() is used for elapsed-time tracking. It is monotonic,
 *     so sleeps don't drift on wall-clock adjustment.
 *   - plat_sleep_ms() sleeps in millisecond increments. Sub-ms sleeps
 *     are rounded to the nearest ms, which is adequate for 10 Hz
 *     replay. When Phase C needs tighter pacing, this can be revised.
 * ------------------------------------------------------------------- */

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

void replay_request_stop(void) {
    g_stop = 1;
}

void replay_opts_default(replay_opts_t *opts) {
    if (!opts) return;
    memset(opts, 0, sizeof(*opts));
    opts->speed_mode  = REPLAY_SPEED_MAX;
    opts->speed_factor = 1.0;
}

/* Sleep for `ns` nanoseconds, best-effort. Uses plat_sleep_ms for the
 * whole milliseconds, then a busy loop for the remainder if the
 * remainder is small enough to be worth it (< 5 ms). For large
 * remainders, rounds to the nearest ms. */
static void sleep_ns(uint64_t ns) {
    if (ns == 0) return;
    uint64_t ms = ns / 1000000ull;
    uint64_t remainder_ns = ns % 1000000ull;
    if (ms > 0) plat_sleep_ms((unsigned)ms);
    if (remainder_ns > 0 && remainder_ns < 5000000ull) {
        /* Busy-wait the remainder. Small enough not to burn CPU. */
        uint64_t target = plat_now_ns() + remainder_ns;
        while (plat_now_ns() < target) { /* spin */ }
    }
}

/* Compute the delay between prev_ts and this_ts according to the
 * speed mode. Returns 0 if the mode requires no pacing. */
static uint64_t compute_delay_ns(const replay_opts_t *opts,
                                 uint64_t prev_ts, uint64_t this_ts) {
    if (opts->speed_mode == REPLAY_SPEED_MAX) return 0;
    if (this_ts <= prev_ts) return 0;   /* out of order or equal: no wait */
    uint64_t delta = this_ts - prev_ts;

    if (opts->speed_mode == REPLAY_SPEED_REAL) {
        return delta;
    }
    /* SCALED */
    double factor = opts->speed_factor;
    if (factor <= 0.0) return delta;   /* bad config, fall back to real */
    double scaled = (double)delta / factor;
    if (scaled < 0.0) return 0;
    if (scaled > 1.0e12) return (uint64_t)1.0e12;   /* cap at 1000s */
    return (uint64_t)scaled;
}

/* Feed one packet from the reader to ingest, honoring the window. */
static int feed_one(log_reader_t *r, ingest_t *ing, replay_stats_t *stats,
                    const replay_opts_t *opts, uint64_t *prev_ts,
                    bool *had_packet) {
    packet_t p;
    int rc = log_reader_next(r, &p);
    if (rc == 0) return 0;         /* end of log */
    if (rc < 0)  return -1;        /* error */

    /* Window filter. */
    if (opts->from_ts_ns && p.timestamp_ns < opts->from_ts_ns) {
        stats->packets_skipped++;
        return 1;   /* skipped, continue */
    }
    if (opts->to_ts_ns && p.timestamp_ns >= opts->to_ts_ns) {
        /* Past the window: signal end-of-run for this loop. */
        return 0;
    }

    /* Pacing. Skip the first packet in a loop when computing delays. */
    if (*had_packet) {
        uint64_t delay = compute_delay_ns(opts, *prev_ts, p.timestamp_ns);
        if (delay) sleep_ns(delay);
    }
    *prev_ts   = p.timestamp_ns;
    *had_packet = true;

    int n = ingest_feed(ing, &p);
    if (n < 0) return -1;
    stats->packets_fed++;
    stats->samples_fed += (uint64_t)n;

    if (opts->progress_every && (stats->packets_fed % opts->progress_every) == 0) {
        plat_log(LOG_INFO, "replay: %llu packets fed",
                 (unsigned long long)stats->packets_fed);
    }
    return 1;
}

int replay_run(const replay_opts_t *opts, ingest_t *ing, replay_stats_t *stats) {
    if (!opts || !opts->log_path || !ing || !stats) return -1;
    if (opts->speed_mode == REPLAY_SPEED_SCALED && opts->speed_factor <= 0.0) {
        plat_log(LOG_ERROR, "replay: invalid speed factor %g",
                 opts->speed_factor);
        return -1;
    }

    memset(stats, 0, sizeof(*stats));
    g_stop = 0;
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    log_reader_t *r = log_reader_open(opts->log_path);
    if (!r) {
        plat_log(LOG_ERROR, "replay: cannot open %s", opts->log_path);
        return -1;
    }

    uint64_t start_ns = plat_now_ns();

    /* If from_ts is set, use the index to seek. */
    if (opts->from_ts_ns) {
        if (log_reader_seek_ns(r, opts->from_ts_ns) != 0) {
            plat_log(LOG_ERROR, "replay: seek to %llu failed",
                     (unsigned long long)opts->from_ts_ns);
            log_reader_close(r);
            return -1;
        }
    }

    uint64_t prev_ts = 0;
    bool had_packet = false;

    for (;;) {
        if (g_stop) break;

        int rc = feed_one(r, ing, stats, opts, &prev_ts, &had_packet);
        if (rc < 0) {
            log_reader_close(r);
            return -1;
        }
        if (rc == 0) {
            /* End of log (or past the window). */
            if (!opts->loop) break;

            stats->total_loops++;
            if (opts->progress_every) {
                plat_log(LOG_INFO,
                         "replay: loop %llu complete (%llu packets total)",
                         (unsigned long long)stats->total_loops,
                         (unsigned long long)stats->packets_fed);
            }

            /* Rewind and seek again if needed. */
            log_reader_rewind(r);
            if (opts->from_ts_ns) {
                if (log_reader_seek_ns(r, opts->from_ts_ns) != 0) {
                    log_reader_close(r);
                    return -1;
                }
            }
            prev_ts = 0;
            had_packet = false;
        }
    }

    stats->duration_ns = plat_now_ns() - start_ns;
    log_reader_close(r);
    return 0;
}