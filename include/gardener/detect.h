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
 * vice versa (CLEARED).
 *
 * Six detectors:
 *   - range        observed value outside [min,max] from the registry
 *   - zscore       |observed - baseline.mean| > threshold * stddev
 *   - flatline     same value over a window (frozen sensor)
 *   - roc          rate of change too fast (fast-moving value)
 *   - correlation  two channels that normally co-vary have decoupled
 *   - mismatch     (reserved; D4)
 *
 * All detectors follow the same lifecycle: one STARTED when a
 * condition becomes true, no further events while it persists, one
 * CLEARED when it becomes false.
 * ------------------------------------------------------------------- */

#define DETECT_EVENT_BUF               32
#define DETECT_MAX_CHANNELS            256
#define DETECT_FLATLINE_WINDOW         32
#define DETECT_FLATLINE_EPSILON        1e-6f
#define DETECT_ROC_WINDOW              16
#define DETECT_CORR_WINDOW             32
#define DETECT_MAX_PAIRS               8

#define DETECT_DEFAULT_ZSCORE_THRESHOLD   5.0f
#define DETECT_DEFAULT_ROC_ABSOLUTE_MAX   1000.0f
#define DETECT_DEFAULT_ROC_FACTOR         10.0f
#define DETECT_DEFAULT_CORR_THRESHOLD     0.5f

/* ---- configuration ------------------------------------------------- */

typedef struct {
    bool enable_range;
    bool enable_zscore;
    bool enable_flatline;
    bool enable_roc;
    bool enable_correlation;

    float zscore_threshold;
    float roc_absolute_max;
    float roc_factor;
    float correlation_threshold;

    struct {
        uint16_t ch_a;
        uint16_t ch_b;
    } correlation_pairs[DETECT_MAX_PAIRS];
    uint8_t correlation_pair_count;

    uint16_t source_id;
} detect_config_t;

/* ---- per-pair state for correlation ------------------------------- */

typedef struct {
    bool in_violation;
} detect_pair_state_t;

/* ---- per-channel state -------------------------------------------- */

typedef struct {
    bool range_in_violation;
    bool zscore_in_violation;
    bool flatline_in_violation;
    bool roc_in_violation;
} detect_channel_state_t;

/* ---- detector instance -------------------------------------------- */

typedef struct {
    detect_config_t config;

    detect_channel_state_t state[DETECT_MAX_CHANNELS];
    detect_pair_state_t    pair_state[DETECT_MAX_PAIRS];

    float last_mean[DETECT_MAX_CHANNELS];
    float last_stddev[DETECT_MAX_CHANNELS];
    float last_threshold[DETECT_MAX_CHANNELS];

    uint64_t events_emitted;
    uint64_t events_dropped;
} detect_t;

/* ---- lifecycle ---------------------------------------------------- */

int detect_init(detect_t *det, const detect_config_t *cfg);
int detect_init_default(detect_t *det, uint16_t source_id);
void detect_reset(detect_t *det);

/* ---- update ------------------------------------------------------- */

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