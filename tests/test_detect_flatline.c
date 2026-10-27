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

static void setup_ingest_and_detect(ingest_t *ing, detect_t *det) {
    ingest_init(ing, NULL);
    detect_init_default(det, 0);
    /* Disable the detectors we aren't testing, to avoid noise. */
    det->config.enable_range = false;
    det->config.enable_zscore = false;
    det->config.enable_roc = false;
}

/* Feed N identical samples through ingest + detector. */
static int feed_constant(ingest_t *ing, detect_t *det, float v,
                         int n, uint64_t *ts_counter) {
    int emitted = 0;
    for (int i = 0; i < n; i++) {
        sample_t s = mk_f32(1, v, *ts_counter);
        packet_t p = mk_packet(*ts_counter, &s, 1);
        ingest_feed(ing, &p);

        event_t events[8];
        int e = detect_update(det, &p, ing, NULL, NULL, events, 8);
        if (e > 0) emitted += e;
        (*ts_counter)++;
    }
    return emitted;
}

static void test_flatline_after_window(void) {
    static ingest_t ing;
    static detect_t det;
    setup_ingest_and_detect(&ing, &det);

    /* Feed 31 identical samples: below window, no event. */
    uint64_t ts = 0;
    int emitted = feed_constant(&ing, &det, 42.0f,
                                DETECT_FLATLINE_WINDOW - 1, &ts);
    CHECK_EQ(emitted, 0);

    /* 32nd sample: window is full, all identical. Should fire. */
    emitted = feed_constant(&ing, &det, 42.0f, 1, &ts);
    CHECK_EQ(emitted, 1);

    /* Further identical samples: no more events. */
    emitted = feed_constant(&ing, &det, 42.0f, 10, &ts);
    CHECK_EQ(emitted, 0);
}

static void test_no_flatline_when_varying(void) {
    static ingest_t ing;
    static detect_t det;
    setup_ingest_and_detect(&ing, &det);

    uint64_t ts = 0;
    int emitted = 0;
    for (int i = 0; i < 100; i++) {
        float v = 42.0f + (float)(i % 3);   /* 42, 43, 44, 42, 43, 44, ... */
        sample_t s = mk_f32(1, v, ts);
        packet_t p = mk_packet(ts, &s, 1);
        ingest_feed(&ing, &p);
        event_t events[8];
        int e = detect_update(&det, &p, &ing, NULL, NULL, events, 8);
        if (e > 0) emitted += e;
        ts++;
    }
    CHECK_EQ(emitted, 0);
}

static void test_flatline_clears(void) {
    static ingest_t ing;
    static detect_t det;
    setup_ingest_and_detect(&ing, &det);

    /* Trigger flatline. */
    uint64_t ts = 0;
    feed_constant(&ing, &det, 42.0f, DETECT_FLATLINE_WINDOW + 5, &ts);
    CHECK(det.state[1].flatline_in_violation);

    /* Feed varied samples to break the flatline. */
    for (int i = 0; i < DETECT_FLATLINE_WINDOW; i++) {
        float v = 42.0f + (float)i;
        sample_t s = mk_f32(1, v, ts);
        packet_t p = mk_packet(ts, &s, 1);
        ingest_feed(&ing, &p);
        event_t events[8];
        detect_update(&det, &p, &ing, NULL, NULL, events, 8);
        ts++;
    }
    CHECK(!det.state[1].flatline_in_violation);
}

int main(void) {
    plat_log_set_level(LOG_ERROR);
    test_flatline_after_window();
    test_no_flatline_when_varying();
    test_flatline_clears();
    if (g_failures) {
        fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_detect_flatline: all checks passed\n");
    return 0;
}