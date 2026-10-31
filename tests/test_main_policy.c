#include "gardener/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond) do {                                              \
    if (!(cond)) {                                                    \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
        g_failures++;                                                 \
    }                                                                 \
} while (0)

#if defined(_WIN32)
  #define GARDENER_BIN  "gardener.exe"
  #define CONFIG_PATH   "../configs/channels.toml"
#else
  #define GARDENER_BIN  "./gardener"
  #define CONFIG_PATH   "../configs/channels.toml"
#endif

static void write_file(const char *path, const char *contents) {
    FILE *f = fopen(path, "w");
    if (!f) { perror("fopen"); exit(2); }
    fputs(contents, f);
    fclose(f);
}

static int run_gardener(const char *args, const char *stdin_file) {
    char cmd[1024];
#if defined(_WIN32)
    if (stdin_file) snprintf(cmd, sizeof(cmd), "%s %s < %s > NUL 2> NUL", GARDENER_BIN, args, stdin_file);
    else            snprintf(cmd, sizeof(cmd), "%s %s > NUL 2> NUL", GARDENER_BIN, args);
#else
    if (stdin_file) snprintf(cmd, sizeof(cmd), "%s %s < %s >/dev/null 2>&1", GARDENER_BIN, args, stdin_file);
    else            snprintf(cmd, sizeof(cmd), "%s %s >/dev/null 2>&1", GARDENER_BIN, args);
#endif
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "  re-running for diagnostics: %s\n", cmd);
        snprintf(cmd, sizeof(cmd),
                 "%s %s%s%s", GARDENER_BIN, args,
                 stdin_file ? " < " : "", stdin_file ? stdin_file : "");
        system(cmd);
    }
    return rc;
}

static void test_end_to_end_policy(void) {
    const char *in = "test_pol_in.jsonl";
    const char *ev = "test_pol_ev.jsonl";
    const char *cfg = "test_pol_policy.toml";
    const char *log = "test_pol_action.log";
    
    remove(ev);
    remove(log);
    
    /* Feed a range violation (coolant_temp > 150) */
    write_file(in,
        "{\"t\":1000,\"v\":{\"1\":200.0,\"30\":true}}\n"
        "{\"t\":2000,\"v\":{\"1\":50.0,\"30\":true}}\n");
        
    /* Create a policy that matches range violations and logs them */
    write_file(cfg,
        "[[rule]]\n"
        "name = \"range_logger\"\n"
        "detector = \"range\"\n"
        "state = \"started\"\n"
        "[[rule.action]]\n"
        "type = \"log\"\n"
        "target = \"test_pol_action.log\"\n");

    char args[512];
    snprintf(args, sizeof(args),
             "--channels %s --detect --events %s --policy %s",
             CONFIG_PATH, ev, cfg);

    /* Run gardener */
    CHECK(run_gardener(args, in) == 0);

    /* Verify the action log was created and contains the event */
    FILE *f = fopen(log, "r");
    CHECK(f != NULL);
    if (f) {
        char line[256];
        int found = 0;
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "det=range")) found = 1;
        }
        fclose(f);
        CHECK(found);
    }

    remove(in);
    remove(ev);
    remove(cfg);
    remove(log);
}

int main(void) {
    plat_log_set_level(LOG_ERROR);
    test_end_to_end_policy();

    if (g_failures) {
        fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_main_policy: all checks passed\n");
    return 0;
}