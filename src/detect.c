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
        det->config.enable_flatline = true;
        det->config.enable_roc = true;
        det->config.zscore_threshold = DETECT_DEFAULT_ZSCORE_THRESHOLD;
        det->config.roc_absolute_max = DETECT_DEFAULT_ROC_ABSOLUTE_MAX;
        det->config.roc_factor = DETECT_DEFAULT_ROC_FACTOR;
        det->config.source_id = 0;
    }
    return 0;
}

int detect_init_default(detect_t *det, uint16_t source_id) {
    if (!det) return -1;
    detect_config_t cfg = {
        .enable_range = true,
        .enable_zscore = true,
        .enable_flatline = true,
        .enable_roc = true,
        .zscore_threshold = DETECT_DEFAULT_ZSCORE_THRESHOLD,
        .roc_absolute_max = DETECT_DEFAULT_ROC_ABSOLUTE_MAX,
        .roc_factor = DETECT_DEFAULT_ROC_FACTOR,
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

/* Flatline: returns 1 if the last FLATLINE_WINDOW samples for this
 * channel all have the same value (within FLATLINE_EPSILON). Returns
 * 0 if insufficient history or values differ. */
static int is_flatline(const ingest_t *ing, uint16_t ch_id) {
    const channel_state_t *cs = ingest_state(ing, ch_id);
    if (!cs || !cs->seen) return 0;
    size_t n = ring_count(&cs->ring);
    if (n < DETECT_FLATLINE_WINDOW) return 0;

    const sample_t *first = ring_at(&cs->ring, n - DETECT_FLATLINE_WINDOW);
    if (!first) return 0;
    float v0 = sample_as_f32(first);

    for (size_t i = n - DETECT_FLATLINE_WINDOW + 1; i < n; i++) {
        const sample_t *s = ring_at(&cs->ring, i);
        if (!s) return 0;
        float vi = sample_as_f32(s);
        if (fabsf(vi - v0) > DETECT_FLATLINE_EPSILON) return 0;
    }
    return 1;
}

/* Rate of change: computes mean absolute rate over the last
 * DETECT_ROC_WINDOW pairs. Returns the rate in units per second.
 * Returns -1 if insufficient history. Timestamps are nanoseconds. */
static float compute_roc(const ingest_t *ing, uint16_t ch_id) {
    const channel_state_t *cs = ingest_state(ing, ch_id);
    if (!cs || !cs->seen) return -1.0f;
    size_t n = ring_count(&cs->ring);
    if (n < DETECT_ROC_WINDOW + 1) return -1.0f;

    size_t start = n - DETECT_ROC_WINDOW - 1;
    float total_rate = 0.0f;
    int pairs = 0;
    for (size_t i = start + 1; i < n; i++) {
        const sample_t *prev = ring_at(&cs->ring, i - 1);
        const sample_t *cur  = ring_at(&cs->ring, i);
        if (!prev || !cur) continue;
        uint64_t dt_ns = cur->timestamp_ns - prev->timestamp_ns;
        if (dt_ns == 0) continue;
        float dv = sample_as_f32(cur) - sample_as_f32(prev);
        float rate = fabsf(dv) / ((float)dt_ns / 1e9f);
        total_rate += rate;
        pairs++;
    }
    if (pairs == 0) return -1.0f;
    return total_rate / (float)pairs;
}

static int check_flatline(detect_t *det, const sample_t *s,
                          const ingest_t *ing, const packet_t *p,
                          event_t *events, size_t cap, size_t *n) {
    if (!det->config.enable_flatline || !ing) return 0;
    if (s->flags & CH_FLAG_STALE) return 0;
    if (!(s->flags & CH_FLAG_VALID)) return 0;

    bool flat = is_flatline(ing, s->channel_id);
    bool was = det->state[s->channel_id].flatline_in_violation;

    if (flat && !was) {
        event_t e;
        memset(&e, 0, sizeof(e));
        event_set_timestamp(&e, plat_wall_ns());
        event_set_source_ts(&e, p->timestamp_ns);
        e.channel_id  = s->channel_id;
        e.channel_id2 = EVENT_CHANNEL_NONE;
        e.regime      = EVENT_REGIME_NONE;
        e.observed    = sample_as_f32(s);
        e.threshold   = DETECT_FLATLINE_EPSILON;
        e.detector    = DETECTOR_FLATLINE;
        e.severity    = SEVERITY_WARN;
        e.state       = EVENT_STATE_STARTED;
        e.version     = EVENT_VERSION;
        push_event(det, events, cap, n, &e);
        det->state[s->channel_id].flatline_in_violation = true;
        return 1;
    }
    if (!flat && was) {
        event_t e;
        memset(&e, 0, sizeof(e));
        event_set_timestamp(&e, plat_wall_ns());
        event_set_source_ts(&e, p->timestamp_ns);
        e.channel_id  = s->channel_id;
        e.channel_id2 = EVENT_CHANNEL_NONE;
        e.regime      = EVENT_REGIME_NONE;
        e.observed    = sample_as_f32(s);
        e.detector    = DETECTOR_FLATLINE;
        e.severity    = SEVERITY_INFO;
        e.state       = EVENT_STATE_CLEARED;
        e.version     = EVENT_VERSION;
        push_event(det, events, cap, n, &e);
        det->state[s->channel_id].flatline_in_violation = false;
        return 1;
    }
    return 0;
}

static int check_roc(detect_t *det, const sample_t *s,
                     const ingest_t *ing, const packet_t *p,
                     event_t *events, size_t cap, size_t *n) {
    if (!det->config.enable_roc || !ing) return 0;
    if (s->flags & CH_FLAG_STALE) return 0;
    if (!(s->flags & CH_FLAG_VALID)) return 0;

    float rate = compute_roc(ing, s->channel_id);
    if (rate < 0.0f) return 0;   /* insufficient history */

    float threshold = det->config.roc_absolute_max;
    if (threshold <= 0.0f) threshold = DETECT_DEFAULT_ROC_ABSOLUTE_MAX;

    bool outside = (rate > threshold);
    bool was = det->state[s->channel_id].roc_in_violation;

    if (outside && !was) {
        event_t e;
        memset(&e, 0, sizeof(e));
        event_set_timestamp(&e, plat_wall_ns());
        event_set_source_ts(&e, p->timestamp_ns);
        e.channel_id  = s->channel_id;
        e.channel_id2 = EVENT_CHANNEL_NONE;
        e.regime      = EVENT_REGIME_NONE;
        e.observed    = rate;
        e.threshold   = threshold;
        e.detector    = DETECTOR_ROC;
        e.severity    = SEVERITY_WARN;
        e.state       = EVENT_STATE_STARTED;
        e.version     = EVENT_VERSION;
        push_event(det, events, cap, n, &e);
        det->state[s->channel_id].roc_in_violation = true;
        return 1;
    }
    if (!outside && was) {
        event_t e;
        memset(&e, 0, sizeof(e));
        event_set_timestamp(&e, plat_wall_ns());
        event_set_source_ts(&e, p->timestamp_ns);
        e.channel_id  = s->channel_id;
        e.channel_id2 = EVENT_CHANNEL_NONE;
        e.regime      = EVENT_REGIME_NONE;
        e.observed    = rate;
        e.threshold   = threshold;
        e.detector    = DETECTOR_ROC;
        e.severity    = SEVERITY_INFO;
        e.state       = EVENT_STATE_CLEARED;
        e.version     = EVENT_VERSION;
        push_event(det, events, cap, n, &e);
        det->state[s->channel_id].roc_in_violation = false;
        return 1;
    }
    return 0;
}

int detect_update(detect_t *det,
                  const packet_t *p,
                  const ingest_t *ing,
                  const baseline_t *baseline,
                  const registry_t *registry,
                  event_t *events, size_t cap) {
    if (!det || !p || !events) return -1;

    size_t n_events = 0;
    uint16_t regime = EVENT_REGIME_NONE;
    if (baseline) {
        uint16_t r = baseline_regime_for_packet(baseline, p);
        if (r != 0xFFFF) regime = r;
    }

    for (uint16_t i = 0; i < p->channel_count; i++) {
        const sample_t *s = &p->samples[i];
        if (s->channel_id >= DETECT_MAX_CHANNELS) continue;

        check_range(det, s, registry, p, events, cap, &n_events);
        check_zscore(det, s, baseline, p, events, cap, &n_events, regime);
        check_flatline(det, s, ing, p, events, cap, &n_events);
        check_roc(det, s, ing, p, events, cap, &n_events);
    }

    return (int)n_events;
}