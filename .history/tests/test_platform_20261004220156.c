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

#define CHECK_GE(a, b) do {                                           \
    long long _a = (long long)(a), _b = (long long)(b);               \
    if (_a < _b) {                                                    \
        fprintf(stderr, "FAIL %s:%d: %s (%lld) < %s (%lld)\n",        \
                __FILE__, __LINE__, #a, _a, #b, _b);                  \
        g_failures++;                                                 \
    }                                                                 \
} while (0)

static void test_monotonic_time_moves_forward(void) {
    uint64_t t0 = plat_now_ns();
    plat_sleep_ms(20);
    uint64_t t1 = plat_now_ns();

    /* Sanity: t1 should be strictly after t0. On any supported platform
     * 20 ms is enough to guarantee at least some advancement. */
    CHECK(t1 > t0);
    /* And the gap should be at least ~15 ms (allow slack for scheduler
     * jitter and coarse timer resolution). */
    CHECK_GE(t1 - t0, 15000000ull);
    /* And not absurd: less than 10 s for a 20 ms sleep. */
    CHECK(t1 - t0 < 10000000000ull);
}

static void test_wall_time_is_plausible(void) {
    /* Anything after 2020-01-01 in nanoseconds is > 1.5e18. If the wall
     * clock is unset, this returns 0 and we should know about it. */
    uint64_t w = plat_wall_ns();
    CHECK_GE(w, 1500000000000000000ull);
}

static void test_sleep_returns(void) {
    uint64_t t0 = plat_now_ns();
    plat_sleep_ms(30);
    uint64_t t1 = plat_now_ns();
    CHECK_GE(t1 - t0, 25000000ull);
}

static void test_file_write_read_roundtrip(void) {
    const char *path = "test_platform_tmp.bin";
    const char payload[] = "gardener-platform-test";

    plat_file_t *w = plat_file_open(path, 1);
    CHECK(w != NULL);
    if (w) {
        int n = plat_file_write(w, payload, sizeof(payload));
        CHECK(n == (int)sizeof(payload));
        CHECK(plat_file_sync(w) == 0);
        plat_file_close(w);
    }

    plat_file_t *r = plat_file_open(path, 0);
    CHECK(r != NULL);
    if (r) {
        char buf[64];
        memset(buf, 0, sizeof(buf));
        int n = plat_file_read(r, buf, sizeof(buf));
        CHECK(n == (int)sizeof(payload));
        CHECK(strcmp(buf, payload) == 0);
        /* Next read should be EOF. */
        int n2 = plat_file_read(r, buf, sizeof(buf));
        CHECK(n2 == 0);
        plat_file_close(r);
    }

    remove(path);
}

static void test_open_missing_file_returns_null(void) {
    plat_file_t *f = plat_file_open("/nonexistent/path/xyz", 0);
    CHECK(f == NULL);
}

static void test_file_null_safety(void) {
    CHECK(plat_file_read(NULL, NULL, 0) == -1);
    CHECK(plat_file_write(NULL, NULL, 0) == -1);
    CHECK(plat_file_sync(NULL) == -1);
    plat_file_close(NULL);  /* must not crash */
}

static void test_log_does_not_crash(void) {
    /* Level filtering and formatting. We don't capture stderr, we just
     * make sure the calls return and don't segfault on bad inputs. */
    plat_log_set_level(LOG_DEBUG);
    plat_log(LOG_DEBUG, "debug %d", 1);
    plat_log(LOG_INFO,  "info %s", "ok");
    plat_log(LOG_WARN,  "warn %g", 3.14);
    plat_log(LOG_ERROR, "error %p", (void *)0);

    plat_log_set_level(LOG_ERROR);
    plat_log(LOG_DEBUG, "should be suppressed");
    plat_log(LOG_INFO,  "should be suppressed too");

    plat_log_set_level(LOG_INFO);
    plat_log(LOG_INFO, "restored");
}

int main(void) {
    test_monotonic_time_moves_forward();
    test_wall_time_is_plausible();
    test_sleep_returns();
    test_file_write_read_roundtrip();
    test_open_missing_file_returns_null();
    test_file_null_safety();
    test_log_does_not_crash();

    if (g_failures) {
        fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_platform: all checks passed\n");
    return 0;
}