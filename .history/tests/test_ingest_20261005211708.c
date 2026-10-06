#include "gardener/ingest.h"
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

/* Write a minimal channels.toml for the tests. Returns the path. */
static const char *write_cfg(void) {
    const char *path = "test_ingest_channels.toml";
    FILE *f = fopen(path, "w");
    if (!f) { perror("fopen"); exit(2); }
    fputs(
        "[[channel]]\n"
        "id = 1\n"
        "name = \"temp\"\n"
        "unit = \"C\"\n"
        "type = \"f32\"\n"
        "role = \"sensor\"\n"
        "min = 0\n"
        "max = 150\n"
        "\n"
        "[[channel]]\n"
        "id = 2\n"
        "name = \"pump\"\n"
        "unit = \"%\"\n"
        "type = \"f32\"\n"
        "role = \"actuator\"\n"
        "controllable = true\n"
        "\n"
        "[[channel]]\n"
        "id = 3\n"
        "name = \"running\"\n"
        "type = \"bool\"\n"
        "role = \"status\"\n"
        "is_regime = true\n",
        f);
    fclose(f);
    return path;
}

/* Initialize an ingest and register cleanup with atexit. */
static void init_ingest(ingest_t *ing, const char *cfg) {
    if (ingest_init(ing, cfg) != 0) {
        fprintf(stderr, "ingest_init failed\n");
        exit(2);
    }
}

static sample_t mk_f32(uint16_t id, float v) {
    sample_t s;
    memset(&s, 0, sizeof(s));
    s.channel_id = id;
    s.type       = CH_F32;
    s.flags      = 0;
    s.v.f        = v;
    return s;
}

static sample_t mk_bool(uint16_t id, int v) {
    sample_t s;
    memset(&s, 0, sizeof(s));
    s.channel_id = id;
    s.type       = CH_BOOL;
    s.v.b        = (uint8_t)(v ? 1 : 0);
    return s;
}

static packet_t mk_packet(uint64_t ts, uint16_t src, sample_t *samples, uint16_t n) {
    packet_t p;
    memset(&p, 0, sizeof(p));
    p.timestamp_ns = ts;
    p.source_id    = src;
    p.channel_count = n;
    for (uint16_t i = 0; i < n; i++) p.samples[i] = samples[i];
    return p;
}

static void test_init_rejects_null(void) {
    CHECK(ingest_init(NULL, NULL) == -1);
}

static void test_init_without_config(void) {
    static ingest_t ing;
    CHECK(ingest_init(&ing, NULL) == 0);
    CHECK_EQ(ing.packets_seen, 0);
    CHECK_EQ(ing.samples_seen, 0);
    CHECK_EQ(ing.registry.count, 0);
}
static void test_feed_null_safety(void) {
    static ingest_t ing;
    init_ingest(&ing, NULL);

    packet_t p;
    memset(&p, 0, sizeof(p));

    CHECK(ingest_feed(NULL, NULL) == -1);
    CHECK(ingest_feed(&ing, NULL) == -1);
    CHECK(ingest_feed(NULL, &p) == -1);
}

static void test_single_channel_roundtrip(void) {
    const char *cfg = write_cfg();
    static ingest_t ing;
    init_ingest(&ing, cfg);

    sample_t s = mk_f32(1, 20.5f);
    packet_t p = mk_packet(1000, 1, &s, 1);

    int n = ingest_feed(&ing, &p);
    CHECK_EQ(n, 1);
    CHECK_EQ(ing.packets_seen, 1);
    CHECK_EQ(ing.samples_seen, 1);
    CHECK_EQ(ing.samples_dropped, 0);
    CHECK_EQ(ing.unknown_channels, 0);

    CHECK(ingest_has_channel(&ing, 1));
    const sample_t *latest = ingest_latest(&ing, 1);
    CHECK(latest != NULL);
    CHECK(latest->v.f == 20.5f);
    CHECK_EQ(latest->timestamp_ns, 1000);
    /* VALID must be set, STALE must not. */
    CHECK((latest->flags & CH_FLAG_VALID) != 0);
    CHECK((latest->flags & CH_FLAG_STALE) == 0);

    /* role and type should have been filled from the descriptor */
    CHECK_EQ(latest->type, CH_F32);
    CHECK_EQ(latest->role, ROLE_SENSOR);

    remove(cfg);
}

static void test_three_channels_one_packet(void) {
    const char *cfg = write_cfg();
    static ingest_t ing;
    init_ingest(&ing, cfg);

    sample_t s[3] = { mk_f32(1, 20.0f), mk_f32(2, 50.0f), mk_bool(3, 1) };
    packet_t p = mk_packet(2000, 1, s, 3);

    CHECK_EQ(ingest_feed(&ing, &p), 3);
    CHECK_EQ(ing.samples_seen, 3);

    CHECK(ingest_latest(&ing, 1) != NULL);
    CHECK(ingest_latest(&ing, 2) != NULL);
    CHECK(ingest_latest(&ing, 3) != NULL);

    CHECK(ingest_latest(&ing, 1)->v.f == 20.0f);
    CHECK(ingest_latest(&ing, 2)->v.f == 50.0f);
    CHECK(ingest_latest(&ing, 3)->v.b == 1);
    CHECK_EQ(ingest_latest(&ing, 3)->type, CH_BOOL);

    remove(cfg);
}

static void test_ring_accumulates(void) {
    const char *cfg = write_cfg();
    static ingest_t ing;
    init_ingest(&ing, cfg);

    for (int i = 0; i < 10; i++) {
        sample_t s = mk_f32(1, (float)i);
        packet_t p = mk_packet((uint64_t)(1000 + i * 100), 1, &s, 1);
        CHECK_EQ(ingest_feed(&ing, &p), 1);
    }

    const channel_state_t *cs = ingest_state(&ing, 1);
    CHECK(cs != NULL);
    CHECK(cs->seen);
    CHECK_EQ(ring_count(&cs->ring), 10);
    CHECK_EQ(ring_at(&cs->ring, 0)->v.f, 0.0f);
    CHECK_EQ(ring_at(&cs->ring, 9)->v.f, 9.0f);
    CHECK_EQ(cs->latest.v.f, 9.0f);

    remove(cfg);
}

static void test_ring_wraps(void) {
    /* Fill past capacity and confirm oldest is dropped. Uses a small
     * cap by pushing INGEST_RING_CAP + 5 samples. */
    const char *cfg = write_cfg();
    static ingest_t ing;
    init_ingest(&ing, cfg);

    const int N = INGEST_RING_CAP + 5;
    for (int i = 0; i < N; i++) {
        sample_t s = mk_f32(1, (float)i);
        packet_t p = mk_packet((uint64_t)i, 1, &s, 1);
        ingest_feed(&ing, &p);
    }

    const channel_state_t *cs = ingest_state(&ing, 1);
    CHECK_EQ(ring_count(&cs->ring), INGEST_RING_CAP);
    CHECK(ring_full(&cs->ring));
    /* Oldest should be sample 5 (0..4 overwritten). */
    CHECK_EQ((int)ring_oldest(&cs->ring)->v.f, 5);
    CHECK_EQ((int)cs->latest.v.f, N - 1);

    remove(cfg);
}

static void test_unknown_channel_synthesized(void) {
    const char *cfg = write_cfg();
    static ingest_t ing;
    init_ingest(&ing, cfg);

    uint16_t before = (uint16_t)ing.registry.count;
    sample_t s = mk_f32(99, 42.0f);     /* not in config */
    packet_t p = mk_packet(1000, 1, &s, 1);

    CHECK_EQ(ingest_feed(&ing, &p), 1);
    CHECK_EQ(ing.unknown_channels, 1);
    CHECK_EQ(ing.registry.count, before + 1);

    const channel_desc_t *d = registry_get(&ing.registry, 99);
    CHECK(d != NULL);
    CHECK_EQ(d->role, ROLE_UNKNOWN);

    CHECK(ingest_has_channel(&ing, 99));
    CHECK(ingest_latest(&ing, 99)->v.f == 42.0f);

    remove(cfg);
}

static void test_unknown_channel_is_cached(void) {
    const char *cfg = write_cfg();
    static ingest_t ing;
    init_ingest(&ing, cfg);

    uint16_t before = (uint16_t)ing.registry.count;
    sample_t s1 = mk_f32(99, 1.0f);
    sample_t s2 = mk_f32(99, 2.0f);
    packet_t p1 = mk_packet(1000, 1, &s1, 1);
    packet_t p2 = mk_packet(2000, 1, &s2, 1);
    ingest_feed(&ing, &p1);
    ingest_feed(&ing, &p2);

    /* Only one entry should have been synthesized for id 99. */
    CHECK_EQ(ing.registry.count, before + 1);
    CHECK_EQ(ing.unknown_channels, 2);  /* two feeds, one registration each */
    remove(cfg);
}

static void test_timestamp_inheritance(void) {
    const char *cfg = write_cfg();
    static ingest_t ing;
    init_ingest(&ing, cfg);

    sample_t s = mk_f32(1, 20.0f);
    s.timestamp_ns = 0;                 /* let packet provide it */
    packet_t p = mk_packet(9999, 1, &s, 1);
    ingest_feed(&ing, &p);

    CHECK_EQ(ingest_latest(&ing, 1)->timestamp_ns, 9999);
    remove(cfg);
}

static void test_timestamp_override(void) {
    const char *cfg = write_cfg();
    static ingest_t ing;
    init_ingest(&ing, cfg);

    sample_t s = mk_f32(1, 20.0f);
    s.timestamp_ns = 1234;              /* sample wins */
    packet_t p = mk_packet(9999, 1, &s, 1);
    ingest_feed(&ing, &p);

    CHECK_EQ(ingest_latest(&ing, 1)->timestamp_ns, 1234);
    remove(cfg);
}

static void test_simulated_flag_propagates(void) {
    const char *cfg = write_cfg();
    static ingest_t ing;
    init_ingest(&ing, cfg);

    sample_t s = mk_f32(1, 20.0f);
    packet_t p = mk_packet(1000, 1, &s, 1);
    p.flags = PACKET_FLAG_SIMULATED;
    ingest_feed(&ing, &p);

    CHECK((ingest_latest(&ing, 1)->flags & CH_FLAG_SIMULATED) != 0);
    remove(cfg);
}

static void test_stale_flag_preserved(void) {
    const char *cfg = write_cfg();
    static ingest_t ing;
    init_ingest(&ing, cfg);

    sample_t s = mk_f32(1, 20.0f);
    s.flags = CH_FLAG_STALE;
    packet_t p = mk_packet(1000, 1, &s, 1);
    ingest_feed(&ing, &p);

    const sample_t *l = ingest_latest(&ing, 1);
    CHECK((l->flags & CH_FLAG_STALE) != 0);
    /* STALE samples must not be marked VALID. */
    CHECK((l->flags & CH_FLAG_VALID) == 0);
    remove(cfg);
}

static void test_out_of_range_id_dropped(void) {
    const char *cfg = write_cfg();
    static ingest_t ing;
    init_ingest(&ing, cfg);

    sample_t s = mk_f32(0, 0.0f);
    s.channel_id = REGISTRY_MAX_CHANNELS + 5;
    packet_t p = mk_packet(1000, 1, &s, 1);

    CHECK_EQ(ingest_feed(&ing, &p), 0);
    CHECK_EQ(ing.samples_seen, 0);
    CHECK_EQ(ing.samples_dropped, 1);
    remove(cfg);
}

static void test_too_many_channels_rejected(void) {
    const char *cfg = write_cfg();
    static ingest_t ing;
    init_ingest(&ing, cfg);

    packet_t p;
    memset(&p, 0, sizeof(p));
    p.channel_count = PACKET_MAX_CH + 1;

    CHECK(ingest_feed(&ing, &p) == -1);
    /* packet counter should not have been incremented for a rejected
     * packet — the feed never got past validation. */
    CHECK_EQ(ing.packets_seen, 0);
    remove(cfg);
}

static void test_registry_fill_from_descriptor(void) {
    /* A packet with type=0, role=0 should inherit from the descriptor. */
    const char *cfg = write_cfg();
    static ingest_t ing;
    init_ingest(&ing, cfg);

    sample_t s;
    memset(&s, 0, sizeof(s));
    s.channel_id = 2;   /* actuator in config */
    s.v.f = 75.0f;
    /* type and role deliberately left 0 */
    packet_t p = mk_packet(1000, 1, &s, 1);
    ingest_feed(&ing, &p);

    const sample_t *l = ingest_latest(&ing, 2);
    CHECK(l != NULL);
    CHECK_EQ(l->type, CH_F32);
    CHECK_EQ(l->role, ROLE_ACTUATOR);
    remove(cfg);
}

static void test_mixed_known_unknown(void) {
    const char *cfg = write_cfg();
    static ingest_t ing;
    init_ingest(&ing, cfg);

    sample_t s[3] = {
        mk_f32(1, 20.0f),   /* known */
        mk_f32(77, 30.0f),  /* unknown, will be synthesized */
        mk_f32(2, 50.0f),   /* known */
    };
    packet_t p = mk_packet(1000, 1, s, 3);

    CHECK_EQ(ingest_feed(&ing, &p), 3);
    CHECK_EQ(ing.unknown_channels, 1);
    CHECK(ingest_has_channel(&ing, 1));
    CHECK(ingest_has_channel(&ing, 77));
    CHECK(ingest_has_channel(&ing, 2));
    remove(cfg);
}

static void test_has_channel_false_when_never_seen(void) {
    const char *cfg = write_cfg();
    static ingest_t ing;
    init_ingest(&ing, cfg);

    CHECK(!ingest_has_channel(&ing, 1));
    CHECK(ingest_latest(&ing, 1) == NULL);
    CHECK(ingest_state(&ing, 1) != NULL);   /* state exists, .seen is false */
    CHECK(!ingest_state(&ing, 1)->seen);
    remove(cfg);
}

static void test_state_and_latest_bounds(void) {
    const char *cfg = write_cfg();
    static ingest_t ing;
    init_ingest(&ing, cfg);

    CHECK(ingest_latest(&ing, REGISTRY_MAX_CHANNELS) == NULL);
    CHECK(ingest_state(&ing, REGISTRY_MAX_CHANNELS) == NULL);
    CHECK(!ingest_has_channel(&ing, REGISTRY_MAX_CHANNELS));
    remove(cfg);
}

int main(void) {
    /* Suppress expected warning noise from the unknown-channel tests. */
    plat_log_set_level(LOG_ERROR);

    test_init_rejects_null();
    test_init_without_config();
    test_feed_null_safety();
    test_single_channel_roundtrip();
    test_three_channels_one_packet();
    test_ring_accumulates();
    test_ring_wraps();
    test_unknown_channel_synthesized();
    test_unknown_channel_is_cached();
    test_timestamp_inheritance();
    test_timestamp_override();
    test_simulated_flag_propagates();
    test_stale_flag_preserved();
    test_out_of_range_id_dropped();
    test_too_many_channels_rejected();
    test_registry_fill_from_descriptor();
    test_mixed_known_unknown();
    test_has_channel_false_when_never_seen();
    test_state_and_latest_bounds();

    if (g_failures) {
        fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_ingest: all checks passed\n");
    return 0;
}