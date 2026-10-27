#ifndef GARDENER_DETECT_H
#define GARDENER_DETECT_H

#include "gardener/baseline.h"
#include "gardener/event.h"
#include "gardener/packet.h"
#include "gardener/registry.h"
#include "gardener/ingest.h"

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
 * Per-channel state is kept for each detector type: "is this channel
 * currently in violation for this detector?". If yes, no new event
 * fires. If no and the condition becomes true, one STARTED event.
 * Same for the transition back.
 *
 * Two detectors implemented in D1:
 *   - range:     observed value outside [min,max] from the registry
 *   - zscore:    |observed - baseline.mean| > zscore_threshold * stddev
 *
 * Z-score requires a READY baseline for the channel's current regime.
 * If the baseline is EMPTY / LEARNING / STALE, z-score does not fire.
 * This prevents false positives during the initial learning period.
 * ------------------------------------------------------------------- */

#define DETECT_EVENT_BUF      32   /* max events per detect_update call */
#define DETECT_MAX_CHANNELS   256  /* matches REGISTRY_MAX_CHANNELS */

/* Threshold defaults. */
#define DETECT_DEFAULT_ZSCORE_THRESHOLD  5.0f
#define DETECT_DEFAULT_ROC_THRESHOLD     1.0f  /* reserved for D2 */

#define DETECT_FLATLINE_WINDOW   32     /* samples for flatline check */
#define DETECT_FLATLINE_EPSILON  1e-6f  /* values closer than this are "same" */
#define DETECT_ROC_WINDOW        16     /* pairs for rate-of-change check */
#define DETECT_DEFAULT_ROC_ABSOLUTE_MAX  1000.0f   /* per-second fallback */
#define DETECT_DEFAULT_ROC_FACTOR        10.0f     /* × normal rate */

/* ---- configuration ------------------------------------------------- */

typedef struct {
    /* Which detectors are enabled. Zero disables; nonzero enables.
     * Range and zscore default to enabled. */
    bool enable_range;
    bool enable_zscore;

    /* Z-score threshold in standard deviations. Ignored if
     * enable_zscore is false. */
    float zscore_threshold;

    /* Used only for logging and event generation; set by the caller
     * from the source_id field of the packet or a config file. */
    uint16_t source_id;

    bool enable_flatline;
    bool enable_roc;

    float roc_absolute_max;   /* per second; used when no baseline rate */
    float roc_factor;         /* × learned rate; used when available */
} detect_config_t;

/* ---- per-channel state -------------------------------------------- */

typedef struct {
    /* Range detector: is the value currently outside [min,max]? */
    bool range_in_violation;

    /* Z-score detector: is the value currently outside the threshold? */
    bool zscore_in_violation;

    bool flatline_in_violation;
    bool roc_in_violation;
} detect_channel_state_t;

/* ---- detector instance -------------------------------------------- */

typedef struct {
    detect_config_t config;

    /* Per-channel state. Indexed by channel id. */
    detect_channel_state_t state[DETECT_MAX_CHANNELS];

    /* For emitting z-score events, we need the last mean/stddev at
     * fire time. Not used for STARTED (the baseline is fresh at that
     * point) but needed for CLEARED events so the dashboard can see
     * what the threshold was. */
    float last_mean[DETECT_MAX_CHANNELS];
    float last_stddev[DETECT_MAX_CHANNELS];
    float last_threshold[DETECT_MAX_CHANNELS];

    /* Counters for diagnostics. */
    uint64_t events_emitted;
    uint64_t events_dropped;   /* if buffer was full */
} detect_t;

/* ---- lifecycle ---------------------------------------------------- */

/* Initialize a detector with default config (range and zscore
 * enabled, zscore_threshold = 5.0). Returns 0 on success, -1 on
 * NULL. */
int detect_init(detect_t *det, const detect_config_t *cfg);

/* Convenience: init with defaults (both detectors enabled, z=5). */
int detect_init_default(detect_t *det, uint16_t source_id);

/* Reset all per-channel state. Useful when switching regimes or
 * after loading a different baseline. Does not touch config. */
void detect_reset(detect_t *det);

/* ---- update ------------------------------------------------------- */

/* Process one packet. For each sample:
 *   - if the channel is in a range violation, emit CLEARED if the
 *     value is back inside [min,max] (and it was previously in
 *     violation), else emit STARTED if it just went outside
 *   - same for z-score
 *
 * Events are appended to `events` (up to `cap`). The number of
 * events appended is returned, or -1 on error.
 *
 * baseline may be NULL, in which case z-score does nothing.
 * registry is required for range checks (needs min/max).
 *
 * The `msg` field of each event is set by the detector. The caller
 * does not need to fill it in.
 */
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