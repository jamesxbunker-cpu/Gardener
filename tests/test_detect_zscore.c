#include "gardener/detect.h"
#include "gardener/registry.h"
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

static packet_t mk_packet(uint64_t ts, const sample_t *s, uint16_t n) {
    packet_t p;
    memset(&p, 0, sizeof(p));
    p.timestamp_ns = ts;
    p.source_id = 1;
    p.channel_count = n;
    for (uint16_t i = 0; i < n; i++) p.samples[i] = s[i];
    return p;
}

/* Build a baseline with channel 1 having mean=50, stddev~1. Feed 200
 * samples around 50 to get it to READY. No regime channels. */
static void build_baseline(baseline_t *b) {
    baseline_init(b, NULL, 0);
    for (int i = 0; i < 200; i++) {
        float v = 50.0f + ((i % 3) - 1) * 0.5f;   /* 49.5, 50.0, 50.5 */
        sample_t s = mk_f32(1, v, (uint64_t)i);
        packet_t p = mk_packet((uint64_t)i, &s, 1);
        baseline_update(b, &p);
    }
}

static void test_ready_baseline_fires(void) {
    static detect_t d;
    detect_init_default(&d, 0);

    static baseline_t b;
    build_baseline(&b);
    const baseline_stat_t *bs = baseline_get(&b, 1, 0);
    CHECK(bs != NULL);
    if (bs) CHECK_EQ(bs->state, BASELINE_READY);

    /* Value far from mean: 50 + 10 = 60, z ≈ 10/stddev */
    /* stddev ≈ 0.408, so z ≈ 24.5. Threshold is 5. Should fire. */
    sample_t s = mk_f32(1, 60.0f, 1000);
    packet_t p = mk_packet(1000, &s, 1);
    event_t events[8];
    int n = detect_update(&d, &p, &b, NULL, events, 8);
    CHECK_EQ(n, 1);
    if (n >= 1) {
        CHECK_EQ(events[0].detector, DETECTOR_ZSCORE);
        CHECK_EQ(events[0].state, EVENT_STATE_STARTED);
        CHECK_EQ(events[0].channel_id, 1);
        CHECK(events[0].observed == 60.0f);
        CHECK(fabsf(events[0].baseline_mean - 50.0f) < 0.1f);
        CHECK(events[0].baseline_stddev > 0.0f);
    }
}

static void test_normal_value_no_event(void) {
    static detect_t d;
    detect_init_default(&d, 0);

    static baseline_t b;
    build_baseline(&b);

    sample_t s = mk_f32(1, 50.0f, 1000);
    packet_t p = mk_packet(1000, &s, 1);
    event_t events[8];
    int n = detect_update(&d, &p, &b, NULL, events, 8);
    CHECK_EQ(n, 0);
}

static void test_not_ready_baseline_no_fire(void) {
    static detect_t d;
    detect_init_default(&d, 0);

    static baseline_t b;
    baseline_init(&b, NULL, 0);
    /* Only 10 samples: LEARNING state. */
    for (int i = 0; i < 10; i++) {
        sample_t s = mk_f32(1, 50.0f, (uint64_t)i);
        packet_t p = mk_packet((uint64_t)i, &s, 1);
        baseline_update(&b, &p);
    }

    /* A wild value. Should not fire because baseline is LEARNING. */
    sample_t s = mk_f32(1, 1000.0f, 1000);
    packet_t p = mk_packet(1000, &s, 1);
    event_t events[8];
    int n = detect_update(&d, &p, &b, NULL, events, 8);
    CHECK_EQ(n, 0);
}

static void test_clears_when_back_to_normal(void) {
    static detect_t d;
    detect_init_default(&d, 0);
    static baseline_t b;
    build_baseline(&b);

    /* Fire. */
    sample_t s = mk_f32(1, 60.0f, 1000);
    packet_t p = mk_packet(1000, &s, 1);
    event_t events[8];
    detect_update(&d, &p, &b, NULL, events, 8);

    /* Back to normal. */
    s = mk_f32(1, 50.0f, 2000);
    p = mk_packet(2000, &s, 1);
    int n = detect_update(&d, &p, &b, NULL, events, 8);
    CHECK_EQ(n, 1);
    if (n >= 1) {
        CHECK_EQ(events[0].detector, DETECTOR_ZSCORE);
        CHECK_EQ(events[0].state, EVENT_STATE_CLEARED);
    }
}

static void test_no_baseline_no_zscore(void) {
    static detect_t d;
    detect_init_default(&d, 0);

    sample_t s = mk_f32(1, 1000.0f, 1000);
    packet_t p = mk_packet(1000, &s, 1);
    event_t events[8];
    int n = detect_update(&d, &p, NULL, NULL, events, 8);
    CHECK_EQ(n, 0);
}

static void test_severity_critical_for_very_far(void) {
    static detect_t d;
    detect_init_default(&d, 0);
    static baseline_t b;
    build_baseline(&b);

    /* 100 is 50 away from mean, stddev ~0.4, z ~ 120. > 2*5=10.
     * Should fire CRITICAL. */
    sample_t s = mk_f32(1, 100.0f, 1000);
    packet_t p = mk_packet(1000, &s, 1);
    event_t events[8];
    int n = detect_update(&d, &p, &b, NULL, events, 8);
    CHECK_EQ(n, 1);
    if (n >= 1) {
        CHECK_EQ(events[0].severity, SEVERITY_CRITICAL);
    }
}

int main(void) {
    plat_log_set_level(LOG_ERROR);

    test_ready_baseline_fires();
    test_normal_value_no_event();
    test_not_ready_baseline_no_fire();
    test_clears_when_back_to_normal();
    test_no_baseline_no_zscore();
    test_severity_critical_for_very_far();

    if (g_failures) {
        fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_detect_zscore: all checks passed\n");
    return 0;
}