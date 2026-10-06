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

/* Remove both the log and its derived index. */
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

/* Read back a log by decoding every record from data_start to
 * write_offset. Assumes no wrap has occurred. Returns the number of
 * packets decoded, and fills `out` (up to `max`). */
static int read_all_packets(const char *path, packet_t *out, int max) {
    plat_file_t *f = plat_file_open(path, PLAT_FILE_READ);
    if (!f) return -1;

    log_header_t h;
    if (plat_file_read(f, &h, sizeof(h)) != (int)sizeof(h)) {
        plat_file_close(f);
        return -1;
    }
    if (h.magic != LOG_MAGIC) {
        plat_file_close(f);
        return -1;
    }

    if (plat_file_seek(f, h.data_start) != 0) {
        plat_file_close(f);
        return -1;
    }

    int count = 0;
    uint64_t pos = h.data_start;
    while (pos < h.write_offset && count < max) {
        /* Read a fixed header first to learn channel_count. */
        uint8_t hdr[22];
        if (pos + sizeof(hdr) > h.write_offset) break;
        if (plat_file_read(f, hdr, sizeof(hdr)) != (int)sizeof(hdr)) break;

        uint16_t ch_n = (uint16_t)(hdr[16] | ((uint16_t)hdr[17] << 8));
        uint64_t record_size = 22 + 18ull * ch_n;
        if (pos + record_size > h.write_offset) break;

        /* Seek back to the start of the record and read the whole
         * thing. */
        if (plat_file_seek(f, pos) != 0) break;
        uint8_t buf[PACKET_BIN_MAX];
        if (record_size > sizeof(buf)) break;
        if (plat_file_read(f, buf, (size_t)record_size)
                != (int)record_size) break;

        if (packet_decode(buf, (size_t)record_size, &out[count]) !=
                (int)record_size) {
            break;
        }
        count++;
        pos += record_size;
    }

    plat_file_close(f);
    return count;
}

/* ------------------------------------------------------------------ */
/* Tests                                                              */
/* ------------------------------------------------------------------ */

static void test_open_create_close(void) {
    const char *path = "test_lw_create.bin";
    cleanup(path);

    log_writer_t *w = log_writer_open(path, 1024 * 1024, NULL, false);
    CHECK(w != NULL);
    if (w) {
        CHECK_EQ(log_writer_record_count(w), 0);
        CHECK_EQ(log_writer_wrap_count(w), 0);
        log_writer_close(w);
    }

    /* File should exist and be readable. */
    plat_file_t *f = plat_file_open(path, PLAT_FILE_READ);
    CHECK(f != NULL);
    if (f) {
        log_header_t h;
        CHECK(plat_file_read(f, &h, sizeof(h)) == (int)sizeof(h));
        CHECK_EQ(h.magic, LOG_MAGIC);
        CHECK_EQ(h.version, LOG_VERSION);
        CHECK_EQ(h.file_size, 1024 * 1024);
        plat_file_close(f);
    }
    cleanup(path);
}

static void test_append_one_and_read_back(void) {
    const char *path = "test_lw_one.bin";
    cleanup(path);

    log_writer_t *w = log_writer_open(path, 1024 * 1024, NULL, false);
    CHECK(w != NULL);
    if (!w) return;

    sample_t s = mk_f32(1, 20.5f, 500);
    packet_t p = mk_packet(1000, 7, &s, 1);

    int n = log_writer_append(w, &p);
    CHECK(n > 0);
    CHECK_EQ(n, 40);   /* 22 + 18 */

    CHECK_EQ(log_writer_record_count(w), 1);
    log_writer_close(w);

    /* Reopen the log and read the packet back. */
    packet_t out[4];
    int count = read_all_packets(path, out, 4);
    CHECK_EQ(count, 1);
    if (count >= 1) {
        CHECK_EQ(out[0].timestamp_ns, 1000);
        CHECK_EQ(out[0].source_id, 7);
        CHECK_EQ(out[0].channel_count, 1);
        CHECK_EQ(out[0].samples[0].channel_id, 1);
        CHECK(out[0].samples[0].v.f == 20.5f);
        CHECK_EQ(out[0].samples[0].timestamp_ns, 500);
    }

    cleanup(path);
}

static void test_append_many_and_read_back(void) {
    const char *path = "test_lw_many.bin";
    cleanup(path);

    log_writer_t *w = log_writer_open(path, 1024 * 1024, NULL, false);
    CHECK(w != NULL);
    if (!w) return;

    const int N = 100;
    for (int i = 0; i < N; i++) {
        sample_t s = mk_f32(1, (float)i, (uint64_t)(1000 + i));
        packet_t p = mk_packet((uint64_t)(1000 + i), 1, &s, 1);
        CHECK(log_writer_append(w, &p) > 0);
    }
    CHECK_EQ(log_writer_record_count(w), N);
    log_writer_close(w);

    packet_t out[128];
    int count = read_all_packets(path, out, 128);
    CHECK_EQ(count, N);
    for (int i = 0; i < count; i++) {
        if (out[i].timestamp_ns != (uint64_t)(1000 + i)) {
            fprintf(stderr, "FAIL record %d ts=%llu expected %d\n",
                    i, (unsigned long long)out[i].timestamp_ns, 1000 + i);
            g_failures++;
            break;
        }
    }

    cleanup(path);
}

static void test_reopen_appends(void) {
    const char *path = "test_lw_reopen.bin";
    cleanup(path);

    /* First session: 10 packets. */
    {
        log_writer_t *w = log_writer_open(path, 1024 * 1024, NULL, false);
        CHECK(w != NULL);
        if (w) {
            for (int i = 0; i < 10; i++) {
                sample_t s = mk_f32(1, (float)i, (uint64_t)i);
                packet_t p = mk_packet((uint64_t)i, 1, &s, 1);
                CHECK(log_writer_append(w, &p) > 0);
            }
            log_writer_close(w);
        }
    }

    /* Second session: 10 more packets, resuming. */
    {
        log_writer_t *w = log_writer_open(path, 1024 * 1024, NULL, false);
        CHECK(w != NULL);
        if (w) {
            CHECK_EQ(log_writer_record_count(w), 10);
            for (int i = 10; i < 20; i++) {
                sample_t s = mk_f32(1, (float)i, (uint64_t)i);
                packet_t p = mk_packet((uint64_t)i, 1, &s, 1);
                CHECK(log_writer_append(w, &p) > 0);
            }
            log_writer_close(w);
        }
    }

    packet_t out[32];
    int count = read_all_packets(path, out, 32);
    CHECK_EQ(count, 20);
    if (count > 0) {
        CHECK_EQ(out[0].timestamp_ns, 0);
        CHECK_EQ(out[19].timestamp_ns, 19);
    }

    cleanup(path);
}

static void test_wrap_overwrites_oldest(void) {
    /* Configure a log just big enough for a small number of packets,
     * then write more than that and confirm wrap_count rises. */
    const char *path = "test_lw_wrap.bin";
    cleanup(path);

    /* data_start is 4096 + registry_size. With a NULL registry,
     * data_start = 4096. Payload = max - 4096.
     *
     * LOG_MIN_MAX is 1 MB. Let's use exactly 1 MB. That gives
     * 1048576 - 4096 = 1044480 bytes of payload. Each 1-channel
     * packet is 40 bytes. That's ~26k packets. Writing 26k packets in
     * a test is slow. Instead, use many-channel packets so each is
     * big: a 64-channel packet is 1174 bytes. 1044480 / 1174 ≈ 890
     * packets. Still a lot, but fast enough.
     *
     * Actually, even 890 iterations is fine. Let's do it. */
    log_writer_t *w = log_writer_open(path, LOG_MIN_MAX, NULL, false);
    CHECK(w != NULL);
    if (!w) { cleanup(path); return; }

    /* Fill a 64-channel packet. */
    packet_t p;
    memset(&p, 0, sizeof(p));
    p.source_id = 1;
    p.channel_count = 64;
    for (uint16_t i = 0; i < 64; i++) {
        p.samples[i] = mk_f32((uint16_t)(i + 1), (float)i, 0);
    }

    /* Write enough to guarantee at least one wrap. */
    const int WRITES = 1500;
    for (int i = 0; i < WRITES; i++) {
        p.timestamp_ns = (uint64_t)(i * 10);
        for (uint16_t k = 0; k < 64; k++) {
            p.samples[k].timestamp_ns = p.timestamp_ns;
        }
        CHECK(log_writer_append(w, &p) > 0);
    }
    CHECK(log_writer_wrap_count(w) >= 1);
    CHECK_EQ(log_writer_record_count(w), WRITES);

    log_writer_close(w);
    cleanup(path);
}

static void test_refuse_invalid_header(void) {
    const char *path = "test_lw_bad.bin";
    cleanup(path);

    /* Create a valid log. */
    {
        log_writer_t *w = log_writer_open(path, 1024 * 1024, NULL, false);
        CHECK(w != NULL);
        if (w) {
            sample_t s = mk_f32(1, 1.0f, 0);
            packet_t p = mk_packet(0, 1, &s, 1);
            log_writer_append(w, &p);
            log_writer_close(w);
        }
    }

    /* Corrupt the magic. */
    {
        plat_file_t *f = plat_file_open(path, PLAT_FILE_UPDATE);
        CHECK(f != NULL);
        if (f) {
            uint32_t bad = 0xDEADBEEF;
            plat_file_seek(f, 0);
            plat_file_write(f, &bad, 4);
            plat_file_close(f);
        }
    }

    /* Reopening without start_fresh must refuse. */
    log_writer_t *w = log_writer_open(path, 1024 * 1024, NULL, false);
    CHECK(w == NULL);

    /* With start_fresh it should succeed. */
    w = log_writer_open(path, 1024 * 1024, NULL, true);
    CHECK(w != NULL);
    if (w) log_writer_close(w);

    cleanup(path);
}

static void test_min_size_refused(void) {
    log_writer_t *w = log_writer_open("test_lw_min.bin", 100, NULL, false);
    CHECK(w == NULL);
    cleanup("test_lw_min.bin");
}

static void test_null_args(void) {
    CHECK(log_writer_open(NULL, 1024 * 1024, NULL, false) == NULL);
    CHECK(log_writer_append(NULL, NULL) == -1);
    CHECK(log_writer_flush(NULL) == -1);
    log_writer_close(NULL);   /* must not crash */
}

int main(void) {
    plat_log_set_level(LOG_ERROR);   /* quiet expected warnings */

    test_open_create_close();
    test_append_one_and_read_back();
    test_append_many_and_read_back();
    test_reopen_appends();
    test_wrap_overwrites_oldest();
    test_refuse_invalid_header();
    test_min_size_refused();
    test_null_args();

    if (g_failures) {
        fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_log_writer: all checks passed\n");
    return 0;
}