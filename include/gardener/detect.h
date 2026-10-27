#ifndef GARDENER_DETECT_H
#define GARDENER_DETECT_H

#include "gardener/baseline.h"
#include "gardener/event.h"
#include "gardener/ingest.h"
#include "gardener/packet.h"
#include "gardener/registry.h"

#include <stdbool.h>
#include <stdint.h>

/* ---------------------------------------------------------------------
 * Detectors.
 *
 * A detector watches a stream of packets and emits events when a
 * condition transitions from "not active" to "active" (STARTED) or
 * vice versa (CLEARED). No ONGOING events in v1: one STARTED per
 * activation, one CLEARED per deactivation.
 *
 * Six detectors:
 *   - range        observed value outside [min,max] from the registry
 *   - zscore       |observed - baseline.mean| > threshold * stddev
 *   - flatline     same value over a window (frozen sensor)
 *   - roc          rate of change too fast (fast-moving value)
 *   - correlation  two channels that normally co-vary have decoupled
 *   - mismatch     (ctrl_out, actuator) pair diverged too long
 *
 * All detectors follow the same lifecycle: one STARTED per episode,
 * one CLEARED per deactivation, no ONGOING in between.
 * ------------------------------------------------------------------- */

/* ---- constants ---------------------------------------------------- */

#define DETECT_EVENT_BUF                32     /* max events per update */
#define DETECT_MAX_CHANNELS             256    /* matches REGISTRY_MAX_CHANNELS */

#define DETECT_FLATLINE_WINDOW          32     /* samples for flatline check */
#define DETECT_FLATLINE_EPSILON         1e-6f  /* values closer than this are "same" */

#define DETECT_ROC_WINDOW               16     /* pairs for rate-of-change check */

#define DETECT_CORR_WINDOW              32     /* samples per pair correlation */
#define DETECT_MAX_PAIRS                8      /* max correlation pairs */

#define DETECT_MAX_MISMATCH_PAIRS       8      /* max mismatch pairs */

/* Threshold defaults. */
#define DETECT_DEFAULT_ZSCORE_THRESHOLD    5.0f
#define DETECT_DEFAULT_ROC_ABSOLUTE_MAX    1000.0f   /* units per second */
#define DETECT_DEFAULT_ROC_FACTOR          10.0f     /* × normal rate (reserved) */
#define DETECT_DEFAULT_CORR_THRESHOLD      0.5f      /* fire when r < this */
#define DETECT_DEFAULT_MISMATCH_THRESHOLD  5.0f      /* abs(ctrl - act) */
#define DETECT_DEFAULT_MISMATCH_SUSTAINED  10        /* consecutive samples */

/* ---- configuration ------------------------------------------------ */

typedef struct {
    bool enable_range;
    bool enable_zscore;
    bool enable_flatline;
    bool enable_roc;
    bool enable_correlation;
    bool enable_mismatch;

    float zscore_threshold;
    float roc_absolute_max;
    float roc_factor;
    float correlation_threshold;

    struct {
        uint16_t ch_a;
        uint16_t ch_b;
    } correlation_pairs[DETECT_MAX_PAIRS];
    uint8_t correlation_pair_count;

    struct {
        uint16_t ctrl_out;
        uint16_t actuator;
        float    threshold;
    } mismatch_pairs[DETECT_MAX_MISMATCH_PAIRS];
    uint8_t mismatch_pair_count;

    uint8_t mismatch_sustained;   /* consecutive samples needed to fire */

    uint16_t source_id;
} detect_config_t;

/* ---- per-pair state for correlation (D3) -------------------------- */

typedef struct {
    bool in_violation;
} detect_pair_state_t;

/* ---- per-pair state for mismatch (D4) ----------------------------- */

typedef struct {
    uint16_t consecutive;    /* samples in a row over threshold */
    bool     in_violation;
} detect_mismatch_state_t;

/* ---- per-channel state (D1–D2) ------------------------------------ */

typedef struct {
    bool range_in_violation;
    bool zscore_in_violation;
    bool flatline_in_violation;
    bool roc_in_violation;
} detect_channel_state_t;

/* ---- detector instance -------------------------------------------- */

typedef struct {
    detect_config_t config;

    /* Per-channel state. Indexed by channel id. */
    detect_channel_state_t state[DETECT_MAX_CHANNELS];

    /* Per-pair state. Indexed by pair index (position in config arrays). */
    detect_pair_state_t     pair_state[DETECT_MAX_PAIRS];
    detect_mismatch_state_t mismatch_state[DETECT_MAX_MISMATCH_PAIRS];

    /* For emitting z-score events: baseline stats at fire time. */
    float last_mean[DETECT_MAX_CHANNELS];
    float last_stddev[DETECT_MAX_CHANNELS];
    float last_threshold[DETECT_MAX_CHANNELS];

    /* Counters for diagnostics. */
    uint64_t events_emitted;
    uint64_t events_dropped;   /* if buffer was full */
} detect_t;

/* ---- lifecycle ---------------------------------------------------- */

/* Initialize a detector with explicit config. Returns 0 on success,
 * -1 on NULL det. If cfg is NULL, uses defaults (all detectors enabled
 * with no pairs, thresholds at their DETECT_DEFAULT values). */
int detect_init(detect_t *det, const detect_config_t *cfg);

/* Convenience: init with defaults. All detectors enabled, no correlation
 * or mismatch pairs (so those detectors do nothing until configured). */
int detect_init_default(detect_t *det, uint16_t source_id);

/* Reset all per-channel and per-pair state. Useful when switching
 * baselines or after reloading config. Does not touch thresholds or
 * enabled flags. */
void detect_reset(detect_t *det);

/* ---- update ------------------------------------------------------- */

/* Process one packet. Runs every enabled detector against the packet's
 * samples (and against ingest/baseline state where needed), and appends
 * any resulting events to `events` (up to `cap`). Returns the number of
 * events appended, or -1 on error.
 *
 * Arguments:
 *   det       — initialized detector
 *   p         — current packet
 *   ing       — required; per-channel history for flatline/roc/correlation
 *   baseline  — optional; needed for zscore (NULL disables zscore)
 *   registry  — optional; needed for range (NULL disables range)
 *   events    — caller-provided buffer for events (fixed size, up to cap)
 *   cap       — max events to append
 *
 * Events beyond `cap` are dropped and counted in `events_dropped`. */
int detect_update(detect_t *det,
                  const packet_t *p,
                  const ingest_t *ing,
                  const baseline_t *baseline,
                  const registry_t *registry,
                  event_t *events, size_t cap);

/* ---- introspection ------------------------------------------------ */

uint64_t detect_events_emitted(const detect_t *det);
uint64_t detect_events_dropped(const detect_t *det);

#endif /* GARDENER_DETECT_H */