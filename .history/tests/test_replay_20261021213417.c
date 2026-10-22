#include "gardener/replay.h"
#include "gardener/log_writer.h"
#include "gardener/log_reader.h"
#include "gardener/log.h"
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

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static void cleanup(const char *path) {
    remove(path);
    char idx[280];
    size_t n = strlen(path);
    if (n >= 4 && strcmp(path + n - 4, ".bin") == 0) {
        snprintf(idx, sizeof(idx), "%.*s.idx", (int)(n - 4), path);
    } else {
        snprintf(idx, sizeof(idx), "%s.idx", path);
    }
    remove(idx);
}

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

static packet_t mk_packet(uint64_t ts, uint16_t src,
                          const sample_t *samples, uint16_t n) {
    packet_t p;
    memset(&p, 0, sizeof(p));
    p.timestamp_ns = ts;
    p.source_id = src;
    p.channel_count = n;
    for (uint16_t i = 0; i < n; i++) p.samples[i] = samples[i];
    return p;
}

/* Write N packets with timestamps start, start+step, ... */
static void write_test_log(const char *path, int n,
                           uint64_t start_ts, uint64_t step,
                           uint16_t ch_id) {
    log_writer_t *w = log_writer_open(path, 1024 * 1024, NULL, true);
    if (!w) { fprintf(stderr, "write_test_log: open failed\n"); exit(2); }
    for (int i = 0; i < n; i++) {
        uint64_t ts = start_ts + (uint64_t)i * step;
        sample_t s = mk_f32(ch_id, (float)i, ts);
        packet_t p = mk_packet(ts, 1, &s, 1);
        if (log_writer_append(w, &p) < 0) {
            fprintf(stderr, "write_test_log: append failed at %d\n", i);
            exit(2);
        }
    }
    log_writer_close(w);
}

/* Fresh ingest, no config. */
static void fresh_ingest(ingest_t *ing) {
    if (ingest_init(ing, NULL) != 0) {
        fprintf(stderr, "ingest_init failed\n");
        exit(2);
    }
}

/* ------------------------------------------------------------------ */
/* Tests                                                              */
/* ------------------------------------------------------------------ */

static void test_opts_default(void) {
    replay_opts_t o;
    /* Fill with garbage first so we know default() clears it. */
    memset(&o, 0xFF, sizeof(o));
    replay_opts_default(&o);
    CHECK_EQ(o.speed_mode, REPLAY_SPEED_MAX);
    CHECK(o.speed_factor == 1.0);
    CHECK(o.log_path == NULL);
    CHECK_EQ(o.from_ts_ns, 0);
    CHECK_EQ(o.to_ts_ns, 0);
    CHECK(!o.loop);
    CHECK_EQ(o.progress_every, 0);
}

static void test_replay_null_args(void) {
    replay_opts_t opts;
    replay_opts_default(&opts);
    static ingest_t ing;
    replay_stats_t stats;

    CHECK(replay_run(NULL, &ing, &stats) == -1);
    CHECK(replay_run(&opts, NULL, &stats) == -1);
    CHECK(replay_run(&opts, &ing, NULL) == -1);
}

static void test_replay_missing_file(void) {
    replay_opts_t opts;
    replay_opts_default(&opts);
    opts.log_path = "test_replay_nonexistent.bin";
    static ingest_t ing;
    fresh_ingest(&ing);
    replay_stats_t stats;
    CHECK(replay_run(&opts, &ing, &stats) == -1);
}

static void test_replay_empty_log(void) {
    const char *path = "test_replay_empty.bin";
    cleanup(path);

    {
        log_writer_t *w = log_writer_open(path, 1024 * 1024, NULL, true);
        CHECK(w != NULL);
        if (w) log_writer_close(w);
    }

    replay_opts_t opts;
    replay_opts_default(&opts);
    opts.log_path = path;

    static ingest_t ing;
    fresh_ingest(&ing);
    replay_stats_t stats;

    CHECK(replay_run(&opts, &ing, &stats) == 0);
    CHECK_EQ(stats.packets_fed, 0);
    CHECK_EQ(stats.samples_fed, 0);
    CHECK_EQ(ing.packets_seen, 0);

    cleanup(path);
}

static void test_replay_ten_packets(void) {
    const char *path = "test_replay_ten.bin";
    cleanup(path);

    write_test_log(path, 10, 1000, 100, 1);

    replay_opts_t opts;
    replay_opts_default(&opts);
    opts.log_path = path;

    static ingest_t ing;
    fresh_ingest(&ing);
    replay_stats_t stats;

    CHECK(replay_run(&opts, &ing, &stats) == 0);
    CHECK_EQ(stats.packets_fed, 10);
    CHECK_EQ(stats.samples_fed, 10);
    CHECK_EQ(stats.packets_skipped, 0);
    CHECK_EQ(stats.total_loops, 0);
    CHECK_EQ(ing.packets_seen, 10);
    CHECK_EQ(ing.samples_seen, 10);

    /* Latest value should be the last packet's (i=9). */
    const sample_t *latest = ingest_latest(&ing, 1);
    CHECK(latest != NULL);
    CHECK(latest->v.f == 9.0f);

    cleanup(path);
}

static void test_replay_from_to(void) {
    const char *path = "test_replay_window.bin";
    cleanup(path);

    /* Timestamps 0, 100, 200, ..., 900. */
    write_test_log(path, 10, 0, 100, 1);

    replay_opts_t opts;
    replay_opts_default(&opts);
    opts.log_path = path;
    opts.from_ts_ns = 250;   /* skip 0, 100, 200 */
    opts.to_ts_ns   = 750;   /* stop before 800 */

    static ingest_t ing;
    fresh_ingest(&ing);
    replay_stats_t stats;

    CHECK(replay_run(&opts, &ing, &stats) == 0);
    /* Expect to feed packets with ts 300, 400, 500, 600, 700 = 5 packets. */
    CHECK_EQ(stats.packets_fed, 5);
    CHECK_EQ(ing.packets_seen, 5);

    /* Latest should be ts=700, value=7. */
    const sample_t *latest = ingest_latest(&ing, 1);
    CHECK(latest != NULL);
    CHECK(latest->v.f == 7.0f);

    cleanup(path);
}

static void test_replay_from_only(void) {
    const char *path = "test_replay_from.bin";
    cleanup(path);

    write_test_log(path, 10, 0, 100, 1);

    replay_opts_t opts;
    replay_opts_default(&opts);
    opts.log_path = path;
    opts.from_ts_ns = 500;

    static ingest_t ing;
    fresh_ingest(&ing);
    replay_stats_t stats;

    CHECK(replay_run(&opts, &ing, &stats) == 0);
    CHECK_EQ(stats.packets_fed, 5);   /* ts 500..900 */
    cleanup(path);
}

static void test_replay_to_only(void) {
    const char *path = "test_replay_to.bin";
    cleanup(path);

    write_test_log(path, 10, 0, 100, 1);

    replay_opts_t opts;
    replay_opts_default(&opts);
    opts.log_path = path;
    opts.to_ts_ns = 300;

    static ingest_t ing;
    fresh_ingest(&ing);
    replay_stats_t stats;

    CHECK(replay_run(&opts, &ing, &stats) == 0);
    CHECK_EQ(stats.packets_fed, 3);   /* ts 0, 100, 200 */
    cleanup(path);
}

static void test_replay_invalid_speed(void) {
    const char *path = "test_replay_speed.bin";
    cleanup(path);

    write_test_log(path, 3, 0, 100, 1);

    replay_opts_t opts;
    replay_opts_default(&opts);
    opts.log_path = path;
    opts.speed_mode = REPLAY_SPEED_SCALED;
    opts.speed_factor = 0.0;   /* invalid */

    static ingest_t ing;
    fresh_ingest(&ing);
    replay_stats_t stats;

    CHECK(replay_run(&opts, &ing, &stats) == -1);
    cleanup(path);
}

static void test_replay_max_is_fast(void) {
    /* 100 packets with 1s intervals would take 100s in real time, but
     * MAX mode should complete in well under a second. */
    const char *path = "test_replay_fast.bin";
    cleanup(path);

    write_test_log(path, 100, 0, 1000000000ull, 1);   /* 1s intervals */

    replay_opts_t opts;
    replay_opts_default(&opts);
    opts.log_path = path;
    opts.speed_mode = REPLAY_SPEED_MAX;

    static ingest_t ing;
    fresh_ingest(&ing);
    replay_stats_t stats;

    uint64_t t0 = plat_now_ns();
    CHECK(replay_run(&opts, &ing, &stats) == 0);
    uint64_t t1 = plat_now_ns();

    CHECK_EQ(stats.packets_fed, 100);
    /* Should be well under 1s. */
    CHECK(t1 - t0 < 1000000000ull);

    cleanup(path);
}

/* Note: we do not test REPLAY_SPEED_REAL or REPLAY_SPEED_SCALED with
 * actual timing verification here, because those introduce real sleeps
 * and would make the test slow and flaky. The delay computation is
 * simple enough to trust by inspection, and the modes are exercised by
 * manual verification. If they ever misbehave in practice, add a test
 * with a small number of packets (e.g. 3 packets at 10ms intervals)
 * that asserts a minimum elapsed time. */

int main(void) {
    plat_log_set_level(LOG_ERROR);

    test_opts_default();
    test_replay_null_args();
    test_replay_missing_file();
    test_replay_empty_log();
    test_replay_ten_packets();
    test_replay_from_to();
    test_replay_from_only();
    test_replay_to_only();
    test_replay_invalid_speed();
    test_replay_max_is_fast();

    if (g_failures) {
        fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_replay: all checks passed\n");
    return 0;
}