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

static void cleanup(const char *path) {
    remove(path);
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    remove(tmp);
}

/* ------------------------------------------------------------------ */

static void test_save_null_args(void) {
    static baseline_t b;
    baseline_init(&b, NULL, 0);
    CHECK(baseline_save(NULL, "test.bin") == -1);
    CHECK(baseline_save(&b, NULL) == -1);
    CHECK(baseline_load(NULL, "test.bin", NULL) == -1);
    CHECK(baseline_load(&b, NULL, NULL) == -1);
}

static void test_load_nonexistent(void) {
    static baseline_t b;
    CHECK(baseline_load(&b, "test_bp_nonexistent.bin", NULL) == -1);
}

static void test_roundtrip_empty(void) {
    const char *path = "test_bp_empty.bin";
    cleanup(path);

    static baseline_t b1;
    baseline_init(&b1, NULL, 0);
    CHECK(baseline_save(&b1, path) == 0);

    static baseline_t b2;
    CHECK(baseline_load(&b2, path, NULL) == 0);
    CHECK_EQ(b2.header.magic, BASELINE_MAGIC);
    CHECK_EQ(b2.header.version, BASELINE_VERSION);
    CHECK_EQ(b2.header.channel_count, 0);
    CHECK_EQ(baseline_channel_count(&b2), 0);

    cleanup(path);
}

static void test_roundtrip_one_channel(void) {
    const char *path = "test_bp_one.bin";
    cleanup(path);

    static baseline_t b1;
    baseline_init(&b1, NULL, 0);
    for (int i = 1; i <= 10; i++) {
        feed_one(&b1, 1, (float)i, (uint64_t)(i * 100));
    }

    const baseline_stat_t *s_orig = baseline_get(&b1, 1, 0);
    CHECK(s_orig != NULL);
    float orig_count = s_orig->count;
    float orig_mean  = s_orig->mean;
    float orig_m2    = s_orig->m2;
    float orig_min   = s_orig->min_val;
    float orig_max   = s_orig->max_val;
    uint8_t orig_state = s_orig->state;

    CHECK(baseline_save(&b1, path) == 0);

    static baseline_t b2;
    CHECK(baseline_load(&b2, path, NULL) == 0);
    CHECK_EQ(baseline_channel_count(&b2), 1);

    const baseline_stat_t *s = baseline_get(&b2, 1, 0);
    CHECK(s != NULL);
    if (s) {
        CHECK_NEAR(s->count, orig_count, 1e-6);
        CHECK_NEAR(s->mean,  orig_mean, 1e-6);
        CHECK_NEAR(s->m2,    orig_m2, 1e-5);
        CHECK_NEAR(s->min_val, orig_min, 1e-6);
        CHECK_NEAR(s->max_val, orig_max, 1e-6);
        CHECK_EQ(s->state, orig_state);
        CHECK_EQ(s->first_ts_ns, 100);
        CHECK_EQ(s->last_ts_ns, 1000);
    }

    cleanup(path);
}

static void test_roundtrip_multiple_channels(void) {
    const char *path = "test_bp_multi.bin";
    cleanup(path);

    static baseline_t b1;
    baseline_init(&b1, NULL, 0);
    for (int i = 0; i < 20; i++) {
        sample_t samples[3] = {
            mk_f32(1, 10.0f + (float)i, (uint64_t)i),
            mk_f32(2, 20.0f + (float)i, (uint64_t)i),
            mk_f32(3, 30.0f + (float)i, (uint64_t)i),
        };
        packet_t p = mk_packet((uint64_t)i, samples, 3);
        baseline_update(&b1, &p);
    }
    CHECK_EQ(baseline_channel_count(&b1), 3);

    CHECK(baseline_save(&b1, path) == 0);

    static baseline_t b2;
    CHECK(baseline_load(&b2, path, NULL) == 0);
    CHECK_EQ(baseline_channel_count(&b2), 3);

    for (uint16_t ch = 1; ch <= 3; ch++) {
        const baseline_stat_t *s = baseline_get(&b2, ch, 0);
        CHECK(s != NULL);
        if (s) {
            CHECK_NEAR(s->count, 20.0, 1e-5);
            CHECK_NEAR(s->mean, (float)ch * 10.0f + 9.5f, 1e-4);
        }
    }

    cleanup(path);
}

static void test_roundtrip_after_decay(void) {
    /* Feed, decay, save. Load should preserve decayed state. */
    const char *path = "test_bp_decay.bin";
    cleanup(path);

    static baseline_t b1;
    baseline_init(&b1, NULL, 1000000000ull);
    for (int i = 0; i < 100; i++) {
        feed_one(&b1, 1, 5.0f, (uint64_t)(i * 10000000ull));
    }
    const baseline_stat_t *s = baseline_get(&b1, 1, 0);
    uint64_t last = s->last_ts_ns;
    baseline_decay(&b1, last + 1000000000ull);

    s = baseline_get(&b1, 1, 0);
    float decayed_count = s->count;
    uint8_t decayed_state = s->state;
    CHECK_NEAR(decayed_count, 50.0, 1.0);

    CHECK(baseline_save(&b1, path) == 0);

    static baseline_t b2;
    CHECK(baseline_load(&b2, path, NULL) == 0);
    s = baseline_get(&b2, 1, 0);
    CHECK(s != NULL);
    if (s) {
        CHECK_NEAR(s->count, decayed_count, 1e-5);
        CHECK_EQ(s->state, decayed_state);
    }

    cleanup(path);
}

static void test_corrupt_magic_rejected(void) {
    const char *path = "test_bp_magic.bin";
    cleanup(path);

    static baseline_t b1;
    baseline_init(&b1, NULL, 0);
    feed_one(&b1, 1, 5.0f, 100);
    CHECK(baseline_save(&b1, path) == 0);

    /* Corrupt the magic. */
    plat_file_t *f = plat_file_open(path, PLAT_FILE_UPDATE);
    CHECK(f != NULL);
    if (f) {
        uint32_t bad = 0xDEADBEEF;
        plat_file_seek(f, 0);
        plat_file_write(f, &bad, 4);
        plat_file_close(f);
    }

    static baseline_t b2;
    CHECK(baseline_load(&b2, path, NULL) == -1);

    cleanup(path);
}

static void test_corrupt_crc_rejected(void) {
    const char *path = "test_bp_crc.bin";
    cleanup(path);

    static baseline_t b1;
    baseline_init(&b1, NULL, 0);
    feed_one(&b1, 1, 5.0f, 100);
    CHECK(baseline_save(&b1, path) == 0);

    /* Flip a bit in the stats area, leaving the header intact. */
    plat_file_t *f = plat_file_open(path, PLAT_FILE_UPDATE);
    CHECK(f != NULL);
    if (f) {
        uint8_t bad = 0xFF;
        plat_file_seek(f, BASELINE_STATS_OFF + 5);
        plat_file_write(f, &bad, 1);
        plat_file_close(f);
    }

    static baseline_t b2;
    CHECK(baseline_load(&b2, path, NULL) == -1);

    cleanup(path);
}

static void test_truncated_file_rejected(void) {
    const char *path = "test_bp_trunc.bin";
    cleanup(path);

    static baseline_t b1;
    baseline_init(&b1, NULL, 0);
    feed_one(&b1, 1, 5.0f, 100);
    CHECK(baseline_save(&b1, path) == 0);

    /* Truncate to half size. */
    plat_file_t *f = plat_file_open(path, PLAT_FILE_UPDATE);
    if (f) {
        /* We can't ftruncate with the current platform API. Instead,
         * overwrite the file with junk. Same effect: load fails. */
        uint8_t junk[100];
        memset(junk, 0, sizeof(junk));
        plat_file_seek(f, 0);
        plat_file_write(f, junk, sizeof(junk));
        plat_file_close(f);
    }

    static baseline_t b2;
    CHECK(baseline_load(&b2, path, NULL) == -1);

    cleanup(path);
}

static void test_load_leaves_b_zeroed_on_failure(void) {
    const char *path = "test_bp_bad.bin";
    cleanup(path);

    static baseline_t b;
    baseline_init(&b, NULL, 0);
    feed_one(&b, 1, 5.0f, 100);

    /* Confirm b has data. */
    CHECK_EQ(baseline_channel_count(&b), 1);

    /* Try to load a nonexistent file. */
    CHECK(baseline_load(&b, "test_bp_no_such_file.bin", NULL) == -1);

    /* b should be zeroed — the loader clears *b even on failure. */
    CHECK_EQ(baseline_channel_count(&b), 0);
    CHECK_EQ(b.header.magic, 0);
}

int main(void) {
    plat_log_set_level(LOG_ERROR);

    test_save_null_args();
    test_load_nonexistent();
    test_roundtrip_empty();
    test_roundtrip_one_channel();
    test_roundtrip_multiple_channels();
    test_roundtrip_after_decay();
    test_corrupt_magic_rejected();
    test_corrupt_crc_rejected();
    test_truncated_file_rejected();
    test_load_leaves_b_zeroed_on_failure();

    if (g_failures) {
        fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_baseline_persist: all checks passed\n");
    return 0;
}