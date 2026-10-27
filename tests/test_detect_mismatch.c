#include "gardener/detect.h"
#include "gardener/ingest.h"
#include "gardener/packet.h"
#include "gardener/platform.h"

#include <stdio.h>
#include <string.h>

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

static ingest_t g_ing;

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

/* Set up a detector with one mismatch pair (ctrl=11, actuator=20). */
static void setup_detect(detect_t *d) {
    detect_init_default(d, 0);
    d->config.enable_range = false;
    d->config.enable_zscore = false;
    d->config.enable_flatline = false;
    d->config.enable_roc = false;
    d->config.enable_correlation = false;
    d->config.enable_mismatch = true;
    d->config.mismatch_pair_count = 1;
    d->config.mismatch_pairs[0].ctrl_out = 11;
    d->config.mismatch_pairs[0].actuator = 20;
    d->config.mismatch_pairs[0].threshold = 5.0f;
    d->config.mismatch_sustained = 5;   /* shorter for test speed */
}

static int feed_pair(detect_t *d, float ctrl, float act,
                     int n, uint64_t *ts_counter) {
    int emitted = 0;
    for (int i = 0; i < n; i++) {
        sample_t s[2] = {
            mk_f32(11, ctrl, *ts_counter),
            mk_f32(20, act,  *ts_counter),
        };
        packet_t p = mk_packet(*ts_counter, s, 2);
        ingest_feed(&g_ing, &p);
        event_t events[8];
        int e = detect_update(d, &p, &g_ing, NULL, NULL, events, 8);
        if (e > 0) emitted += e;
        (*ts_counter)++;
    }
    return emitted;
}

static void test_agreeing_no_event(void) {
    ingest_init(&g_ing, NULL);
    static detect_t d;
    setup_detect(&d);

    uint64_t ts = 0;
    int emitted = feed_pair(&d, 50.0f, 50.0f, 20, &ts);
    CHECK_EQ(emitted, 0);
}

static void test_small_diff_no_event(void) {
    ingest_init(&g_ing, NULL);
    static detect_t d;
    setup_detect(&d);

    /* Diff of 3, threshold 5. Below threshold, no event. */
    uint64_t ts = 0;
    int emitted = feed_pair(&d, 50.0f, 47.0f, 20, &ts);
    CHECK_EQ(emitted, 0);
}

static void test_brief_divergence_no_event(void) {
    ingest_init(&g_ing, NULL);
    static detect_t d;
    setup_detect(&d);

    /* Divergence for 3 samples, sustained=5. Should not fire. */
    uint64_t ts = 0;
    int emitted = feed_pair(&d, 80.0f, 40.0f, 3, &ts);
    CHECK_EQ(emitted, 0);
}

static void test_sustained_divergence_fires(void) {
    ingest_init(&g_ing, NULL);
    static detect_t d;
    setup_detect(&d);

    /* Divergence for 10 samples, sustained=5. Should fire once. */
    uint64_t ts = 0;
    int emitted = feed_pair(&d, 80.0f, 40.0f, 10, &ts);
    CHECK_EQ(emitted, 1);

    CHECK(d.mismatch_state[0].in_violation);
}

static void test_ongoing_divergence_no_more_events(void) {
    ingest_init(&g_ing, NULL);
    static detect_t d;
    setup_detect(&d);

    /* Fire. */
    uint64_t ts = 0;
    feed_pair(&d, 80.0f, 40.0f, 10, &ts);
    CHECK(d.mismatch_state[0].in_violation);

    /* Continue diverging. No more events. */
    int emitted = feed_pair(&d, 80.0f, 40.0f, 20, &ts);
    CHECK_EQ(emitted, 0);
}

static void test_recovery_clears(void) {
    ingest_init(&g_ing, NULL);
    static detect_t d;
    setup_detect(&d);

    uint64_t ts = 0;
    feed_pair(&d, 80.0f, 40.0f, 10, &ts);
    CHECK(d.mismatch_state[0].in_violation);

    /* Back to agreement. */
    int emitted = feed_pair(&d, 50.0f, 50.0f, 5, &ts);
    CHECK_EQ(emitted, 1);
    CHECK(!d.mismatch_state[0].in_violation);
}

static void test_ctrl_below_actuator_also_fires(void) {
    ingest_init(&g_ing, NULL);
    static detect_t d;
    setup_detect(&d);

    /* ctrl < act. Still a mismatch, fires with INFO severity. */
    uint64_t ts = 0;
    int emitted = feed_pair(&d, 20.0f, 80.0f, 10, &ts);
    CHECK_EQ(emitted, 1);
    CHECK(d.mismatch_state[0].in_violation);
}

static void test_mismatch_pair_state_independent(void) {
    /* Two pairs, both configured. One diverges, the other doesn't.
     * Only the diverging one fires. */
    ingest_init(&g_ing, NULL);
    static detect_t d;
    setup_detect(&d);
    d.config.mismatch_pair_count = 2;
    d.config.mismatch_pairs[1].ctrl_out = 12;
    d.config.mismatch_pairs[1].actuator = 21;
    d.config.mismatch_pairs[1].threshold = 5.0f;

    uint64_t ts = 0;
    int emitted = 0;
    for (int i = 0; i < 10; i++) {
        sample_t s[4] = {
            mk_f32(11, 80.0f, ts),   /* pair 0: divergent */
            mk_f32(20, 40.0f, ts),
            mk_f32(12, 50.0f, ts),   /* pair 1: agreeing */
            mk_f32(21, 50.0f, ts),
        };
        packet_t p = mk_packet(ts, s, 4);
        ingest_feed(&g_ing, &p);
        event_t events[8];
        int e = detect_update(&d, &p, &g_ing, NULL, NULL, events, 8);
        if (e > 0) emitted += e;
        ts++;
    }
    CHECK_EQ(emitted, 1);
    CHECK(d.mismatch_state[0].in_violation);
    CHECK(!d.mismatch_state[1].in_violation);
}

static void test_no_pairs_no_events(void) {
    ingest_init(&g_ing, NULL);
    static detect_t d;
    detect_init_default(&d, 0);
    d.config.enable_range = false;
    d.config.enable_zscore = false;
    d.config.enable_flatline = false;
    d.config.enable_roc = false;
    d.config.enable_correlation = false;
    d.config.enable_mismatch = true;
    d.config.mismatch_pair_count = 0;

    uint64_t ts = 0;
    int emitted = 0;
    for (int i = 0; i < 20; i++) {
        sample_t s[2] = {
            mk_f32(11, 80.0f, ts),
            mk_f32(20, 40.0f, ts),
        };
        packet_t p = mk_packet(ts, s, 2);
        ingest_feed(&g_ing, &p);
        event_t events[8];
        int e = detect_update(&d, &p, &g_ing, NULL, NULL, events, 8);
        if (e > 0) emitted += e;
        ts++;
    }
    CHECK_EQ(emitted, 0);
}

int main(void) {
    plat_log_set_level(LOG_ERROR);

    test_agreeing_no_event();
    test_small_diff_no_event();
    test_brief_divergence_no_event();
    test_sustained_divergence_fires();
    test_ongoing_divergence_no_more_events();
    test_recovery_clears();
    test_ctrl_below_actuator_also_fires();
    test_mismatch_pair_state_independent();
    test_no_pairs_no_events();

    if (g_failures) {
        fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_detect_mismatch: all checks passed\n");
    return 0;
}