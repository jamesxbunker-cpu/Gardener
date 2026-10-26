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

static sample_t mk_bool(uint16_t id, int v, uint64_t ts) {
    sample_t s;
    memset(&s, 0, sizeof(s));
    s.channel_id = id;
    s.type = CH_BOOL;
    s.role = ROLE_STATUS;
    s.flags = CH_FLAG_VALID;
    s.v.b = (uint8_t)(v ? 1 : 0);
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

/* Build a registry with the given channels marked as regime. */
static void build_registry(registry_t *r, const uint16_t *regime_ids, int n) {
    registry_init(r);
    for (int i = 0; i < n; i++) {
        channel_desc_t *c = &r->entries[r->count];
        memset(c, 0, sizeof(*c));
        c->id = regime_ids[i];
        c->type = CH_F32;
        c->role = ROLE_SENSOR;
        c->is_regime = true;
        r->count++;
    }
}

/* Feed a packet with one value sample (ch 1) and one regime sample
 * (ch 30, bool). */
static void feed_tick(baseline_t *b, float value, int running, uint64_t ts) {
    sample_t s[2] = {
        mk_f32(1, value, ts),
        mk_bool(30, running, ts),
    };
    packet_t p = mk_packet(ts, s, 2);
    baseline_update(b, &p);
}

/* ------------------------------------------------------------------ */
/* Tests                                                              */
/* ------------------------------------------------------------------ */

static void test_init_no_regime_channels(void) {
    static baseline_t b;
    CHECK(baseline_init(&b, NULL, 0) == 0);
    CHECK_EQ(b.regime_ch_count, 0);
    CHECK_EQ(b.header.regime_ch_count, 0);
    CHECK_EQ(b.header.channel_count, 0);
    CHECK_EQ(b.header.regime_count, 0);
}

static void test_init_one_regime_channel(void) {
    static registry_t reg;
    uint16_t ids[] = {30};
    build_registry(&reg, ids, 1);

    static baseline_t b;
    CHECK(baseline_init(&b, &reg, 0) == 0);
    CHECK_EQ(b.regime_ch_count, 1);
    CHECK_EQ(b.regime_ch_ids[0], 30);
    CHECK_EQ(b.header.regime_ch_ids[0], 30);
}

static void test_init_multiple_regime_channels(void) {
    static registry_t reg;
    uint16_t ids[] = {30, 31};
    build_registry(&reg, ids, 2);

    static baseline_t b;
    CHECK(baseline_init(&b, &reg, 0) == 0);
    CHECK_EQ(b.regime_ch_count, 2);
    CHECK_EQ(b.regime_ch_ids[0], 30);
    CHECK_EQ(b.regime_ch_ids[1], 31);
}

static void test_init_ignores_extra_regime_channels(void) {
    static registry_t reg;
    uint16_t ids[] = {30, 31, 32, 33, 34, 35};   /* 6, but max 4 */
    build_registry(&reg, ids, 6);

    static baseline_t b;
    CHECK(baseline_init(&b, &reg, 0) == 0);
    CHECK_EQ(b.regime_ch_count, BASELINE_MAX_REGIME_CH);
}

static void test_no_regime_channels_is_single_regime(void) {
    static baseline_t b;
    baseline_init(&b, NULL, 0);

    sample_t s = mk_f32(1, 5.0f, 100);
    packet_t p = mk_packet(100, &s, 1);
    baseline_update(&b, &p);

    /* All samples go to regime 0. */
    CHECK(baseline_get(&b, 1, 0) != NULL);
    CHECK(baseline_get(&b, 1, 1) == NULL);
    CHECK_EQ(baseline_regime_count(&b), 1);
}

static void test_one_regime_channel_creates_two_regimes(void) {
    static registry_t reg;
    uint16_t ids[] = {30};
    build_registry(&reg, ids, 1);

    static baseline_t b;
    baseline_init(&b, &reg, 0);

    /* Feed both running states. */
    for (int i = 0; i < 5; i++) {
        feed_tick(&b, 10.0f + (float)i, 0, (uint64_t)i);
        feed_tick(&b, 20.0f + (float)i, 1, (uint64_t)(100 + i));
    }

    /* Both regimes should have data for channel 1. */
    uint16_t r0 = 0xFFFF, r1 = 0xFFFF;
    for (uint16_t r = 0; r < BASELINE_MAX_REGIMES; r++) {
        if (baseline_get(&b, 1, r) != NULL) {
            if (r0 == 0xFFFF) r0 = r;
            else r1 = r;
        }
    }
    CHECK(r0 != 0xFFFF);
    CHECK(r1 != 0xFFFF);
    CHECK(r0 != r1);

    /* Means should be ~12 and ~22. */
    const baseline_stat_t *s0 = baseline_get(&b, 1, r0);
    const baseline_stat_t *s1 = baseline_get(&b, 1, r1);
    float m0 = s0->mean;
    float m1 = s1->mean;
    /* Identify which is which by mean. */
    float lo = m0 < m1 ? m0 : m1;
    float hi = m0 < m1 ? m1 : m0;
    CHECK_NEAR(lo, 12.0, 0.5);
    CHECK_NEAR(hi, 22.0, 0.5);

    CHECK_EQ(baseline_regime_count(&b), 2);
}

static void test_regime_is_stable_across_time(void) {
    /* Same regime-channel value → same regime index, every tick. */
    static registry_t reg;
    uint16_t ids[] = {30};
    build_registry(&reg, ids, 1);

    static baseline_t b;
    baseline_init(&b, &reg, 0);

    int regimes_seen[16] = {0};
    for (int i = 0; i < 10; i++) {
        sample_t s[2] = {
            mk_f32(1, 1.0f, (uint64_t)i),
            mk_bool(30, 1, (uint64_t)i),
        };
        packet_t p = mk_packet((uint64_t)i, s, 2);
        uint16_t r = baseline_regime_for_packet(&b, &p);
        CHECK(r < BASELINE_MAX_REGIMES);
        regimes_seen[r]++;
    }
    /* Only one regime should have been seen. */
    int count = 0;
    for (int i = 0; i < 16; i++) if (regimes_seen[i]) count++;
    CHECK_EQ(count, 1);
}

static void test_missing_regime_channel_skips_packet(void) {
    static registry_t reg;
    uint16_t ids[] = {30};
    build_registry(&reg, ids, 1);

    static baseline_t b;
    baseline_init(&b, &reg, 0);

    /* Packet with a sample for ch 1 but no sample for ch 30. */
    sample_t s = mk_f32(1, 5.0f, 100);
    packet_t p = mk_packet(100, &s, 1);
    int n = baseline_update(&b, &p);
    CHECK_EQ(n, 0);

    /* No channel registered. */
    CHECK_EQ(baseline_channel_count(&b), 0);
}

static void test_regime_for_packet_unknown(void) {
    static registry_t reg;
    uint16_t ids[] = {30};
    build_registry(&reg, ids, 1);
    static baseline_t b;
    baseline_init(&b, &reg, 0);

    /* Empty packet. */
    packet_t p;
    memset(&p, 0, sizeof(p));
    CHECK_EQ(baseline_regime_for_packet(&b, &p), 0xFFFF);
}

static void test_regime_for_packet_no_regime_channels(void) {
    static baseline_t b;
    baseline_init(&b, NULL, 0);

    /* Any packet returns regime 0. */
    sample_t s = mk_f32(1, 5.0f, 100);
    packet_t p = mk_packet(100, &s, 1);
    CHECK_EQ(baseline_regime_for_packet(&b, &p), 0);
}

static void test_regime_quantization(void) {
    /* Values within 0.001 should hash to the same regime. */
    static registry_t reg;
    uint16_t ids[] = {30};
    build_registry(&reg, ids, 1);
    static baseline_t b;
    baseline_init(&b, &reg, 0);

    sample_t s1[2] = { mk_f32(1, 1.0f, 0), mk_f32(30, 0.5f, 0) };
    packet_t p1 = mk_packet(0, s1, 2);
    uint16_t r1 = baseline_regime_for_packet(&b, &p1);

    sample_t s2[2] = { mk_f32(1, 1.0f, 0), mk_f32(30, 0.5005f, 0) };
    packet_t p2 = mk_packet(0, s2, 2);
    uint16_t r2 = baseline_regime_for_packet(&b, &p2);

    /* Within 0.001 → same regime. */
    CHECK_EQ(r1, r2);

    /* Different by > 0.001 → different regime (may collide, but
     * likely won't). */
    sample_t s3[2] = { mk_f32(1, 1.0f, 0), mk_f32(30, 0.51f, 0) };
    packet_t p3 = mk_packet(0, s3, 2);
    uint16_t r3 = baseline_regime_for_packet(&b, &p3);
    CHECK(r3 != r1);   /* extremely likely; if collides, hash is broken */
}

static void test_regime_with_multiple_channels(void) {
    static registry_t reg;
    uint16_t ids[] = {30, 31};
    build_registry(&reg, ids, 2);
    static baseline_t b;
    baseline_init(&b, &reg, 0);

    int combos[4][2] = {{0,0},{0,1},{1,0},{1,1}};
    uint16_t regimes[4];
    for (int i = 0; i < 4; i++) {
        sample_t s[3] = {
            mk_f32(1, 1.0f, 0),
            mk_bool(30, combos[i][0], 0),
            mk_bool(31, combos[i][1], 0),
        };
        packet_t p = mk_packet(0, s, 3);
        regimes[i] = baseline_regime_for_packet(&b, &p);
    }

    for (int i = 0; i < 4; i++) CHECK(regimes[i] < BASELINE_MAX_REGIMES);

    int distinct = 0;
    for (int i = 0; i < 4; i++) {
        int seen = 0;
        for (int j = 0; j < i; j++) if (regimes[j] == regimes[i]) seen = 1;
        if (!seen) distinct++;
    }
    CHECK(distinct >= 3);
}

static void test_get_null_for_unseen_regime(void) {
    static registry_t reg;
    uint16_t ids[] = {30};
    build_registry(&reg, ids, 1);
    static baseline_t b;
    baseline_init(&b, &reg, 0);

    /* Only feed running=1. */
    for (int i = 0; i < 5; i++) feed_tick(&b, 20.0f, 1, (uint64_t)i);

    /* Find which regime got the samples. */
    int found_regime = -1;
    for (uint16_t r = 0; r < BASELINE_MAX_REGIMES; r++) {
        if (baseline_get(&b, 1, r) != NULL) { found_regime = r; break; }
    }
    CHECK(found_regime != -1);

    /* All other regimes return NULL for channel 1. */
    for (uint16_t r = 0; r < BASELINE_MAX_REGIMES; r++) {
        if (r != (uint16_t)found_regime) {
            CHECK(baseline_get(&b, 1, r) == NULL);
        }
    }
}

int main(void) {
    plat_log_set_level(LOG_ERROR);

    test_init_no_regime_channels();
    test_init_one_regime_channel();
    test_init_multiple_regime_channels();
    test_init_ignores_extra_regime_channels();
    test_no_regime_channels_is_single_regime();
    test_one_regime_channel_creates_two_regimes();
    test_regime_is_stable_across_time();
    test_missing_regime_channel_skips_packet();
    test_regime_for_packet_unknown();
    test_regime_for_packet_no_regime_channels();
    test_regime_quantization();
    test_regime_with_multiple_channels();
    test_get_null_for_unseen_regime();

    if (g_failures) {
        fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_baseline_regimes: all checks passed\n");
    return 0;
}