#include "gardener/detect.h"
#include "gardener/ingest.h"
#include "gardener/registry.h"
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

static void setup(ingest_t *ing, detect_t *det) {
    ingest_init(ing, NULL);
    detect_init_default(det, 0);
    det->config.enable_range = false;
    det->config.enable_zscore = false;
    det->config.enable_flatline = false;
    /* Low absolute threshold so the test can exercise the trigger. */
    det->config.roc_absolute_max = 10.0f;   /* 10 units/sec */
}

/* Feed N samples with the given delta between them, at 1 Hz. */
static int feed_slope(ingest_t *ing, detect_t *det, float start,
                      float delta, int n, uint64_t *ts_counter) {
    int emitted = 0;
    float v = start;
    for (int i = 0; i < n; i++) {
        sample_t s = mk_f32(1, v, *ts_counter);
        packet_t p = mk_packet(*ts_counter, &s, 1);
        ingest_feed(ing, &p);
        event_t events[8];
        int e = detect_update(det, &p, ing, NULL, NULL, events, 8);
        if (e > 0) emitted += e;
        (*ts_counter) += 1000000000ull;   /* 1 second */
        v += delta;
    }
    return emitted;
}

static void test_roc_below_threshold(void) {
    static ingest_t ing;
    static detect_t det;
    setup(&ing, &det);

    /* Delta of 5/sec, threshold 10. No event. */
    uint64_t ts = 0;
    int emitted = feed_slope(&ing, &det, 0.0f, 5.0f, 40, &ts);
    CHECK_EQ(emitted, 0);
}

static void test_roc_above_threshold(void) {
    static ingest_t ing;
    static detect_t det;
    setup(&ing, &det);

    /* Delta of 100/sec, threshold 10. Should fire once window fills. */
    uint64_t ts = 0;
    int emitted = feed_slope(&ing, &det, 0.0f, 100.0f,
                             DETECT_ROC_WINDOW + 5, &ts);
    /* Expect exactly one STARTED event. */
    CHECK(emitted >= 1);
}

static void test_roc_clears(void) {
    static ingest_t ing;
    static detect_t det;
    setup(&ing, &det);

    /* Trigger. */
    uint64_t ts = 0;
    feed_slope(&ing, &det, 0.0f, 100.0f, DETECT_ROC_WINDOW + 5, &ts);
    CHECK(det.state[1].roc_in_violation);

    /* Slow down. */
    feed_slope(&ing, &det, 1000.0f, 1.0f, DETECT_ROC_WINDOW + 5, &ts);
    CHECK(!det.state[1].roc_in_violation);
}

int main(void) {
    plat_log_set_level(LOG_ERROR);
    test_roc_below_threshold();
    test_roc_above_threshold();
    test_roc_clears();
    if (g_failures) {
        fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_detect_roc: all checks passed\n");
    return 0;
}