#include "gardener/detect.h"
#include "gardener/platform.h"

#include <string.h>
#include <math.h>

/* ---------------------------------------------------------------------
 * Range and z-score detectors.
 *
 * The range detector uses the registry's min/max per channel. It
 * needs no baseline. It fires on the transition from "in range" to
 * "out of range" and clears on the reverse.
 *
 * The z-score detector compares each sample to the current baseline
 * for the channel's regime. It requires READY state (enough samples,
 * not decayed below threshold). It fires when |x - mean| > threshold
 * * stddev, clears when the value comes back within bounds.
 *
 * Both detectors keep per-channel state so they emit one STARTED and
 * one CLEARED per violation episode, not one event per sample.
 * ------------------------------------------------------------------- */

int detect_init(detect_t *det, const detect_config_t *cfg) {
    if (!det) return -1;
    memset(det, 0, sizeof(*det));
    if (cfg) {
        det->config = *cfg;
    } else {
        det->config.enable_range = true;
        det->config.enable_zscore = true;
        det->config.zscore_threshold = DETECT_DEFAULT_ZSCORE_THRESHOLD;
        det->config.source_id = 0;
    }
    return 0;
}

int detect_init_default(detect_t *det, uint16_t source_id) {
    if (!det) return -1;
    detect_config_t cfg = {
        .enable_range = true,
        .enable_zscore = true,
        .zscore_threshold = DETECT_DEFAULT_ZSCORE_THRESHOLD,
        .source_id = source_id,
    };
    return detect_init(det, &cfg);
}

void detect_reset(detect_t *det) {
    if (!det) return;
    memset(det->state, 0, sizeof(det->state));
    memset(det->last_mean, 0, sizeof(det->last_mean));
    memset(det->last_stddev, 0, sizeof(det->last_stddev));
    memset(det->last_threshold, 0, sizeof(det->last_threshold));
    det->events_emitted = 0;
    det->events_dropped = 0;
}

uint64_t detect_events_emitted(const detect_t *det) {
    return det ? det->events_emitted : 0;
}

uint64_t detect_events_dropped(const detect_t *det) {
    return det ? det->events_dropped : 0;
}

/* Helper: append an event to the output buffer, updating counters.
 * Returns 1 if appended, 0 if dropped due to cap. */
static int push_event(detect_t *det, event_t *out, size_t cap,
                      size_t *count, const event_t *e) {
    if (*count >= cap) {
        det->events_dropped++;
        return 0;
    }
    out[(*count)++] = *e;
    det->events_emitted++;
    return 1;
}

/* Check the range detector for one sample. Returns 1 if an event was
 * emitted, 0 if not. Appends to the event buffer via push_event. */
static int check_range(detect_t *det,
                       const sample_t *s,
                       const registry_t *reg,
                       const packet_t *p,
                       event_t *events, size_t cap, size_t *n_events) {
    if (!det->config.enable_range) return 0;
    if (!reg) return 0;

    /* Unknown channels have no min/max; skip. */
    const channel_desc_t *desc = registry_get(reg, s->channel_id);
    if (!desc || !desc->has_range) return 0;

    /* Skip invalid and stale samples. */
    if (s->flags & CH_FLAG_STALE) return 0;
    if (!(s->flags & CH_FLAG_VALID)) return 0;

    float v = sample_as_f32(s);
    bool outside = (v < desc->min_valid) || (v > desc->max_valid);
    bool was_in_violation = det->state[s->channel_id].range_in_violation;

    if (outside && !was_in_violation) {
        /* Transition into violation: emit STARTED. */
        event_t e;
        memset(&e, 0, sizeof(e));
        event_set_timestamp(&e, plat_wall_ns());
        event_set_source_ts(&e, p->timestamp_ns);
        e.channel_id   = s->channel_id;
        e.channel_id2  = EVENT_CHANNEL_NONE;
        e.regime       = EVENT_REGIME_NONE;
        e.observed     = v;
        e.baseline_mean = 0.0f;
        e.baseline_stddev = 0.0f;
        /* Threshold: the boundary that was crossed. Use the closer
         * of min/max as a "this is the line you crossed" hint. */
        e.threshold = (v < desc->min_valid) ? desc->min_valid
                                            : desc->max_valid;
        e.detector = DETECTOR_RANGE;
        e.severity = SEVERITY_WARN;
        e.state    = EVENT_STATE_STARTED;
        e.version  = EVENT_VERSION;

        push_event(det, events, cap, n_events, &e);
        det->state[s->channel_id].range_in_violation = true;
        return 1;
    }

    if (!outside && was_in_violation) {
        /* Transition out of violation: emit CLEARED. */
        event_t e;
        memset(&e, 0, sizeof(e));
        event_set_timestamp(&e, plat_wall_ns());
        event_set_source_ts(&e, p->timestamp_ns);
        e.channel_id   = s->channel_id;
        e.channel_id2  = EVENT_CHANNEL_NONE;
        e.regime       = EVENT_REGIME_NONE;
        e.observed     = v;
        e.threshold    = (v < desc->min_valid) ? desc->min_valid
                                              : desc->max_valid;
        e.detector = DETECTOR_RANGE;
        e.severity = SEVERITY_WARN;
        e.state    = EVENT_STATE_CLEARED;
        e.version  = EVENT_VERSION;

        push_event(det, events, cap, n_events, &e);
        det->state[s->channel_id].range_in_violation = false;
        return 1;
    }

    return 0;
}

/* Check the z-score detector for one sample. */
static int check_zscore(detect_t *det,
                        const sample_t *s,
                        const baseline_t *baseline,
                        const packet_t *p,
                        event_t *events, size_t cap, size_t *n_events,
                        uint16_t regime) {
    if (!det->config.enable_zscore) return 0;
    if (!baseline) return 0;

    if (s->flags & CH_FLAG_STALE) return 0;
    if (!(s->flags & CH_FLAG_VALID)) return 0;

    /* Get the baseline for this channel in this regime. */
    const baseline_stat_t *bs = baseline_get(baseline, s->channel_id,
                                             regime);
    if (!bs) return 0;
    if (bs->state != BASELINE_READY) return 0;

    float mean = bs->mean;
    float sd   = baseline_stddev(bs);
    if (sd <= 0.0f) return 0;   /* flat baseline: cannot compute z */

    float v = sample_as_f32(s);
    float z = (v - mean) / sd;
    float abs_z = fabsf(z);
    float threshold = det->config.zscore_threshold;

    bool outside = (abs_z > threshold);
    bool was_in_violation = det->state[s->channel_id].zscore_in_violation;

    if (outside && !was_in_violation) {
        event_t e;
        memset(&e, 0, sizeof(e));
        event_set_timestamp(&e, plat_wall_ns());
        event_set_source_ts(&e, p->timestamp_ns);
        e.channel_id     = s->channel_id;
        e.channel_id2    = EVENT_CHANNEL_NONE;
        e.regime         = regime;
        e.observed       = v;
        e.baseline_mean  = mean;
        e.baseline_stddev = sd;
        e.threshold      = threshold;
        e.detector = DETECTOR_ZSCORE;
        e.severity = (abs_z > 2.0f * threshold) ? SEVERITY_CRITICAL
                                                : SEVERITY_WARN;
        e.state    = EVENT_STATE_STARTED;
        e.version  = EVENT_VERSION;

        push_event(det, events, cap, n_events, &e);
        det->state[s->channel_id].zscore_in_violation = true;
        det->last_mean[s->channel_id]     = mean;
        det->last_stddev[s->channel_id]   = sd;
        det->last_threshold[s->channel_id] = threshold;
        return 1;
    }

    if (!outside && was_in_violation) {
        event_t e;
        memset(&e, 0, sizeof(e));
        event_set_timestamp(&e, plat_wall_ns());
        event_set_source_ts(&e, p->timestamp_ns);
        e.channel_id      = s->channel_id;
        e.channel_id2     = EVENT_CHANNEL_NONE;
        e.regime          = regime;
        e.observed        = v;
        e.baseline_mean   = mean;
        e.baseline_stddev = sd;
        e.threshold       = threshold;
        e.detector = DETECTOR_ZSCORE;
        e.severity = SEVERITY_INFO;
        e.state    = EVENT_STATE_CLEARED;
        e.version  = EVENT_VERSION;

        push_event(det, events, cap, n_events, &e);
        det->state[s->channel_id].zscore_in_violation = false;
        return 1;
    }

    return 0;
}

int detect_update(detect_t *det,
                  const packet_t *p,
                  const baseline_t *baseline,
                  const registry_t *registry,
                  event_t *events, size_t cap) {
    if (!det || !p || !events) return -1;

    size_t n_events = 0;

    /* Derive regime once per packet. */
    uint16_t regime = EVENT_REGIME_NONE;
    if (baseline) {
        uint16_t r = baseline_regime_for_packet(baseline, p);
        if (r != 0xFFFF) regime = r;
    }

    for (uint16_t i = 0; i < p->channel_count; i++) {
        const sample_t *s = &p->samples[i];
        if (s->channel_id >= DETECT_MAX_CHANNELS) continue;

        check_range(det, s, registry, p, events, cap, &n_events);
        check_zscore(det, s, baseline, p, events, cap, &n_events,
                     regime);
    }

    return (int)n_events;
}