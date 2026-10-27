#include "gardener/detect.h"
#include "gardener/registry.h"
#include "gardener/packet.h"
#include "gardener/platform.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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

static void setup_registry(registry_t *reg) {
    registry_init(reg);
    /* Channel 1: range [0, 100] */
    reg->entries[0].id = 1;
    reg->entries[0].name = "temp";
    reg->entries[0].type = CH_F32;
    reg->entries[0].role = ROLE_SENSOR;
    reg->entries[0].has_range = true;
    reg->entries[0].min_valid = 0.0f;
    reg->entries[0].max_valid = 100.0f;
    reg->count = 1;
}

static void test_init(void) {
    static detect_t d;
    CHECK_EQ(detect_init_default(&d, 0), 0);
    CHECK(d.config.enable_range);
    CHECK(d.config.enable_zscore);
    CHECK(d.config.zscore_threshold == 5.0f);
}

static void test_in_range_no_event(void) {
    static detect_t d;
    detect_init_default(&d, 0);

    static registry_t reg;
    setup_registry(&reg);

    sample_t s = mk_f32(1, 50.0f, 100);
    packet_t p = mk_packet(100, &s, 1);

    event_t events[8];
    int n = detect_update(&d, &p, NULL, &reg, events, 8);
    CHECK_EQ(n, 0);
}

static void test_out_of_range_emits_started(void) {
    static detect_t d;
    detect_init_default(&d, 0);

    static registry_t reg;
    setup_registry(&reg);

    sample_t s = mk_f32(1, 150.0f, 100);   /* > max=100 */
    packet_t p = mk_packet(100, &s, 1);

    event_t events[8];
    int n = detect_update(&d, &p, NULL, &reg, events, 8);
    CHECK_EQ(n, 1);
    if (n >= 1) {
        CHECK_EQ(events[0].detector, DETECTOR_RANGE);
        CHECK_EQ(events[0].state, EVENT_STATE_STARTED);
        CHECK_EQ(events[0].channel_id, 1);
        CHECK(events[0].observed == 150.0f);
        CHECK(events[0].threshold == 100.0f);
    }
}

static void test_ongoing_violation_no_event(void) {
    static detect_t d;
    detect_init_default(&d, 0);
    static registry_t reg;
    setup_registry(&reg);

    /* First packet: out of range, emits STARTED. */
    sample_t s = mk_f32(1, 150.0f, 100);
    packet_t p = mk_packet(100, &s, 1);
    event_t events[8];
    CHECK_EQ(detect_update(&d, &p, NULL, &reg, events, 8), 1);

    /* Second packet: still out of range, no event. */
    s = mk_f32(1, 160.0f, 200);
    p = mk_packet(200, &s, 1);
    CHECK_EQ(detect_update(&d, &p, NULL, &reg, events, 8), 0);
}

static void test_back_in_range_emits_cleared(void) {
    static detect_t d;
    detect_init_default(&d, 0);
    static registry_t reg;
    setup_registry(&reg);

    /* Out of range. */
    sample_t s = mk_f32(1, 150.0f, 100);
    packet_t p = mk_packet(100, &s, 1);
    event_t events[8];
    detect_update(&d, &p, NULL, &reg, events, 8);

    /* Back in range. */
    s = mk_f32(1, 50.0f, 200);
    p = mk_packet(200, &s, 1);
    int n = detect_update(&d, &p, NULL, &reg, events, 8);
    CHECK_EQ(n, 1);
    if (n >= 1) {
        CHECK_EQ(events[0].detector, DETECTOR_RANGE);
        CHECK_EQ(events[0].state, EVENT_STATE_CLEARED);
    }
}

static void test_below_min_emits_started(void) {
    static detect_t d;
    detect_init_default(&d, 0);
    static registry_t reg;
    setup_registry(&reg);

    sample_t s = mk_f32(1, -10.0f, 100);   /* < min=0 */
    packet_t p = mk_packet(100, &s, 1);
    event_t events[8];
    int n = detect_update(&d, &p, NULL, &reg, events, 8);
    CHECK_EQ(n, 1);
    if (n >= 1) {
        CHECK(events[0].threshold == 0.0f);   /* lower bound */
    }
}

static void test_no_range_config_skips(void) {
    static detect_t d;
    detect_init_default(&d, 0);

    static registry_t reg;
    registry_init(&reg);
    /* Channel with no range configured. */
    reg.entries[0].id = 1;
    reg.entries[0].name = "temp";
    reg.entries[0].type = CH_F32;
    reg.entries[0].role = ROLE_SENSOR;
    reg.entries[0].has_range = false;
    reg.count = 1;

    sample_t s = mk_f32(1, 10000.0f, 100);
    packet_t p = mk_packet(100, &s, 1);
    event_t events[8];
    int n = detect_update(&d, &p, NULL, &reg, events, 8);
    CHECK_EQ(n, 0);
}

static void test_stale_sample_skipped(void) {
    static detect_t d;
    detect_init_default(&d, 0);
    static registry_t reg;
    setup_registry(&reg);

    sample_t s = mk_f32(1, 150.0f, 100);
    s.flags = CH_FLAG_STALE;
    packet_t p = mk_packet(100, &s, 1);
    event_t events[8];
    CHECK_EQ(detect_update(&d, &p, NULL, &reg, events, 8), 0);
}

static void test_null_args(void) {
    static detect_t d;
    detect_init_default(&d, 0);
    static registry_t reg;
    setup_registry(&reg);

    packet_t p;
    memset(&p, 0, sizeof(p));
    event_t events[8];

    CHECK_EQ(detect_update(NULL, &p, NULL, &reg, events, 8), -1);
    CHECK_EQ(detect_update(&d, NULL, NULL, &reg, events, 8), -1);
    CHECK_EQ(detect_update(&d, &p, NULL, &reg, NULL, 8), -1);
}

int main(void) {
    plat_log_set_level(LOG_ERROR);

    test_init();
    test_in_range_no_event();
    test_out_of_range_emits_started();
    test_ongoing_violation_no_event();
    test_back_in_range_emits_cleared();
    test_below_min_emits_started();
    test_no_range_config_skips();
    test_stale_sample_skipped();
    test_null_args();

    if (g_failures) {
        fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_detect_range: all checks passed\n");
    return 0;
}