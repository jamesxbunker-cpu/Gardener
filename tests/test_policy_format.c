#include "gardener/policy.h"
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

#define CHECK_EQ(a, b) do {                                           \
    long long _a = (long long)(a), _b = (long long)(b);               \
    if (_a != _b) {                                                   \
        fprintf(stderr, "FAIL %s:%d: %s (%lld) != %s (%lld)\n",       \
                __FILE__, __LINE__, #a, _a, #b, _b);                  \
        g_failures++;                                                 \
    }                                                                 \
} while (0)

static void test_constants(void) {
    CHECK_EQ(POLICY_MAX_RULES, 32);
    CHECK_EQ(POLICY_MAX_ACTIONS, 8);
    CHECK_EQ(POLICY_NAME_MAX, 32);
}

static void test_action_enum(void) {
    CHECK_EQ(ACTION_NOTIFY, 0);
    CHECK_EQ(ACTION_LOG, 1);
    CHECK_EQ(ACTION_WEBHOOK, 2);
}

static void test_engine_init(void) {
    static policy_engine_t pe;
    memset(&pe, 0xFF, sizeof(pe)); // Fill with garbage
    
    CHECK_EQ(policy_init(&pe), 0);
    CHECK_EQ(pe.rule_count, 0);
    // Verify first rule is zeroed out
    CHECK_EQ(pe.rules[0].name[0], 0);
}

int main(void) {
    plat_log_set_level(LOG_ERROR);

    test_constants();
    test_action_enum();
    test_engine_init();

    if (g_failures) {
        fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_policy_format: all checks passed\n");
    return 0;
}