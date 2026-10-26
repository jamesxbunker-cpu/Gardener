#include "gardener/baseline.h"
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
    CHECK_EQ(BASELINE_MAGIC, 0x42415345u);   /* "BASE" */
    CHECK_EQ(BASELINE_VERSION, 1);
    CHECK_EQ(BASELINE_HEADER_SIZE, 64);
    CHECK_EQ(BASELINE_MAX_CHANNELS, 256);
    CHECK_EQ(BASELINE_MAX_REGIMES, 16);
    CHECK_EQ(BASELINE_MAX_REGIME_CH, 4);
    CHECK_EQ(BASELINE_STAT_SIZE, 40);   /* was 32 */
    CHECK_EQ(BASELINE_CHANNEL_ENTRY_SIZE, 8);
    CHECK_EQ(BASELINE_MIN_SAMPLES, 100);
    CHECK_EQ(BASELINE_DEFAULT_HALFLIFE_NS, 300ull * 1000000000ull);
}

/* ---- struct sizes ------------------------------------------------- */

static void test_header_size(void) {
    CHECK_EQ(sizeof(baseline_header_t), BASELINE_HEADER_SIZE);
}

static void test_channel_entry_size(void) {
    CHECK_EQ(sizeof(baseline_channel_entry_t), BASELINE_CHANNEL_ENTRY_SIZE);
}

static void test_stat_size(void) {
    CHECK_EQ(sizeof(baseline_stat_t), BASELINE_STAT_SIZE);
}

static void test_trailer_size(void) {
    CHECK_EQ(sizeof(baseline_trailer_t), 4);
}

/* ---- struct field offsets ---------------------------------------- */

static void test_header_offsets(void) {
    CHECK_EQ(offsetof(baseline_header_t, magic), 0);
    CHECK_EQ(offsetof(baseline_header_t, version), 4);
    CHECK_EQ(offsetof(baseline_header_t, header_size), 6);
    CHECK_EQ(offsetof(baseline_header_t, channel_count), 8);
    CHECK_EQ(offsetof(baseline_header_t, regime_count), 12);
    CHECK_EQ(offsetof(baseline_header_t, created_ns), 16);
    CHECK_EQ(offsetof(baseline_header_t, saved_ns), 24);
    CHECK_EQ(offsetof(baseline_header_t, halflife_ns), 32);
    CHECK_EQ(offsetof(baseline_header_t, regime_ch_ids), 40);
    CHECK_EQ(offsetof(baseline_header_t, regime_ch_count), 48);
}

static void test_stat_offsets(void) {
    CHECK_EQ(offsetof(baseline_stat_t, count), 0);
    CHECK_EQ(offsetof(baseline_stat_t, mean), 4);
    CHECK_EQ(offsetof(baseline_stat_t, m2), 8);
    CHECK_EQ(offsetof(baseline_stat_t, min_val), 12);
    CHECK_EQ(offsetof(baseline_stat_t, max_val), 16);
    CHECK_EQ(offsetof(baseline_stat_t, first_ts_ns), 20);   /* unaligned on purpose, packed */
    CHECK_EQ(offsetof(baseline_stat_t, last_ts_ns), 28);
    CHECK_EQ(offsetof(baseline_stat_t, state), 36);
    CHECK_EQ(offsetof(baseline_stat_t, flags), 37);
    CHECK_EQ(offsetof(baseline_stat_t, _pad), 38);
}

static void test_channel_entry_offsets(void) {
    CHECK_EQ(offsetof(baseline_channel_entry_t, ch_id), 0);
    CHECK_EQ(offsetof(baseline_channel_entry_t, n_regimes_used), 2);
    CHECK_EQ(offsetof(baseline_channel_entry_t, _pad), 4);
}

/* ---- file layout math --------------------------------------------- */

static void test_file_layout_offsets(void) {
    CHECK_EQ(BASELINE_CHANNEL_TABLE_OFF, 64);
    CHECK_EQ(BASELINE_STATS_OFF, 64 + 256 * 8);
    CHECK_EQ(BASELINE_STATS_OFF, 2112);
    CHECK_EQ(BASELINE_STATS_SIZE, 256 * 16 * 40);
    CHECK_EQ(BASELINE_STATS_SIZE, 163840);
    CHECK_EQ(BASELINE_TRAILER_OFF, 2112 + 163840);
    CHECK_EQ(BASELINE_TRAILER_OFF, 165952);
    CHECK_EQ(BASELINE_FILE_SIZE, 165952 + 4);
    CHECK_EQ(BASELINE_FILE_SIZE, 165956);
}

/* ---- state enum --------------------------------------------------- */

static void test_state_enum(void) {
    CHECK_EQ(BASELINE_EMPTY, 0);
    CHECK_EQ(BASELINE_LEARNING, 1);
    CHECK_EQ(BASELINE_READY, 2);
    CHECK_EQ(BASELINE_STALE, 3);
}

/* ---- baseline_t size ---------------------------------------------- */

static void test_baseline_t_size_sanity(void) {
    /* The full model should be around 140 KB. If it grows past 200 KB,
     * something has been added without considering the static
     * allocation budget. */
    size_t s = sizeof(baseline_t);
    CHECK(s > 100000);      /* at least 100 KB */
    CHECK(s < 200000);      /* at most 200 KB */
}

int main(void) {
    test_constants();
    test_header_size();
    test_channel_entry_size();
    test_stat_size();
    test_trailer_size();
    test_header_offsets();
    test_stat_offsets();
    test_channel_entry_offsets();
    test_file_layout_offsets();
    test_state_enum();
    test_baseline_t_size_sanity();

    if (g_failures) {
        fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_baseline_format: all checks passed\n");
    return 0;
}