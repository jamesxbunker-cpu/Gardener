#include "gardener/dashboard.h"
#include "gardener/platform.h"
#include <stdio.h>
#include <string.h>

/* Since we can't easily test sockets in a pure unit test without 
   heavy mocking, this test just verifies init/close don't crash on 
   a random high port. The real test is running the Python dashboard. */

static int g_failures = 0;

#define CHECK(cond) do {                                              \
    if (!(cond)) {                                                    \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
        g_failures++;                                                 \
    }                                                                 \
} while (0)

static void test_init_close(void) {
    static dashboard_server_t dash;
    /* Use port 0 to let the OS assign a free port, though we don't
       actually need to test the bind logic deeply here. */
    CHECK(dashboard_init(&dash, 0) == 0);
    dashboard_close(&dash);
}

int main(void) {
    plat_log_set_level(LOG_ERROR);
    test_init_close();

    if (g_failures) {
        fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_dashboard: all checks passed\n");
    return 0;
}