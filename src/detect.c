#include "gardener/detect.h"
#include "gardener/platform.h"

#include <math.h>
#include <string.h>

/* ---------------------------------------------------------------------
 * Detectors.
 *
 * Implemented: range, zscore, flatline, roc, correlation.
 * Reserved:    mismatch (D4).
 *
 * All detectors share a common lifecycle: when a condition becomes
 * true, one STARTED event is emitted; while it persists, no further
 * events; when it becomes false, one CLEARED event is emitted.
 *
 * Per-channel state (range_in_violation, zscore_in_violation, etc.)
 * tracks whether each detector is currently firing for each channel.
 * Per-pair state (in_violation) does the same for correlation pairs.
 * ------------------------------------------------------------------- */

/* ---- lifecycle ---------------------------------------------------- */

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
        det->config.enable_correlation = true;
        det->config.zscore_threshold = DETECT_DEFAULT_ZSCORE_THRESHOLD;
        det->config.roc_absolute_max = DETECT_DEFAULT_ROC_ABSOLUTE_MAX;
        det->config.roc_factor = DETECT_DEFAULT_ROC_FACTOR;
        det->config.correlation_threshold = DETECT_DEFAULT_CORR_THRESHOLD;
        det->config.correlation_pair_count = 0;
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
        .enable_correlation = true,
        .zscore_threshold = DETECT_DEFAULT_ZSCORE_THRESHOLD,
        .roc_absolute_max = DETECT_DEFAULT_ROC_ABSOLUTE_MAX,
        .roc_factor = DETECT_DEFAULT_ROC_FACTOR,
        .correlation_threshold = DETECT_DEFAULT_CORR_THRESHOLD,
        .correlation_pair_count = 0,
        .source_id = source_id,
    };
    return detect_init(det, &cfg);
}

void detect_reset(detect_t *det) {
    if (!det) return;
    memset(det->state, 0, sizeof(det->state));
    memset(det->pair_state, 0, sizeof(det->pair_state));
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

/* ---- event buffer helper ------------------------------------------ */

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

/* ---- range detector ----------------------------------------------- */

static int check_range(detect_t *det,
                       const sample_t *s,
                       const registry_t *reg,
                       const packet_t *p,
                       event_t *events, size_t cap, size_t *n) {
    if (!det->config.enable_range) return 0;
    if (!reg) return 0;

    const channel_desc_t *desc = registry_get(reg, s->channel_id);
    if (!desc || !desc->has_range) return 0;

    if (s->flags & CH_FLAG_STALE) return 0;
    if (!(s->flags & CH_FLAG_VALID)) return 0;

    float v = sample_as_f32(s);
    bool outside = (v < desc->min_valid) || (v > desc->max_valid);
    bool was = det->state[s->channel_id].range_in_violation;

    if (outside && !was) {
        event_t e;
        memset(&e, 0, sizeof(e));
        event_set_timestamp(&e, plat_wall_ns());
        event_set_source_ts(&e, p->timestamp_ns);
        e.channel_id  = s->channel_id;
        e.channel_id2 = EVENT_CHANNEL_NONE;
        e.regime      = EVENT_REGIME_NONE;
        e.observed    = v;
        e.threshold   = (v < desc->min_valid) ? desc->min_valid
                                              : desc->max_valid;
        e.detector = DETECTOR_RANGE;
        e.severity = SEVERITY_WARN;
        e.state    = EVENT_STATE_STARTED;
        e.version  = EVENT_VERSION;
        push_event(det, events, cap, n, &e);
        det->state[s->channel_id].range_in_violation = true;
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
        e.observed    = v;
        e.threshold   = (v < desc->min_valid) ? desc->min_valid
                                              : desc->max_valid;
        e.detector = DETECTOR_RANGE;
        e.severity = SEVERITY_INFO;
        e.state    = EVENT_STATE_CLEARED;
        e.version  = EVENT_VERSION;
        push_event(det, events, cap, n, &e);
        det->state[s->channel_id].range_in_violation = false;
        return 1;
    }
    return 0;
}

/* ---- z-score detector --------------------------------------------- */

static int check_zscore(detect_t *det,
                        const sample_t *s,
                        const baseline_t *baseline,
                        const packet_t *p,
                        event_t *events, size_t cap, size_t *n,
                        uint16_t regime) {
    if (!det->config.enable_zscore) return 0;
    if (!baseline) return 0;

    if (s->flags & CH_FLAG_STALE) return 0;
    if (!(s->flags & CH_FLAG_VALID)) return 0;

    const baseline_stat_t *bs = baseline_get(baseline, s->channel_id,
                                             regime);
    if (!bs) return 0;
    if (bs->state != BASELINE_READY) return 0;

    float mean = bs->mean;
    float sd   = baseline_stddev(bs);
    if (sd <= 0.0f) return 0;

    float v = sample_as_f32(s);
    float z = (v - mean) / sd;
    float abs_z = fabsf(z);
    float threshold = det->config.zscore_threshold;
    if (threshold <= 0.0f) threshold = DETECT_DEFAULT_ZSCORE_THRESHOLD;

    bool outside = (abs_z > threshold);
    bool was = det->state[s->channel_id].zscore_in_violation;

    if (outside && !was) {
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
        e.severity = (abs_z > 2.0f * threshold) ? SEVERITY_CRITICAL
                                                : SEVERITY_WARN;
        e.state    = EVENT_STATE_STARTED;
        e.version  = EVENT_VERSION;
        push_event(det, events, cap, n, &e);
        det->state[s->channel_id].zscore_in_violation = true;
        det->last_mean[s->channel_id]      = mean;
        det->last_stddev[s->channel_id]    = sd;
        det->last_threshold[s->channel_id] = threshold;
        return 1;
    }
    if (!outside && was) {
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
        push_event(det, events, cap, n, &e);
        det->state[s->channel_id].zscore_in_violation = false;
        return 1;
    }
    return 0;
}

/* ---- flatline helpers --------------------------------------------- */

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
        if (fabsf(sample_as_f32(s) - v0) > DETECT_FLATLINE_EPSILON)
            return 0;
    }
    return 1;
}

static int check_flatline(detect_t *det,
                          const sample_t *s,
                          const ingest_t *ing,
                          const packet_t *p,
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

/* ---- rate-of-change helpers --------------------------------------- */

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
        if (cur->timestamp_ns <= prev->timestamp_ns) continue;
        uint64_t dt_ns = cur->timestamp_ns - prev->timestamp_ns;
        float dv = sample_as_f32(cur) - sample_as_f32(prev);
        float rate = fabsf(dv) / ((float)dt_ns / 1e9f);
        total_rate += rate;
        pairs++;
    }
    if (pairs == 0) return -1.0f;
    return total_rate / (float)pairs;
}

static int check_roc(detect_t *det,
                     const sample_t *s,
                     const ingest_t *ing,
                     const packet_t *p,
                     event_t *events, size_t cap, size_t *n) {
    if (!det->config.enable_roc || !ing) return 0;
    if (s->flags & CH_FLAG_STALE) return 0;
    if (!(s->flags & CH_FLAG_VALID)) return 0;

    float rate = compute_roc(ing, s->channel_id);
    if (rate < 0.0f) return 0;

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

/* ---- correlation helpers ------------------------------------------ */

static float compute_correlation(const ingest_t *ing,
                                 uint16_t ch_a, uint16_t ch_b) {
    const channel_state_t *ca = ingest_state(ing, ch_a);
    const channel_state_t *cb = ingest_state(ing, ch_b);
    if (!ca || !cb || !ca->seen || !cb->seen) return NAN;

    size_t na = ring_count(&ca->ring);
    size_t nb = ring_count(&cb->ring);
    if (na < DETECT_CORR_WINDOW || nb < DETECT_CORR_WINDOW) return NAN;

    size_t start_a = na - DETECT_CORR_WINDOW;
    size_t start_b = nb - DETECT_CORR_WINDOW;

    float sum_a = 0.0f, sum_b = 0.0f;
    float sum_ab = 0.0f, sum_aa = 0.0f, sum_bb = 0.0f;
    int n = 0;

    for (size_t i = 0; i < DETECT_CORR_WINDOW; i++) {
        const sample_t *sa = ring_at(&ca->ring, start_a + i);
        const sample_t *sb = ring_at(&cb->ring, start_b + i);
        if (!sa || !sb) continue;
        if (sa->flags & CH_FLAG_STALE) continue;
        if (!(sa->flags & CH_FLAG_VALID)) continue;
        if (sb->flags & CH_FLAG_STALE) continue;
        if (!(sb->flags & CH_FLAG_VALID)) continue;

        float a = sample_as_f32(sa);
        float b = sample_as_f32(sb);
        sum_a  += a;
        sum_b  += b;
        sum_ab += a * b;
        sum_aa += a * a;
        sum_bb += b * b;
        n++;
    }
    if (n < 4) return NAN;

    float nf = (float)n;
    float num = nf * sum_ab - sum_a * sum_b;
    float den_a = nf * sum_aa - sum_a * sum_a;
    float den_b = nf * sum_bb - sum_b * sum_b;
    if (den_a <= 0.0f || den_b <= 0.0f) return NAN;

    return num / sqrtf(den_a * den_b);
}

static int check_correlation(detect_t *det,
                             const packet_t *p,
                             const ingest_t *ing,
                             event_t *events, size_t cap, size_t *n) {
    if (!det->config.enable_correlation || !ing) return 0;
    if (det->config.correlation_pair_count == 0) return 0;

    float threshold = det->config.correlation_threshold;
    if (threshold <= 0.0f) threshold = DETECT_DEFAULT_CORR_THRESHOLD;

    int emitted = 0;

    for (uint8_t k = 0; k < det->config.correlation_pair_count; k++) {
        uint16_t a = det->config.correlation_pairs[k].ch_a;
        uint16_t b = det->config.correlation_pairs[k].ch_b;

        /* Only check this pair when the current packet has both
         * channels. Otherwise we'd re-evaluate the pair on every tick,
         * which is wasteful and could fire based on stale packet
         * boundaries. */
        bool has_a = false, has_b = false;
        for (uint16_t i = 0; i < p->channel_count; i++) {
            if (p->samples[i].channel_id == a) has_a = true;
            if (p->samples[i].channel_id == b) has_b = true;
        }
        if (!has_a || !has_b) continue;

        float r = compute_correlation(ing, a, b);
        if (isnan(r)) continue;

        bool outside = (r < threshold);
        bool was = det->pair_state[k].in_violation;

        if (outside && !was) {
            event_t e;
            memset(&e, 0, sizeof(e));
            event_set_timestamp(&e, plat_wall_ns());
            event_set_source_ts(&e, p->timestamp_ns);
            e.channel_id  = a;
            e.channel_id2 = b;
            e.regime      = EVENT_REGIME_NONE;
            e.observed    = r;
            e.threshold   = threshold;
            e.detector    = DETECTOR_CORRELATION;
            e.severity    = SEVERITY_WARN;
            e.state       = EVENT_STATE_STARTED;
            e.version     = EVENT_VERSION;
            push_event(det, events, cap, n, &e);
            det->pair_state[k].in_violation = true;
            emitted++;
        } else if (!outside && was) {
            event_t e;
            memset(&e, 0, sizeof(e));
            event_set_timestamp(&e, plat_wall_ns());
            event_set_source_ts(&e, p->timestamp_ns);
            e.channel_id  = a;
            e.channel_id2 = b;
            e.regime      = EVENT_REGIME_NONE;
            e.observed    = r;
            e.threshold   = threshold;
            e.detector    = DETECTOR_CORRELATION;
            e.severity    = SEVERITY_INFO;
            e.state       = EVENT_STATE_CLEARED;
            e.version     = EVENT_VERSION;
            push_event(det, events, cap, n, &e);
            det->pair_state[k].in_violation = false;
            emitted++;
        }
    }

    return emitted;
}

/* ---- top-level update --------------------------------------------- */

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

    /* Per-sample detectors. */
    for (uint16_t i = 0; i < p->channel_count; i++) {
        const sample_t *s = &p->samples[i];
        if (s->channel_id >= DETECT_MAX_CHANNELS) continue;

        check_range(det, s, registry, p, events, cap, &n_events);
        check_zscore(det, s, baseline, p, events, cap, &n_events, regime);
        check_flatline(det, s, ing, p, events, cap, &n_events);
        check_roc(det, s, ing, p, events, cap, &n_events);
    }

    /* Per-pair detector, once per packet. */
    check_correlation(det, p, ing, events, cap, &n_events);

    return (int)n_events;
}