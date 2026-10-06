#include "gardener/log.h"
#include "gardener/packet.h"
#include "gardener/platform.h"

#include <stdio.h>
#include <string.h>
#include <stddef.h>

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

/* ---- constants ---------------------------------------------------- */

static void test_constants(void) {
    CHECK_EQ(LOG_MAGIC, 0x47415244u);
    CHECK_EQ(LOG_VERSION, 1);
    CHECK_EQ(LOG_HEADER_SIZE, 4096);
    CHECK_EQ(LOG_DEFAULT_MAX, 100u * 1024u * 1024u);
    CHECK_EQ(LOG_MIN_MAX, 1024u * 1024u);
    CHECK(LOG_MIN_MAX < LOG_DEFAULT_MAX);

    CHECK_EQ(LOG_INDEX_MAGIC, 0x47494458u);
    CHECK_EQ(LOG_INDEX_VERSION, 1);
    CHECK_EQ(LOG_INDEX_HEADER, 64);
    CHECK_EQ(LOG_INDEX_ENTRY, 16);
    CHECK_EQ(LOG_INDEX_INTERVAL, 256);
}

/* ---- header layout ------------------------------------------------ */

static void test_header_size(void) {
    CHECK_EQ(sizeof(log_header_t), LOG_HEADER_SIZE);
}

static void test_header_offsets(void) {
    /* Lock the offsets of every field. Anyone who reorders the struct
     * has to update these and bump LOG_VERSION. */
    CHECK_EQ(offsetof(log_header_t, magic), 0);
    CHECK_EQ(offsetof(log_header_t, version), 4);
    CHECK_EQ(offsetof(log_header_t, header_size), 6);
    CHECK_EQ(offsetof(log_header_t, file_size), 8);
    CHECK_EQ(offsetof(log_header_t, data_start), 16);
    CHECK_EQ(offsetof(log_header_t, write_offset), 24);
    CHECK_EQ(offsetof(log_header_t, record_count), 32);
    CHECK_EQ(offsetof(log_header_t, first_ts_ns), 40);
    CHECK_EQ(offsetof(log_header_t, last_ts_ns), 48);
    CHECK_EQ(offsetof(log_header_t, start_wall_ns), 56);
    CHECK_EQ(offsetof(log_header_t, last_wall_ns), 64);
    CHECK_EQ(offsetof(log_header_t, channel_count), 72);
    CHECK_EQ(offsetof(log_header_t, registry_size), 76);
    CHECK_EQ(offsetof(log_header_t, header_crc), 80);
    CHECK_EQ(offsetof(log_header_t, wrap_count), 84);
}

/* ---- index layout ------------------------------------------------- */

static void test_index_header_size(void) {
    CHECK_EQ(sizeof(log_index_header_t), LOG_INDEX_HEADER);
}

static void test_index_entry_size(void) {
    CHECK_EQ(sizeof(log_index_entry_t), LOG_INDEX_ENTRY);
}

static void test_index_entry_offsets(void) {
    CHECK_EQ(offsetof(log_index_entry_t, timestamp_ns), 0);
    CHECK_EQ(offsetof(log_index_entry_t, file_offset), 8);
}

/* ---- record size formula ------------------------------------------ */

static void test_record_size_formula(void) {
    /* Matches packet_encode for a packet with N channels:
     *   20 header + N*18 samples + 2 CRC = 22 + 18*N */
    CHECK_EQ(log_record_size(0), 22);
    CHECK_EQ(log_record_size(1), 40);
    CHECK_EQ(log_record_size(2), 58);
    CHECK_EQ(log_record_size(3), 76);
    CHECK_EQ(log_record_size(PACKET_MAX_CH), 22 + 18 * 64);
}

static void test_record_size_matches_packet_encode(void) {
    /* Encode a real packet and confirm our formula matches. */
    for (uint16_t n = 0; n <= 5; n++) {
        packet_t p;
        memset(&p, 0, sizeof(p));
        p.channel_count = n;
        uint8_t buf[PACKET_BIN_MAX];
        int encoded = packet_encode(&p, buf, sizeof(buf));
        CHECK_EQ(encoded, (int)log_record_size(n));
    }
}

static void test_record_size_matches_packet_encode_max(void) {
    packet_t p;
    memset(&p, 0, sizeof(p));
    p.channel_count = PACKET_MAX_CH;
    uint8_t buf[PACKET_BIN_MAX];
    int encoded = packet_encode(&p, buf, sizeof(buf));
    CHECK_EQ(encoded, (int)log_record_size(PACKET_MAX_CH));
}

/* ---- sanity: a header and a few records fit in the minimum log ---- */

static void test_min_log_holds_header_and_records(void) {
    /* Even the minimum log size should hold the header, a small
     * registry, and at least a few max-size records. */
    uint64_t min_payload = LOG_MIN_MAX - LOG_HEADER_SIZE;
    uint64_t worst_record = log_record_size(PACKET_MAX_CH);
    /* 64 MB of worst-case packets in a 1 MB minimum log would be silly;
     * 1 MB / ~1.2 KB = ~870 records, so we're fine. Just assert it's
     * more than 100 records so the ring is meaningful. */
    CHECK(min_payload / worst_record > 100);
}

/* ---- registry snapshot budget ------------------------------------ */

static void test_registry_snapshot_fits_in_default_log(void) {
    /* The registry snapshot at 4096 should be small compared to the
     * file. Worst case: 256 channels, each ~128 bytes of JSONL. */
    uint64_t worst_registry = 256u * 128u;
    uint64_t available = LOG_DEFAULT_MAX - LOG_HEADER_SIZE;
    CHECK(worst_registry < available / 100);   /* < 1% of file */
}

int main(void) {
    test_constants();

    test_header_size();
    test_header_offsets();

    test_index_header_size();
    test_index_entry_size();
    test_index_entry_offsets();

    test_record_size_formula();
    test_record_size_matches_packet_encode();
    test_record_size_matches_packet_encode_max();

    test_min_log_holds_header_and_records();
    test_registry_snapshot_fits_in_default_log();

    if (g_failures) {
        fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_log_format: all checks passed\n");
    return 0;
}