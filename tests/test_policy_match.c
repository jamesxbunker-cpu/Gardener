#include "gardener/policy.h"
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

static void write_cfg(const char *path, const char *contents) {
    FILE *f = fopen(path, "w");
    if (!f) { perror("fopen"); exit(2); }
    fputs(contents, f);
    fclose(f);
}

static event_t mk_event(detector_t d, severity_t s, event_state_t st) {
    event_t e;
    memset(&e, 0, sizeof(e));
    e.detector = d;
    e.severity = s;
    e.state = st;
    return e;
}

static void test_load_and_match(void) {
    const char *path = "test_policy1.toml";
    write_cfg(path,
        "[[rule]]\n"
        "name = \"test_rule\"\n"
        "detector = \"zscore\"\n"
        "severity = \"critical\"\n"
        "state = \"started\"\n"
        "cooldown = \"10s\"\n"
        "[[rule.action]]\n"
        "type = \"notify\"\n"
        "target = \"test message\"\n");

    static policy_engine_t pe;
    CHECK_EQ(policy_load_toml(&pe, path), 0);
    CHECK_EQ(pe.rule_count, 1);
    CHECK(strcmp(pe.rules[0].name, "test_rule") == 0);
    CHECK_EQ(pe.rules[0].match_detector, DETECTOR_ZSCORE);
    CHECK_EQ(pe.rules[0].match_severity, SEVERITY_CRITICAL);
    CHECK_EQ(pe.rules[0].match_state, EVENT_STATE_STARTED);
    CHECK_EQ(pe.rules[0].cooldown_sec, 10);
    CHECK_EQ(pe.rules[0].action_count, 1);
    CHECK_EQ(pe.rules[0].actions[0].type, ACTION_NOTIFY);
    CHECK(strcmp(pe.rules[0].actions[0].target, "test message") == 0);

    const policy_rule_t *fired[4];

    // 1. Exact match
    event_t e1 = mk_event(DETECTOR_ZSCORE, SEVERITY_CRITICAL, EVENT_STATE_STARTED);
    CHECK_EQ(policy_evaluate(&pe, &e1, 1000000000ull, fired, 4), 1);
    CHECK(fired[0] == &pe.rules[0]);

    // 2. Same event, but within cooldown (5s later)
    CHECK_EQ(policy_evaluate(&pe, &e1, 6000000000ull, fired, 4), 0);

    // 3. Same event, after cooldown (11s later)
    CHECK_EQ(policy_evaluate(&pe, &e1, 12000000000ull, fired, 4), 1);

    // 4. Wrong detector
    event_t e2 = mk_event(DETECTOR_RANGE, SEVERITY_CRITICAL, EVENT_STATE_STARTED);
    CHECK_EQ(policy_evaluate(&pe, &e2, 13000000000ull, fired, 4), 0);

    // 5. Wrong state
    event_t e3 = mk_event(DETECTOR_ZSCORE, SEVERITY_CRITICAL, EVENT_STATE_CLEARED);
    CHECK_EQ(policy_evaluate(&pe, &e3, 14000000000ull, fired, 4), 0);

    remove(path);
}

static void test_wildcard_match(void) {
    const char *path = "test_policy2.toml";
    write_cfg(path,
        "[[rule]]\n"
        "name = \"any_critical\"\n"
        "severity = \"critical\"\n"
        "state = \"started\"\n");

    static policy_engine_t pe;
    policy_load_toml(&pe, path);
    CHECK_EQ(pe.rule_count, 1);
    
    // Detector is wildcard (255)
    CHECK_EQ(pe.rules[0].match_detector, (detector_t)255);

    const policy_rule_t *fired[4];

    // Any detector fires it, as long as it's critical/started
    event_t e1 = mk_event(DETECTOR_RANGE, SEVERITY_CRITICAL, EVENT_STATE_STARTED);
    CHECK_EQ(policy_evaluate(&pe, &e1, 1000, fired, 4), 1);

    event_t e2 = mk_event(DETECTOR_ROC, SEVERITY_CRITICAL, EVENT_STATE_STARTED);
    CHECK_EQ(policy_evaluate(&pe, &e2, 2000, fired, 4), 1);

    // Wrong severity
    event_t e3 = mk_event(DETECTOR_ROC, SEVERITY_WARN, EVENT_STATE_STARTED);
    CHECK_EQ(policy_evaluate(&pe, &e3, 3000, fired, 4), 0);

    remove(path);
}

int main(void) {
    plat_log_set_level(LOG_ERROR);
    test_load_and_match();
    test_wildcard_match();

    if (g_failures) {
        fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_policy_match: all checks passed\n");
    return 0;
}