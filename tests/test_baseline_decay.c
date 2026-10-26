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

static packet_t mk_packet(uint64_t ts, const sample_t *samples, uint16_t n) {
    packet_t p;
    memset(&p, 0, sizeof(p));
    p.timestamp_ns = ts;
    p.source_id = 1;
    p.channel_count = n;
    for (uint16_t i = 0; i < n; i++) p.samples[i] = samples[i];
    return p;
}

static void feed_one(baseline_t *b, uint16_t ch, float v, uint64_t ts) {
    sample_t s = mk_f32(ch, v, ts);
    packet_t p = mk_packet(ts, &s, 1);
    baseline_update(b, &p);
}

static void test_decay_null_safe(void) {
    baseline_decay(NULL, 0);
    CHECK_EQ(baseline_effective_count(NULL), 0.0f);
}

static void test_decay_empty_model_noop(void) {
    static baseline_t b;
    baseline_init(&b, NULL, 0);
    baseline_decay(&b, 1000000000ull);
    CHECK_EQ(baseline_channel_count(&b), 0);
}

static void test_decay_half_life(void) {
    static baseline_t b;
    baseline_init(&b, NULL, 1000000000ull);   /* 1 second half-life */

    for (int i = 0; i < 100; i++) {
        feed_one(&b, 1, 42.0f, (uint64_t)(i * 10000000ull));  /* every 10 ms */
    }

    const baseline_stat_t *s = baseline_get(&b, 1, 0);
    CHECK(s != NULL);
    float count_before = s->count;
    float m2_before = s->m2;
    float mean_before = s->mean;
    uint64_t last_before = s->last_ts_ns;

    CHECK_NEAR(count_before, 100.0, 1e-5);

    baseline_decay(&b, last_before + 1000000000ull);

    s = baseline_get(&b, 1, 0);
    if (s) {
        CHECK_NEAR(s->count, count_before / 2.0, 0.5);
        CHECK_NEAR(s->m2, m2_before / 2.0, 1e-4);
        CHECK_NEAR(s->mean, mean_before, 1e-6);
    }
}

static void test_decay_two_half_lives(void) {
    static baseline_t b;
    baseline_init(&b, NULL, 1000000000ull);

    for (int i = 0; i < 100; i++) {
        feed_one(&b, 1, 10.0f, (uint64_t)(i * 10000000ull));
    }

    const baseline_stat_t *s = baseline_get(&b, 1, 0);
    uint64_t last = s->last_ts_ns;

    baseline_decay(&b, last + 2 * 1000000000ull);

    s = baseline_get(&b, 1, 0);
    if (s) CHECK_NEAR(s->count, 25.0, 1.0);
}

static void test_decay_noop_for_zero_dt(void) {
    static baseline_t b;
    baseline_init(&b, NULL, 1000000000ull);

    for (int i = 0; i < 10; i++) feed_one(&b, 1, 5.0f, (uint64_t)i);

    const baseline_stat_t *s = baseline_get(&b, 1, 0);
    uint64_t last = s->last_ts_ns;
    float count_before = s->count;

    baseline_decay(&b, last);
    s = baseline_get(&b, 1, 0);
    if (s) CHECK_NEAR(s->count, count_before, 1e-6);
}

static void test_decay_ready_to_stale(void) {
    static baseline_t b;
    baseline_init(&b, NULL, 1000000000ull);

    for (int i = 0; i < BASELINE_MIN_SAMPLES; i++) {
        feed_one(&b, 1, 5.0f, (uint64_t)(i * 10000000ull));
    }

    const baseline_stat_t *s = baseline_get(&b, 1, 0);
    CHECK(s != NULL);
    if (s) CHECK_EQ(s->state, BASELINE_READY);

    uint64_t last = s->last_ts_ns;
    baseline_decay(&b, last + 2 * 1000000000ull);

    s = baseline_get(&b, 1, 0);
    if (s) {
        CHECK(s->count < (float)BASELINE_MIN_SAMPLES);
        CHECK_EQ(s->state, BASELINE_STALE);
    }
}

static void test_decay_learning_stays_learning(void) {
    static baseline_t b;
    baseline_init(&b, NULL, 1000000000ull);

    for (int i = 0; i < 50; i++) {
        feed_one(&b, 1, 5.0f, (uint64_t)(i * 10000000ull));
    }

    const baseline_stat_t *s = baseline_get(&b, 1, 0);
    CHECK(s != NULL);
    if (s) CHECK_EQ(s->state, BASELINE_LEARNING);

    baseline_decay(&b, s->last_ts_ns + 1000000000ull);

    s = baseline_get(&b, 1, 0);
    if (s) CHECK_EQ(s->state, BASELINE_LEARNING);
}

static void test_update_does_not_decay(void) {
    /* baseline_update accumulates. Decay is separate. Feeding 100
     * samples gives count == 100, regardless of timing. */
    static baseline_t b;
    baseline_init(&b, NULL, 1000000000ull);

    for (int i = 0; i < 100; i++) {
        feed_one(&b, 1, 10.0f, (uint64_t)(i * 10000000ull));
    }

    const baseline_stat_t *s = baseline_get(&b, 1, 0);
    CHECK(s != NULL);
    if (s) CHECK_NEAR(s->count, 100.0, 1e-5);

    /* Add another sample after a simulated 10 half-lives. Count
     * should still just be 101. */
    uint64_t last = s->last_ts_ns;
    feed_one(&b, 1, 10.0f, last + 10 * 1000000000ull);

    s = baseline_get(&b, 1, 0);
    if (s) CHECK_NEAR(s->count, 101.0, 1e-5);
}

static void test_decay_preserves_mean(void) {
    static baseline_t b;
    baseline_init(&b, NULL, 1000000000ull);

    for (int i = 0; i < 50; i++) feed_one(&b, 1, 42.0f, (uint64_t)(i * 10000000ull));
    const baseline_stat_t *s = baseline_get(&b, 1, 0);
    uint64_t last = s->last_ts_ns;

    baseline_decay(&b, last + 3 * 1000000000ull);

    s = baseline_get(&b, 1, 0);
    if (s) CHECK_NEAR(s->mean, 42.0, 1e-5);
}

static void test_decay_stale_to_ready(void) {
    /* After decay to STALE, feed enough new samples to cross the
     * READY threshold again. State should return to READY. */
    static baseline_t b;
    baseline_init(&b, NULL, 1000000000ull);

    for (int i = 0; i < BASELINE_MIN_SAMPLES * 4; i++) {
        feed_one(&b, 1, 5.0f, (uint64_t)(i * 10000000ull));
    }
    const baseline_stat_t *s = baseline_get(&b, 1, 0);
    CHECK_EQ(s->state, BASELINE_READY);

    uint64_t last = s->last_ts_ns;
    /* 2.5 half-lives: 400 * 2^-2.5 ≈ 70.7, below threshold. */
    baseline_decay(&b, last + 2500000000ull);

    s = baseline_get(&b, 1, 0);
    CHECK_EQ(s->state, BASELINE_STALE);
    float stale_count = s->count;

    /* Feed 50 new samples: 70.7 + 50 = 120.7, above threshold. */
    for (int i = 0; i < 50; i++) {
        feed_one(&b, 1, 5.0f, last + 2500000000ull + (uint64_t)(i * 10000000ull));
    }

    s = baseline_get(&b, 1, 0);
    if (s) {
        CHECK(s->count > stale_count);
        CHECK(s->count >= (float)BASELINE_MIN_SAMPLES);
        CHECK_EQ(s->state, BASELINE_READY);
    }
}

static void test_decay_to_zero(void) {
    static baseline_t b;
    baseline_init(&b, NULL, 1000000000ull);

    for (int i = 0; i < 50; i++) feed_one(&b, 1, 5.0f, (uint64_t)(i * 10000000ull));
    const baseline_stat_t *s = baseline_get(&b, 1, 0);
    uint64_t last = s->last_ts_ns;

    baseline_decay(&b, last + 100 * 1000000000ull);

    s = baseline_get(&b, 1, 0);
    if (s) {
        CHECK(s->count >= 0.0f);
        CHECK(s->count < 0.001f);
    }
}

int main(void) {
    plat_log_set_level(LOG_ERROR);

    test_decay_null_safe();
    test_decay_empty_model_noop();
    test_decay_half_life();
    test_decay_two_half_lives();
    test_decay_noop_for_zero_dt();
    test_decay_ready_to_stale();
    test_decay_learning_stays_learning();
    test_update_does_not_decay();
    test_decay_preserves_mean();
    test_decay_stale_to_ready();
    test_decay_to_zero();

    if (g_failures) {
        fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_baseline_decay: all checks passed\n");
    return 0;
}