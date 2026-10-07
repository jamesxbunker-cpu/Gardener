#include "gardener/log_reader.h"
#include "gardener/log_writer.h"
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

/* Write N packets with timestamps start, start+step, start+2*step, ...
 * Each packet has one f32 sample with the given channel id. */
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

/* ------------------------------------------------------------------ */
/* Tests                                                              */
/* ------------------------------------------------------------------ */

static void test_open_missing_file(void) {
    log_reader_t *r = log_reader_open("test_lr_nonexistent.bin");
    CHECK(r == NULL);
}

static void test_open_null_arg(void) {
    CHECK(log_reader_open(NULL) == NULL);
}

static void test_close_null_safe(void) {
    log_reader_close(NULL);   /* must not crash */
}

static void test_open_empty_and_iterate(void) {
    const char *path = "test_lr_empty.bin";
    cleanup(path);

    /* Create an empty log. */
    {
        log_writer_t *w = log_writer_open(path, 1024 * 1024, NULL, true);
        CHECK(w != NULL);
        if (w) log_writer_close(w);
    }

    log_reader_t *r = log_reader_open(path);
    CHECK(r != NULL);
    if (r) {
        CHECK_EQ(log_reader_total_records(r), 0);
        CHECK_EQ(log_reader_wrap_count(r), 0);

        packet_t p;
        int rc = log_reader_next(r, &p);
        CHECK_EQ(rc, 0);   /* end of log */
        log_reader_close(r);
    }
    cleanup(path);
}

static void test_write_read_five(void) {
    const char *path = "test_lr_five.bin";
    cleanup(path);

    write_test_log(path, 5, 1000, 100, 1);

    log_reader_t *r = log_reader_open(path);
    CHECK(r != NULL);
    if (!r) { cleanup(path); return; }

    CHECK_EQ(log_reader_total_records(r), 5);

    packet_t p;
    int i = 0;
    for (;;) {
        int rc = log_reader_next(r, &p);
        if (rc == 0) break;
        CHECK_EQ(rc, 1);
        CHECK_EQ(p.timestamp_ns, 1000 + (uint64_t)i * 100);
        CHECK_EQ(p.channel_count, 1);
        CHECK_EQ(p.samples[0].channel_id, 1);
        CHECK(p.samples[0].v.f == (float)i);
        i++;
    }
    CHECK_EQ(i, 5);

    /* Further next() calls must return 0. */
    CHECK_EQ(log_reader_next(r, &p), 0);
    CHECK_EQ(log_reader_next(r, &p), 0);

    log_reader_close(r);
    cleanup(path);
}

static void test_rewind(void) {
    const char *path = "test_lr_rewind.bin";
    cleanup(path);

    write_test_log(path, 3, 1000, 10, 1);

    log_reader_t *r = log_reader_open(path);
    CHECK(r != NULL);
    if (!r) { cleanup(path); return; }

    packet_t p;
    CHECK_EQ(log_reader_next(r, &p), 1);
    CHECK_EQ(p.timestamp_ns, 1000);
    CHECK_EQ(log_reader_next(r, &p), 1);
    CHECK_EQ(p.timestamp_ns, 1010);

    /* Rewind, iterate again. */
    log_reader_rewind(r);
    CHECK_EQ(log_reader_next(r, &p), 1);
    CHECK_EQ(p.timestamp_ns, 1000);

    log_reader_close(r);
    cleanup(path);
}

static void test_registry_snapshot(void) {
    const char *path = "test_lr_registry.bin";
    cleanup(path);

    /* Write a log with a small registry. */
    registry_t reg;
    registry_init(&reg);
    reg.entries[0].id = 1;
    reg.entries[0].name = "temp";
    reg.entries[0].unit = "C";
    reg.entries[0].type = CH_F32;
    reg.entries[0].role = ROLE_SENSOR;
    reg.entries[0].min_valid = 0.0f;
    reg.entries[0].max_valid = 150.0f;
    reg.entries[0].has_range = true;

    reg.entries[1].id = 2;
    reg.entries[1].name = "pump";
    reg.entries[1].unit = "%";
    reg.entries[1].type = CH_F32;
    reg.entries[1].role = ROLE_ACTUATOR;
    reg.entries[1].controllable = true;

    reg.entries[2].id = 3;
    reg.entries[2].name = "running";
    reg.entries[2].type = CH_BOOL;
    reg.entries[2].role = ROLE_STATUS;
    reg.entries[2].is_regime = true;
    reg.count = 3;

    {
        log_writer_t *w = log_writer_open(path, 1024 * 1024, &reg, true);
        CHECK(w != NULL);
        if (w) {
            sample_t s = mk_f32(1, 20.5f, 1000);
            packet_t p = mk_packet(1000, 1, &s, 1);
            log_writer_append(w, &p);
            log_writer_close(w);
        }
    }

    log_reader_t *r = log_reader_open(path);
    CHECK(r != NULL);
    if (!r) { cleanup(path); return; }

    const registry_t *got = log_reader_registry(r);
    CHECK(got != NULL);
    CHECK_EQ(got->count, 3);
    if (got->count >= 3) {
        const channel_desc_t *c = &got->entries[0];
        CHECK_EQ(c->id, 1);
        CHECK(c->name && strcmp(c->name, "temp") == 0);
        CHECK(c->unit && strcmp(c->unit, "C") == 0);
        CHECK_EQ(c->type, CH_F32);
        CHECK_EQ(c->role, ROLE_SENSOR);
        CHECK(c->has_range);

        c = &got->entries[1];
        CHECK_EQ(c->id, 2);
        CHECK(c->controllable);

        c = &got->entries[2];
        CHECK_EQ(c->type, CH_BOOL);
        CHECK(c->is_regime);
    }

    log_reader_close(r);
    cleanup(path);
}

static void test_seek_ns(void) {
    const char *path = "test_lr_seek.bin";
    cleanup(path);

    /* Write packets with timestamps 0, 100, 200, ..., 900. */
    write_test_log(path, 10, 0, 100, 1);

    log_reader_t *r = log_reader_open(path);
    CHECK(r != NULL);
    if (!r) { cleanup(path); return; }

    packet_t p;

    /* Seek to 250: should land on 300. */
    CHECK_EQ(log_reader_seek_ns(r, 250), 0);
    CHECK_EQ(log_reader_next(r, &p), 1);
    CHECK_EQ(p.timestamp_ns, 300);

    /* Seek to exactly 500: should land on 500. */
    CHECK_EQ(log_reader_seek_ns(r, 500), 0);
    CHECK_EQ(log_reader_next(r, &p), 1);
    CHECK_EQ(p.timestamp_ns, 500);

    /* Seek to 0: should land on 0. */
    CHECK_EQ(log_reader_seek_ns(r, 0), 0);
    CHECK_EQ(log_reader_next(r, &p), 1);
    CHECK_EQ(p.timestamp_ns, 0);

    /* Seek to 9999 (past end): no record. next should return 0. */
    CHECK_EQ(log_reader_seek_ns(r, 9999), 0);
    int rc = log_reader_next(r, &p);
    CHECK_EQ(rc, 0);

    log_reader_close(r);
    cleanup(path);
}

static void test_null_safety(void) {
    packet_t p;
    CHECK(log_reader_next(NULL, &p) == -1);
    CHECK(log_reader_next(NULL, NULL) == -1);
    /* Cannot test log_reader_next(r, NULL) without a valid reader;
     * skip for now. */
    CHECK(log_reader_seek_ns(NULL, 0) == -1);
    CHECK(log_reader_total_records(NULL) == 0);
    CHECK(log_reader_wrap_count(NULL) == 0);
    CHECK(log_reader_header(NULL) == NULL);
    CHECK(log_reader_registry(NULL) == NULL);
    CHECK(log_reader_path(NULL) == NULL);
}

int main(void) {
    plat_log_set_level(LOG_ERROR);

    test_open_missing_file();
    test_open_null_arg();
    test_close_null_safe();
    test_open_empty_and_iterate();
    test_write_read_five();
    test_rewind();
    test_registry_snapshot();
    test_seek_ns();
    test_null_safety();

    if (g_failures) {
        fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_log_reader: all checks passed\n");
    return 0;
}