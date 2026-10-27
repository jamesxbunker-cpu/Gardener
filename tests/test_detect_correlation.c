#include "gardener/detect.h"
#include "gardener/ingest.h"
#include "gardener/packet.h"
#include "gardener/platform.h"

#include <stdio.h>
#include <string.h>
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

/* Set up a detector with one correlation pair (ch 1, ch 2). */
static void setup_detect(detect_t *d) {
    detect_init_default(d, 0);
    d->config.enable_range = false;
    d->config.enable_zscore = false;
    d->config.enable_flatline = false;
    d->config.enable_roc = false;
    d->config.enable_correlation = true;
    d->config.correlation_pair_count = 1;
    d->config.correlation_pairs[0].ch_a = 1;
    d->config.correlation_pairs[0].ch_b = 2;
    d->config.correlation_threshold = 0.5f;
}

/* Feed N samples where the two channels correlate (ch2 = ch1 + small noise). */
static int feed_correlated(detect_t *d, int n, uint64_t *ts_counter) {
    int emitted = 0;
    for (int i = 0; i < n; i++) {
        float v = (float)(i % 10);   /* sawtooth 0..9 */
        sample_t s[2] = {
            mk_f32(1, v, *ts_counter),
            mk_f32(2, v + 0.1f * ((i % 3) - 1), *ts_counter),  /* ±0.1 noise */
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

/* Feed N samples where the two channels do NOT correlate
 * (ch2 is independent random-ish values). */
static int feed_uncorrelated(detect_t *d, int n, uint64_t *ts_counter) {
    int emitted = 0;
    for (int i = 0; i < n; i++) {
        float va = (float)(i % 10);
        float vb = (float)((i * 7 + 3) % 10);   /* different period */
        sample_t s[2] = {
            mk_f32(1, va, *ts_counter),
            mk_f32(2, vb, *ts_counter),
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

static void test_correlated_no_event(void) {
    ingest_init(&g_ing, NULL);
    static detect_t d;
    setup_detect(&d);

    uint64_t ts = 0;
    int emitted = feed_correlated(&d, 100, &ts);
    CHECK_EQ(emitted, 0);
}

static void test_decoupling_fires(void) {
    ingest_init(&g_ing, NULL);
    static detect_t d;
    setup_detect(&d);

    /* First, establish correlation. */
    uint64_t ts = 0;
    feed_correlated(&d, 60, &ts);

    /* Then break it. Should fire. */
    int emitted = feed_uncorrelated(&d, 60, &ts);
    CHECK(emitted >= 1);

    if (emitted >= 1) {
        /* Verify the pair is now in violation. */
        CHECK(d.pair_state[0].in_violation);
    }
}

static void test_recovery_clears(void) {
    ingest_init(&g_ing, NULL);
    static detect_t d;
    setup_detect(&d);

    uint64_t ts = 0;
    feed_correlated(&d, 60, &ts);
    feed_uncorrelated(&d, 60, &ts);
    CHECK(d.pair_state[0].in_violation);

    /* Restore correlation. Should clear. */
    feed_correlated(&d, 60, &ts);
    CHECK(!d.pair_state[0].in_violation);
}

static void test_no_pairs_no_events(void) {
    ingest_init(&g_ing, NULL);
    static detect_t d;
    detect_init_default(&d, 0);
    d.config.enable_range = false;
    d.config.enable_zscore = false;
    d.config.enable_flatline = false;
    d.config.enable_roc = false;
    d.config.enable_correlation = true;
    d.config.correlation_pair_count = 0;   /* no pairs */

    uint64_t ts = 0;
    int emitted = 0;
    for (int i = 0; i < 60; i++) {
        sample_t s[2] = {
            mk_f32(1, (float)i, ts),
            mk_f32(2, (float)(-i), ts),
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

static void test_insufficient_history_no_fire(void) {
    ingest_init(&g_ing, NULL);
    static detect_t d;
    setup_detect(&d);

    uint64_t ts = 0;
    /* Only 10 samples per channel: less than DETECT_CORR_WINDOW. */
    int emitted = feed_uncorrelated(&d, 10, &ts);
    CHECK_EQ(emitted, 0);
}

int main(void) {
    plat_log_set_level(LOG_ERROR);
    test_correlated_no_event();
    test_decoupling_fires();
    test_recovery_clears();
    test_no_pairs_no_events();
    test_insufficient_history_no_fire();
    if (g_failures) {
        fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_detect_correlation: all checks passed\n");
    return 0;
}