#ifndef GARDENER_REPLAY_H
#define GARDENER_REPLAY_H

#include "gardener/ingest.h"
#include "gardener/log_reader.h"

#include <stdbool.h>
#include <stdint.h>

/* ---------------------------------------------------------------------
 * Replay engine.
 *
 * Reads packets from a binary log and feeds them into an ingest_t as if
 * they were arriving from a live source. Supports three timing modes
 * (max speed, real time, scaled), windowing (from/to timestamps), and
 * looping.
 *
 * The replay engine owns a log_reader. Callers provide an initialized
 * ingest_t. Replay does not touch the log file beyond reading it.
 * ------------------------------------------------------------------- */

typedef enum {
    REPLAY_SPEED_MAX    = 0,   /* no pacing, process as fast as possible */
    REPLAY_SPEED_REAL   = 1,   /* pace to match recorded timestamps */
    REPLAY_SPEED_SCALED = 2,   /* pace at factor × real time */
} replay_speed_t;

typedef struct {
    /* Path to the log file. Required. */
    const char *log_path;

    /* Timing mode. */
    replay_speed_t speed_mode;
    double         speed_factor;   /* used only for REPLAY_SPEED_SCALED; >0 */

    /* Window. Set from_ts/to_ts to 0 to disable either bound. */
    uint64_t from_ts_ns;
    uint64_t to_ts_ns;

    /* Loop until interrupted. */
    bool loop;

    /* Print a progress line to stderr every N packets. 0 = never. */
    uint64_t progress_every;
} replay_opts_t;

typedef struct {
    uint64_t packets_fed;
    uint64_t samples_fed;
    uint64_t packets_skipped;    /* filtered out by from/to */
    uint64_t total_loops;
    uint64_t duration_ns;         /* wall time spent in replay */
} replay_stats_t;

/* Initialize a set of default options. */
void replay_opts_default(replay_opts_t *opts);

/* Run replay until the log ends (or forever, if opts->loop is set).
 * Returns 0 on normal completion (or on SIGINT for looping mode),
 * -1 on error. The ingest_t is populated as if by live feed; callers
 * can query it after.
 *
 * On SIGINT during loop mode, the function returns 0 with stats
 * reflecting how far it got. */
int replay_run(const replay_opts_t *opts, ingest_t *ing, replay_stats_t *stats);

/* Register a SIGINT handler that causes replay_run to return. Called
 * automatically by replay_run; exposed so tests can install their own. */
void replay_request_stop(void);

#endif /* GARDENER_REPLAY_H */