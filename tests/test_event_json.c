#include "gardener/event.h"
#include "gardener/platform.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond) do {                                              \
    if (!(cond)) {                                                    \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
        g_failures++;                                                 \
    }                                                                 \
} while (0)

static void test_basic_encoding(void) {
    event_t e;
    memset(&e, 0, sizeof(e));
    event_set_timestamp(&e, 1000);
    event_set_source_ts(&e, 900);
    e.channel_id = 1;
    e.channel_id2 = EVENT_CHANNEL_NONE;
    e.regime = 3;
    e.observed = 20.5f;
    e.baseline_mean = 15.0f;
    e.baseline_stddev = 1.0f;
    e.threshold = 5.0f;
    e.detector = DETECTOR_ZSCORE;
    e.severity = SEVERITY_WARN;
    e.state = EVENT_STATE_STARTED;
    e.version = EVENT_VERSION;

    char buf[512];
    int n = event_to_json(&e, "test message", buf, sizeof(buf));
    CHECK(n > 0);
    CHECK(strstr(buf, "\"t\":1000") != NULL);
    CHECK(strstr(buf, "\"src_t\":900") != NULL);
    CHECK(strstr(buf, "\"ch\":1") != NULL);
    CHECK(strstr(buf, "\"detector\":\"zscore\"") != NULL);
    CHECK(strstr(buf, "\"severity\":\"warn\"") != NULL);
    CHECK(strstr(buf, "\"state\":\"started\"") != NULL);
    CHECK(strstr(buf, "\"msg\":\"test message\"") != NULL);
    CHECK(strstr(buf, "\"observed\":20.5") != NULL);
    CHECK(strstr(buf, "\"mean\":15") != NULL);
    CHECK(strstr(buf, "\"stddev\":1") != NULL);
    CHECK(strstr(buf, "\"threshold\":5") != NULL);
}

static void test_none_sentinels(void) {
    event_t e;
    memset(&e, 0, sizeof(e));
    e.channel_id = EVENT_CHANNEL_NONE;
    e.channel_id2 = EVENT_CHANNEL_NONE;
    e.regime = EVENT_REGIME_NONE;

    char buf[512];
    int n = event_to_json(&e, "test", buf, sizeof(buf));
    CHECK(n > 0);
    CHECK(strstr(buf, "\"ch\":null") != NULL);
    CHECK(strstr(buf, "\"ch2\":null") != NULL);
    CHECK(strstr(buf, "\"regime\":null") != NULL);
}

static void test_msg_escaping(void) {
    event_t e;
    memset(&e, 0, sizeof(e));
    e.channel_id = 1;
    e.regime = 0;
    e.detector = DETECTOR_RANGE;
    e.severity = SEVERITY_WARN;
    e.state = EVENT_STATE_STARTED;

    char buf[512];
    int n = event_to_json(&e, "a \"quoted\" word", buf, sizeof(buf));
    CHECK(n > 0);
    CHECK(strstr(buf, "a \\\"quoted\\\" word") != NULL);
}

static void test_null_msg(void) {
    event_t e;
    memset(&e, 0, sizeof(e));
    e.channel_id = 1;
    e.regime = 0;

    char buf[512];
    int n = event_to_json(&e, NULL, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK(strstr(buf, "\"msg\":\"\"") != NULL);
}

static void test_null_args(void) {
    event_t e;
    memset(&e, 0, sizeof(e));
    char buf[512];
    CHECK(event_to_json(NULL, "x", buf, sizeof(buf)) == -1);
    CHECK(event_to_json(&e, "x", NULL, 0) == -1);
}

int main(void) {
    plat_log_set_level(LOG_ERROR);

    test_basic_encoding();
    test_none_sentinels();
    test_msg_escaping();
    test_null_msg();
    test_null_args();

    if (g_failures) {
        fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_event_json: all checks passed\n");
    return 0;
}