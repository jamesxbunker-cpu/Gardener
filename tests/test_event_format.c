#include "gardener/event.h"

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
    CHECK_EQ(EVENT_MAGIC, 0x45564E54u);   /* "EVNT" */
    CHECK_EQ(EVENT_VERSION, 1);
    CHECK_EQ(EVENT_SIZE, 48);
    CHECK_EQ(EVENT_CHANNEL_NONE, 0xFFFF);
    CHECK_EQ(EVENT_REGIME_NONE, 0xFFFF);
    CHECK_EQ(EVENT_MSG_MAX, 128);
}

/* ---- struct size and offsets -------------------------------------- */

static void test_event_size(void) {
    CHECK_EQ(sizeof(event_t), EVENT_SIZE);
    CHECK_EQ(sizeof(event_t), 48);
}

static void test_event_offsets(void) {
    CHECK_EQ(offsetof(event_t, timestamp_ns_lo),    0);
    CHECK_EQ(offsetof(event_t, timestamp_ns_hi),    4);
    CHECK_EQ(offsetof(event_t, source_ts_ns_lo),    8);
    CHECK_EQ(offsetof(event_t, source_ts_ns_hi),    12);
    CHECK_EQ(offsetof(event_t, channel_id),         16);
    CHECK_EQ(offsetof(event_t, channel_id2),        18);
    CHECK_EQ(offsetof(event_t, regime),             20);
    CHECK_EQ(offsetof(event_t, _pad),               22);
    CHECK_EQ(offsetof(event_t, observed),           24);
    CHECK_EQ(offsetof(event_t, baseline_mean),      28);
    CHECK_EQ(offsetof(event_t, baseline_stddev),    32);
    CHECK_EQ(offsetof(event_t, threshold),          36);
    CHECK_EQ(offsetof(event_t, detector),           40);
    CHECK_EQ(offsetof(event_t, severity),           41);
    CHECK_EQ(offsetof(event_t, state),              42);
    CHECK_EQ(offsetof(event_t, version),            43);
    CHECK_EQ(offsetof(event_t, crc),                44);
}

/* ---- enum values -------------------------------------------------- */

static void test_detector_enum(void) {
    CHECK_EQ(DETECTOR_UNKNOWN,     0);
    CHECK_EQ(DETECTOR_RANGE,       1);
    CHECK_EQ(DETECTOR_ZSCORE,      2);
    CHECK_EQ(DETECTOR_FLATLINE,    3);
    CHECK_EQ(DETECTOR_ROC,         4);
    CHECK_EQ(DETECTOR_CORRELATION, 5);
    CHECK_EQ(DETECTOR_MISMATCH,    6);
}

static void test_severity_enum(void) {
    CHECK_EQ(SEVERITY_INFO,     0);
    CHECK_EQ(SEVERITY_WARN,     1);
    CHECK_EQ(SEVERITY_CRITICAL, 2);
}

static void test_state_enum(void) {
    CHECK_EQ(EVENT_STATE_STARTED, 0);
    CHECK_EQ(EVENT_STATE_CLEARED, 1);
}

/* ---- naming helpers ----------------------------------------------- */

static void test_detector_names(void) {
    CHECK(strcmp(detector_name(DETECTOR_UNKNOWN),     "unknown")     == 0);
    CHECK(strcmp(detector_name(DETECTOR_RANGE),       "range")       == 0);
    CHECK(strcmp(detector_name(DETECTOR_ZSCORE),      "zscore")      == 0);
    CHECK(strcmp(detector_name(DETECTOR_FLATLINE),    "flatline")    == 0);
    CHECK(strcmp(detector_name(DETECTOR_ROC),         "roc")         == 0);
    CHECK(strcmp(detector_name(DETECTOR_CORRELATION), "correlation") == 0);
    CHECK(strcmp(detector_name(DETECTOR_MISMATCH),    "mismatch")    == 0);
    CHECK(strcmp(detector_name((detector_t)99),       "?")           == 0);
}

static void test_severity_names(void) {
    CHECK(strcmp(severity_name(SEVERITY_INFO),     "info")     == 0);
    CHECK(strcmp(severity_name(SEVERITY_WARN),     "warn")     == 0);
    CHECK(strcmp(severity_name(SEVERITY_CRITICAL), "critical") == 0);
    CHECK(strcmp(severity_name((severity_t)99),    "?")        == 0);
}

static void test_state_names(void) {
    CHECK(strcmp(event_state_name(EVENT_STATE_STARTED), "started") == 0);
    CHECK(strcmp(event_state_name(EVENT_STATE_CLEARED), "cleared") == 0);
    CHECK(strcmp(event_state_name((event_state_t)99),   "?")       == 0);
}

/* ---- timestamp helpers -------------------------------------------- */

static void test_timestamp_roundtrip(void) {
    event_t e;
    memset(&e, 0, sizeof(e));

    event_set_timestamp(&e, 1234567890123456789ull);
    CHECK_EQ(event_get_timestamp(&e), 1234567890123456789ull);

    event_set_source_ts(&e, 9876543210ull);
    CHECK_EQ(event_get_source_ts(&e), 9876543210ull);

    /* Verify the lo/hi split. */
    CHECK_EQ(e.timestamp_ns_lo, (uint32_t)(1234567890123456789ull & 0xFFFFFFFFu));
    CHECK_EQ(e.timestamp_ns_hi, (uint32_t)(1234567890123456789ull >> 32));
}

static void test_timestamp_zero(void) {
    event_t e;
    memset(&e, 0, sizeof(e));

    event_set_timestamp(&e, 0);
    CHECK_EQ(event_get_timestamp(&e), 0);
}

/* ---- NULL safety -------------------------------------------------- */

static void test_helpers_null_safe(void) {
    /* These are documented to handle NULL. Not implemented yet, but
     * the header declares them, so we can check the current behavior
     * is "don't crash". Once D1 implements them, add specific asserts. */
    event_set_timestamp(NULL, 0);   /* must not crash */
    event_set_source_ts(NULL, 0);   /* must not crash */
    CHECK_EQ(event_get_timestamp(NULL), 0);
    CHECK_EQ(event_get_source_ts(NULL), 0);
}

int main(void) {
    test_constants();
    test_event_size();
    test_event_offsets();
    test_detector_enum();
    test_severity_enum();
    test_state_enum();
    test_detector_names();
    test_severity_names();
    test_state_names();
    test_timestamp_roundtrip();
    test_timestamp_zero();
    test_helpers_null_safe();

    if (g_failures) {
        fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_event_format: all checks passed\n");
    return 0;
}