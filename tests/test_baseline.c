#include "gardener/baseline.h"
#include "gardener/packet.h"
#include "gardener/platform.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

static int g_failures = 0;

#define CHECK(cond) do {                                              \
    if (!(cond)) {                                                    \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
        g_failures++;                                                 \
    }                                                                 \
} while (0)

#define CHECK_EQ(a, b) do {                                           \
    long long _a = (long long)(a), _b = (long long)(b);               \
    if (_a != _b) {                                                   \
        fprintf(stderr, "FAIL %s:%d: %s (%lld) != %s (%lld)\n",       \
                __FILE__, __LINE__, #a, _a, #b, _b);                  \
        g_failures++;                                                 \
    }                                                                 \
} while (0)

#define CHECK_NEAR(a, b, eps) do {                                    \
    double _a = (double)(a), _b = (double)(b);                        \
    if (fabs(_a - _b) > (eps)) {                                      \
        fprintf(stderr, "FAIL %s:%d: %g != %g (eps=%g)\n",            \
                __FILE__, __LINE__, _a, _b, (double)(eps));           \
        g_failures++;                                                 \
    }                                                                 \
} while (0)

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static sample_t mk_f32(uint16_t id, float v, uint64_t ts) {
    sample_t s;
    memset(&s, 0, sizeof(s));
    s.channel_id = id;
    s.type = CH_F32;
    s.role = ROLE_SENSOR;
    s.flags = CH_FLAG_VALID;
    s.v.f = v;
    s.timestamp_ns = ts;
    return s;
}

static packet_t mk_packet(uint64_t ts, uint16_t src,
                          const sample_t *samples, uint16_t n) {
    packet_t p;
    memset(&p, 0, sizeof(p));
    p.timestamp_ns = ts;
    p.source_id = src;
    p.channel_count = n;
    for (uint16_t i = 0; i < n; i++) p.samples[i] = samples[i];
    return p;
}

/* Feed a single-sample packet. */
static void feed_one(baseline_t *b, uint16_t ch, float v, uint64_t ts) {
    sample_t s = mk_f32(ch, v, ts);
    packet_t p = mk_packet(ts, 1, &s, 1);
    baseline_update(b, &p);
}

/* ------------------------------------------------------------------ */
/* Tests                                                              */
/* ------------------------------------------------------------------ */

static void test_init_null(void) {
    CHECK(baseline_init(NULL, 0) == -1);
}

static void test_init_empty(void) {
    static baseline_t b;
    CHECK(baseline_init(&b, 0) == 0);
    CHECK_EQ(b.header.magic, BASELINE_MAGIC);
    CHECK_EQ(b.header.version, BASELINE_VERSION);
    CHECK_EQ(b.header.channel_count, 0);
    CHECK_EQ(baseline_channel_count(&b), 0);
    CHECK_EQ(b.next_slot, 0);
}

static void test_update_null(void) {
    static baseline_t b;
    baseline_init(&b, 0);
    CHECK(baseline_update(NULL, NULL) == -1);
    CHECK(baseline_update(&b, NULL) == -1);

    packet_t p;
    memset(&p, 0, sizeof(p));
    CHECK(baseline_update(NULL, &p) == -1);
}

static void test_welford_known_sequence(void) {
    /* Sequence 1, 2, 3, 4, 5. Expected:
     *   count = 5
     *   mean  = 3
     *   m2    = (1-3)^2 + (2-3)^2 + 0 + (4-3)^2 + (5-3)^2 = 4 + 1 + 0 + 1 + 4 = 10
     *   sample variance = 10 / (5 - 1) = 2.5
     *   stddev = sqrt(2.5) ≈ 1.5811
     */
    static baseline_t b;
    baseline_init(&b, 0);

    for (int i = 1; i <= 5; i++) {
        feed_one(&b, 1, (float)i, (uint64_t)(i * 100));
    }

    const baseline_stat_t *s = baseline_get(&b, 1, 0);
    CHECK(s != NULL);
    if (s) {
        CHECK_NEAR(s->count, 5.0, 1e-5);
        CHECK_NEAR(s->mean, 3.0, 1e-5);
        CHECK_NEAR(s->m2, 10.0, 1e-4);
        CHECK_NEAR(baseline_variance(s), 2.5, 1e-4);
        CHECK_NEAR(baseline_stddev(s), sqrt(2.5), 1e-4);
        CHECK_NEAR(s->min_val, 1.0, 1e-6);
        CHECK_NEAR(s->max_val, 5.0, 1e-6);
        CHECK_EQ(s->first_ts_ns, 100);
        CHECK_EQ(s->last_ts_ns, 500);
        CHECK_EQ(s->state, BASELINE_LEARNING);
    }
}

static void test_welford_constant_sequence(void) {
    /* All samples identical: mean = v, variance = 0. */
    static baseline_t b;
    baseline_init(&b, 0);
    for (int i = 0; i < 100; i++) {
        feed_one(&b, 1, 42.0f, (uint64_t)i);
    }
    const baseline_stat_t *s = baseline_get(&b, 1, 0);
    CHECK(s != NULL);
    if (s) {
        CHECK_NEAR(s->mean, 42.0, 1e-5);
        CHECK_NEAR(baseline_variance(s), 0.0, 1e-5);
        CHECK_NEAR(s->min_val, 42.0, 1e-6);
        CHECK_NEAR(s->max_val, 42.0, 1e-6);
    }
}

static void test_welford_single_sample(void) {
    /* One sample: mean = v, variance = 0 (undefined for count < 2). */
    static baseline_t b;
    baseline_init(&b, 0);
    feed_one(&b, 1, 7.5f, 123);

    const baseline_stat_t *s = baseline_get(&b, 1, 0);
    CHECK(s != NULL);
    if (s) {
        CHECK_NEAR(s->count, 1.0, 1e-6);
        CHECK_NEAR(s->mean, 7.5, 1e-6);
        CHECK_NEAR(s->m2, 0.0, 1e-6);
        CHECK_NEAR(baseline_variance(s), 0.0, 1e-6);
        CHECK_NEAR(baseline_stddev(s), 0.0, 1e-6);
        CHECK_EQ(s->first_ts_ns, 123);
        CHECK_EQ(s->last_ts_ns, 123);
        CHECK_EQ(s->state, BASELINE_LEARNING);
    }
}

static void test_state_transitions(void) {
    /* EMPTY → LEARNING on first sample. LEARNING → READY at
     * BASELINE_MIN_SAMPLES. */
    static baseline_t b;
    baseline_init(&b, 0);

    /* No samples yet. */
    CHECK(baseline_get(&b, 1, 0) == NULL);   /* NULL for EMPTY */

    /* First sample. */
    feed_one(&b, 1, 1.0f, 0);
    const baseline_stat_t *s = baseline_get(&b, 1, 0);
    CHECK(s != NULL);
    if (s) CHECK_EQ(s->state, BASELINE_LEARNING);

    /* One below threshold. */
    for (int i = 1; i < BASELINE_MIN_SAMPLES - 1; i++) {
        feed_one(&b, 1, 1.0f, (uint64_t)i);
    }
    s = baseline_get(&b, 1, 0);
    CHECK(s != NULL);
    if (s) {
        CHECK_NEAR(s->count, (float)(BASELINE_MIN_SAMPLES - 1), 1e-5);
        CHECK_EQ(s->state, BASELINE_LEARNING);
    }

    /* Exactly threshold. */
    feed_one(&b, 1, 1.0f, (uint64_t)(BASELINE_MIN_SAMPLES - 1));
    s = baseline_get(&b, 1, 0);
    CHECK(s != NULL);
    if (s) {
        CHECK_NEAR(s->count, (float)BASELINE_MIN_SAMPLES, 1e-5);
        CHECK_EQ(s->state, BASELINE_READY);
    }
}

static void test_multiple_channels(void) {
    static baseline_t b;
    baseline_init(&b, 0);

    feed_one(&b, 1, 10.0f, 100);
    feed_one(&b, 2, 20.0f, 100);
    feed_one(&b, 3, 30.0f, 100);

    CHECK_EQ(baseline_channel_count(&b), 3);

    const baseline_stat_t *s1 = baseline_get(&b, 1, 0);
    const baseline_stat_t *s2 = baseline_get(&b, 2, 0);
    const baseline_stat_t *s3 = baseline_get(&b, 3, 0);
    CHECK(s1 && s2 && s3);
    if (s1) CHECK_NEAR(s1->mean, 10.0, 1e-6);
    if (s2) CHECK_NEAR(s2->mean, 20.0, 1e-6);
    if (s3) CHECK_NEAR(s3->mean, 30.0, 1e-6);
}

static void test_multi_sample_packet(void) {
    static baseline_t b;
    baseline_init(&b, 0);

    sample_t samples[3] = {
        mk_f32(1, 1.0f, 100),
        mk_f32(2, 2.0f, 100),
        mk_f32(3, 3.0f, 100),
    };
    packet_t p = mk_packet(100, 1, samples, 3);

    int n = baseline_update(&b, &p);
    CHECK_EQ(n, 3);
    CHECK_EQ(baseline_channel_count(&b), 3);
}

static void test_sample_timestamp_used(void) {
    /* Sample's own timestamp should be used when set. */
    static baseline_t b;
    baseline_init(&b, 0);

    sample_t s = mk_f32(1, 5.0f, 12345);
    packet_t p = mk_packet(99999, 1, &s, 1);
    baseline_update(&b, &p);

    const baseline_stat_t *st = baseline_get(&b, 1, 0);
    CHECK(st != NULL);
    if (st) {
        CHECK_EQ(st->first_ts_ns, 12345);
        CHECK_EQ(st->last_ts_ns, 12345);
    }
}

static void test_packet_timestamp_inherited(void) {
    /* If sample timestamp is 0, use packet's. */
    static baseline_t b;
    baseline_init(&b, 0);

    sample_t s = mk_f32(1, 5.0f, 0);
    packet_t p = mk_packet(99999, 1, &s, 1);
    baseline_update(&b, &p);

    const baseline_stat_t *st = baseline_get(&b, 1, 0);
    CHECK(st != NULL);
    if (st) {
        CHECK_EQ(st->first_ts_ns, 99999);
    }
}

static void test_stale_samples_skipped(void) {
    static baseline_t b;
    baseline_init(&b, 0);

    sample_t s = mk_f32(1, 5.0f, 100);
    s.flags = CH_FLAG_STALE;   /* stale, not valid */
    packet_t p = mk_packet(100, 1, &s, 1);
    int n = baseline_update(&b, &p);
    CHECK_EQ(n, 0);
    CHECK(baseline_get(&b, 1, 0) == NULL);
}

static void test_invalid_samples_skipped(void) {
    static baseline_t b;
    baseline_init(&b, 0);

    sample_t s = mk_f32(1, 5.0f, 100);
    s.flags = 0;               /* neither VALID nor STALE */
    packet_t p = mk_packet(100, 1, &s, 1);
    int n = baseline_update(&b, &p);
    CHECK_EQ(n, 0);
    CHECK(baseline_get(&b, 1, 0) == NULL);
}

static void test_get_bounds(void) {
    static baseline_t b;
    baseline_init(&b, 0);
    feed_one(&b, 1, 5.0f, 100);

    /* Out-of-range regime. */
    CHECK(baseline_get(&b, 1, BASELINE_MAX_REGIMES) == NULL);
    /* Unknown channel. */
    CHECK(baseline_get(&b, 99, 0) == NULL);
    /* NULL baseline. */
    CHECK(baseline_get(NULL, 1, 0) == NULL);
}

static void test_variance_null_safety(void) {
    CHECK(baseline_variance(NULL) == 0.0f);
    CHECK(baseline_stddev(NULL) == 0.0f);
}

static void test_no_neg_variance(void) {
    /* Floating point can produce tiny negative variance for near-
     * constant data. baseline_variance must not return negative. */
    static baseline_t b;
    baseline_init(&b, 0);
    for (int i = 0; i < 1000; i++) {
        feed_one(&b, 1, 42.0f + (i % 3) * 1e-6f, (uint64_t)i);
    }
    const baseline_stat_t *s = baseline_get(&b, 1, 0);
    CHECK(s != NULL);
    if (s) {
        CHECK(baseline_variance(s) >= 0.0f);
        CHECK(baseline_stddev(s) >= 0.0f);
    }
}

static void test_large_mean_small_variance(void) {
    /* The whole point of Welford: numerical stability for large means.
     * 1e6 with variations of ±0.5 should give stddev ≈ 0.2887 (std of
     * [-0.5, 0, 0.5] = sqrt(0.5/3) ≈ 0.408 if sample; if we use values
     * 1e6 - 0.5, 1e6, 1e6 + 0.5, sample variance = (0.25+0+0.25)/2 = 0.25,
     * stddev = 0.5). */
    static baseline_t b;
    baseline_init(&b, 0);
    feed_one(&b, 1, 1.0e6f - 0.5f, 0);
    feed_one(&b, 1, 1.0e6f,       1);
    feed_one(&b, 1, 1.0e6f + 0.5f, 2);

    const baseline_stat_t *s = baseline_get(&b, 1, 0);
    CHECK(s != NULL);
    if (s) {
        CHECK_NEAR(s->mean, 1.0e6, 1e-1);
        CHECK_NEAR(baseline_variance(s), 0.25, 1e-3);
        CHECK_NEAR(baseline_stddev(s), 0.5, 1e-3);
    }
}

static void test_channel_capacity_limit(void) {
    /* Register more channels than BASELINE_MAX_CHANNELS. Extras should
     * be silently dropped. */
    static baseline_t b;
    baseline_init(&b, 0);

    for (uint16_t id = 1; id <= BASELINE_MAX_CHANNELS; id++) {
        feed_one(&b, id, (float)id, 0);
    }
    CHECK_EQ(baseline_channel_count(&b), BASELINE_MAX_CHANNELS);

    /* One more should fail silently. */
    feed_one(&b, BASELINE_MAX_CHANNELS + 1, 999.0f, 0);
    CHECK_EQ(baseline_channel_count(&b), BASELINE_MAX_CHANNELS);
    CHECK(baseline_get(&b, BASELINE_MAX_CHANNELS + 1, 0) == NULL);
}

int main(void) {
    plat_log_set_level(LOG_ERROR);

    test_init_null();
    test_init_empty();
    test_update_null();
    test_welford_known_sequence();
    test_welford_constant_sequence();
    test_welford_single_sample();
    test_state_transitions();
    test_multiple_channels();
    test_multi_sample_packet();
    test_sample_timestamp_used();
    test_packet_timestamp_inherited();
    test_stale_samples_skipped();
    test_invalid_samples_skipped();
    test_get_bounds();
    test_variance_null_safety();
    test_no_neg_variance();
    test_large_mean_small_variance();
    test_channel_capacity_limit();

    if (g_failures) {
        fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_baseline: all checks passed\n");
    return 0;
}