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

/* Helper to check if a file contains a string */
static int file_contains(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, needle)) { found = 1; break; }
    }
    fclose(f);
    return found;
}

static void test_action_log(void) {
    const char *log_path = "test_action.log";
    remove(log_path);

    policy_rule_t rule;
    memset(&rule, 0, sizeof(rule));
    snprintf(rule.name, sizeof(rule.name), "test_log");
    rule.action_count = 1;
    rule.actions[0].type = ACTION_LOG;
    snprintf(rule.actions[0].target, sizeof(rule.actions[0].target), "%s", log_path);

    event_t e;
    memset(&e, 0, sizeof(e));
    e.detector = DETECTOR_ZSCORE;
    e.channel_id = 42;
    e.severity = SEVERITY_CRITICAL;
    e.state = EVENT_STATE_STARTED;
    event_set_timestamp(&e, 123456789);

    // Execute
    CHECK_EQ(policy_execute(&rule, &e), 0);

    // Verify file contents
    CHECK(file_contains(log_path, "t=123456789"));
    CHECK(file_contains(log_path, "det=zscore"));
    CHECK(file_contains(log_path, "ch=42"));
    CHECK(file_contains(log_path, "state=started"));

    remove(log_path);
}

static void test_action_webhook_success(void) {
    /* Use a safe, portable command that always succeeds */
#if defined(_WIN32)
    const char *safe_cmd = "cmd /c exit 0";
#else
    const char *safe_cmd = "true";
#endif

    policy_rule_t rule;
    memset(&rule, 0, sizeof(rule));
    rule.action_count = 1;
    rule.actions[0].type = ACTION_WEBHOOK;
    snprintf(rule.actions[0].target, sizeof(rule.actions[0].target), "%s", safe_cmd);

    event_t e;
    memset(&e, 0, sizeof(e));

    CHECK_EQ(policy_execute(&rule, &e), 0);
}

static void test_action_webhook_failure(void) {
    /* Use a command guaranteed to fail */
#if defined(_WIN32)
    const char *bad_cmd = "cmd /c exit 1";
#else
    const char *bad_cmd = "false";
#endif

    policy_rule_t rule;
    memset(&rule, 0, sizeof(rule));
    rule.action_count = 1;
    rule.actions[0].type = ACTION_WEBHOOK;
    snprintf(rule.actions[0].target, sizeof(rule.actions[0].target), "%s", bad_cmd);

    event_t e;
    memset(&e, 0, sizeof(e));

    /* Should return -1 because the action failed */
    CHECK_EQ(policy_execute(&rule, &e), -1);
}

static void test_action_missing_target(void) {
    policy_rule_t rule;
    memset(&rule, 0, sizeof(rule));
    rule.action_count = 1;
    rule.actions[0].type = ACTION_LOG;
    /* target is empty */

    event_t e;
    memset(&e, 0, sizeof(e));

    CHECK_EQ(policy_execute(&rule, &e), -1);
}

int main(void) {
    plat_log_set_level(LOG_ERROR); // Suppress expected warnings
    test_action_log();
    test_action_webhook_success();
    test_action_webhook_failure();
    test_action_missing_target();

    if (g_failures) {
        fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_policy_actions: all checks passed\n");
    return 0;
}