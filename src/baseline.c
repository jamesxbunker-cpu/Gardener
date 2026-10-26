#include "gardener/baseline.h"
#include "gardener/platform.h"

#include <string.h>
#include <math.h>

/* ---------------------------------------------------------------------
 * Baseline model, C1: per-channel Welford statistics.
 *
 * In C1, every sample updates slot 0 for its channel. Regimes arrive
 * in C2. Decay arrives in C3. Persistence arrives in C4.
 *
 * Welford's online algorithm maintains (count, mean, M2) where M2 is
 * the sum of squared deviations from the mean. At any point:
 *   sample_variance = M2 / (count - 1)     (for count > 1)
 *   sample_stddev   = sqrt(sample_variance)
 *
 * This is numerically stable even for large means with small variance
 * (e.g. a barometric pressure of 101325 Pa that varies by 5 Pa). A
 * naive "sum of squares" approach would lose all precision in that
 * case.
 *
 * Reference: Welford, B.P. (1962). "Note on a method for calculating
 * corrected sums of squares and products". Technometrics 4 (3).
 * ------------------------------------------------------------------- */

int baseline_init(baseline_t *b, uint64_t halflife_ns) {
    if (!b) return -1;

    memset(b, 0, sizeof(*b));

    b->header.magic          = BASELINE_MAGIC;
    b->header.version        = BASELINE_VERSION;
    b->header.header_size    = BASELINE_HEADER_SIZE;
    b->header.channel_count  = 0;
    b->header.regime_count   = 0;
    b->header.created_ns     = plat_wall_ns();
    b->header.saved_ns       = 0;
    b->header.halflife_ns    = halflife_ns ? halflife_ns
                                           : BASELINE_DEFAULT_HALFLIFE_NS;
    b->header.regime_ch_count = 0;

    b->next_slot = 0;

    /* All slots EMPTY. slot_to_ch_id already zeroed. */
    return 0;
}

/* Find the slot index for a channel, or register a new one.
 * Returns the slot, or 0xFFFF if the model is full. */
static uint16_t find_or_register_slot(baseline_t *b, uint16_t ch_id) {
    /* Linear scan. 256 comparisons worst case, ~microseconds. */
    for (uint16_t i = 0; i < b->next_slot; i++) {
        if (b->slot_to_ch_id[i] == ch_id) return i;
    }
    if (b->next_slot >= BASELINE_MAX_CHANNELS) return 0xFFFF;
    uint16_t slot = b->next_slot++;
    b->slot_to_ch_id[slot] = ch_id;
    b->channels[slot].ch_id = ch_id;
    b->header.channel_count++;
    return slot;
}

/* Find the slot index for a channel without registering. Returns
 * 0xFFFF if not found. */
static uint16_t find_slot(const baseline_t *b, uint16_t ch_id) {
    for (uint16_t i = 0; i < b->next_slot; i++) {
        if (b->slot_to_ch_id[i] == ch_id) return i;
    }
    return 0xFFFF;
}

/* Welford's online update. Increments count, updates mean and M2,
 * tracks min and max, tracks first and last timestamps, and moves the
 * state from EMPTY to LEARNING to READY. */
static void welford_update(baseline_stat_t *s, float v,
                           uint64_t ts_ns) {
    if (s->state == BASELINE_EMPTY) {
        s->count       = 1.0f;
        s->mean        = v;
        s->m2          = 0.0f;
        s->min_val     = v;
        s->max_val     = v;
        s->first_ts_ns = ts_ns;
        s->last_ts_ns  = ts_ns;
        s->state       = BASELINE_LEARNING;
        return;
    }

    s->count += 1.0f;
    float delta = v - s->mean;
    s->mean += delta / s->count;
    float delta2 = v - s->mean;
    s->m2 += delta * delta2;

    if (v < s->min_val) s->min_val = v;
    if (v > s->max_val) s->max_val = v;

    if (ts_ns < s->first_ts_ns) s->first_ts_ns = ts_ns;
    if (ts_ns > s->last_ts_ns)  s->last_ts_ns  = ts_ns;

    if (s->state == BASELINE_LEARNING &&
        s->count >= (float)BASELINE_MIN_SAMPLES) {
        s->state = BASELINE_READY;
    }
}

int baseline_update(baseline_t *b, const packet_t *p) {
    if (!b || !p) return -1;

    int accepted = 0;
    for (uint16_t i = 0; i < p->channel_count; i++) {
        const sample_t *sample = &p->samples[i];

        /* Skip invalid samples. STALE-marked samples are also skipped:
         * a stale reading is not a real observation of the channel. */
        if (sample->flags & CH_FLAG_STALE) continue;
        if (!(sample->flags & CH_FLAG_VALID)) continue;

        uint16_t slot = find_or_register_slot(b, sample->channel_id);
        if (slot == 0xFFFF) {
            /* Model is full. Log once? For now, silently drop. */
            continue;
        }

        /* Timestamp: sample's own if set, else packet's. */
        uint64_t ts = sample->timestamp_ns ? sample->timestamp_ns
                                           : p->timestamp_ns;

        baseline_stat_t *stat = &b->stats[slot][0];
        welford_update(stat, sample_as_f32(sample), ts);
        accepted++;
    }

    return accepted;
}

float baseline_variance(const baseline_stat_t *s) {
    if (!s) return 0.0f;
    if (s->count < 2.0f) return 0.0f;
    return s->m2 / (s->count - 1.0f);
}

float baseline_stddev(const baseline_stat_t *s) {
    float v = baseline_variance(s);
    if (v <= 0.0f) return 0.0f;
    return sqrtf(v);
}

const baseline_stat_t *baseline_get(const baseline_t *b, uint16_t ch_id,
                                    uint16_t regime) {
    if (!b) return NULL;
    if (regime >= BASELINE_MAX_REGIMES) return NULL;
    uint16_t slot = find_slot(b, ch_id);
    if (slot == 0xFFFF) return NULL;
    const baseline_stat_t *s = &b->stats[slot][regime];
    if (s->state == BASELINE_EMPTY) return NULL;
    return s;
}

uint16_t baseline_channel_count(const baseline_t *b) {
    return b ? b->next_slot : 0;
}