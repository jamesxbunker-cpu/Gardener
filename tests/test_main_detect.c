#include "gardener/log_reader.h"
#include "gardener/log.h"
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

#define CHECK_EQ(a, b) do {                                           \
    long long _a = (long long)(a), _b = (long long)(b);               \
    if (_a != _b) {                                                   \
        fprintf(stderr, "FAIL %s:%d: %s (%lld) != %s (%lld)\n",       \
                __FILE__, __LINE__, #a, _a, #b, _b);                  \
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

static int run_gardener(const char *args, const char *stdin_file) {
    char cmd[1024];
#if defined(_WIN32)
    if (stdin_file) {
        snprintf(cmd, sizeof(cmd),
                 "%s %s < %s > NUL 2> NUL", GARDENER_BIN, args, stdin_file);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "%s %s > NUL 2> NUL", GARDENER_BIN, args);
    }
#else
    if (stdin_file) {
        snprintf(cmd, sizeof(cmd),
                 "%s %s < %s >/dev/null 2>&1", GARDENER_BIN, args, stdin_file);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "%s %s >/dev/null 2>&1", GARDENER_BIN, args);
    }
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

static int file_exists(const char *path) {
    plat_file_t *f = plat_file_open(path, PLAT_FILE_READ);
    if (!f) return 0;
    plat_file_close(f);
    return 1;
}

static void cleanup(const char *path) {
    remove(path);
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    remove(tmp);
}

static void write_jsonl(const char *path, const char *contents) {
    FILE *f = fopen(path, "w");
    if (!f) { perror("fopen"); exit(2); }
    fputs(contents, f);
    fclose(f);
}

static int count_lines(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int lines = 0, c;
    while ((c = fgetc(f)) != EOF) if (c == '\n') lines++;
    fclose(f);
    return lines;
}

/* ------------------------------------------------------------------ */

static void test_no_detect_flag_no_events(void) {
    const char *in = "test_md_in1.jsonl";
    const char *ev = "test_md_ev1.jsonl";
    cleanup(ev);
    write_jsonl(in, "{\"t\":1,\"v\":{\"1\":20.5}}\n");

    char args[256];
    snprintf(args, sizeof(args), "--channels %s", CONFIG_PATH);
    CHECK_EQ(run_gardener(args, in), 0);
    /* No --events, no --detect: nothing should be created. */
    CHECK(!file_exists(ev));

    remove(in);
}

static void test_detect_with_events_flag_creates_file(void) {
    const char *in = "test_md_in2.jsonl";
    const char *ev = "test_md_ev2.jsonl";
    cleanup(ev);
    write_jsonl(in, "{\"t\":1,\"v\":{\"1\":20.5}}\n");

    char args[512];
    snprintf(args, sizeof(args),
             "--channels %s --detect --events %s", CONFIG_PATH, ev);
    CHECK_EQ(run_gardener(args, in), 0);
    /* File opened at startup, closed at exit. It exists. */
    CHECK(file_exists(ev));

    remove(in);
    cleanup(ev);
}

static void test_range_violation_emits_event(void) {
    const char *in = "test_md_in3.jsonl";
    const char *ev = "test_md_ev3.jsonl";
    cleanup(ev);
    /* coolant_temp has min=0 max=150. Feed 200 (out of range). */
    write_jsonl(in,
        "{\"t\":1000,\"v\":{\"1\":200.0,\"30\":true}}\n"
        "{\"t\":2000,\"v\":{\"1\":50.0,\"30\":true}}\n");

    char args[512];
    snprintf(args, sizeof(args),
             "--channels %s --detect --events %s", CONFIG_PATH, ev);
    CHECK_EQ(run_gardener(args, in), 0);
    CHECK(file_exists(ev));

    /* Should have 2 events: STARTED at t=1, CLEARED at t=2. */
    int lines = count_lines(ev);
    CHECK_EQ(lines, 2);

    remove(in);
    cleanup(ev);
}

static void test_detect_pairs_accepted(void) {
    /* --detect-pair 1,2 should not error. It won't fire (no correlation
     * built up in 3 packets), but it should be accepted. */
    const char *in = "test_md_in4.jsonl";
    const char *ev = "test_md_ev4.jsonl";
    cleanup(ev);
    write_jsonl(in,
        "{\"t\":1,\"v\":{\"1\":10.0,\"2\":10.0}}\n"
        "{\"t\":2,\"v\":{\"1\":20.0,\"2\":20.0}}\n"
        "{\"t\":3,\"v\":{\"1\":30.0,\"2\":30.0}}\n");

    char args[512];
    snprintf(args, sizeof(args),
             "--channels %s --detect --events %s --detect-pair 1,2",
             CONFIG_PATH, ev);
    CHECK_EQ(run_gardener(args, in), 0);
    CHECK(file_exists(ev));

    remove(in);
    cleanup(ev);
}

static void test_detect_mismatch_accepted(void) {
    const char *in = "test_md_in5.jsonl";
    const char *ev = "test_md_ev5.jsonl";
    cleanup(ev);
    write_jsonl(in,
        "{\"t\":1,\"v\":{\"11\":50.0,\"20\":50.0}}\n"
        "{\"t\":2,\"v\":{\"11\":50.0,\"20\":50.0}}\n");

    char args[512];
    snprintf(args, sizeof(args),
             "--channels %s --detect --events %s --detect-mismatch 11,20",
             CONFIG_PATH, ev);
    CHECK_EQ(run_gardener(args, in), 0);
    CHECK(file_exists(ev));

    remove(in);
    cleanup(ev);
}

static void test_bad_pair_syntax_rejected(void) {
    const char *in = "test_md_in6.jsonl";
    cleanup(in);
    write_jsonl(in, "{\"t\":1,\"v\":{\"1\":1.0}}\n");

    char args[256];
    snprintf(args, sizeof(args),
             "--channels %s --detect --detect-pair nocomma",
             CONFIG_PATH);
    int rc = run_gardener(args, in);
    /* Usage error should return 2. On Windows, system() returns 2
     * directly. On POSIX it returns the exit code shifted left 8,
     * but system() on POSIX returns the wait status; the shell
     * return would be 2. CTest sees 2. For robustness, accept
     * any nonzero. */
    CHECK(rc != 0);

    remove(in);
}

int main(void) {
    plat_log_set_level(LOG_ERROR);

    test_no_detect_flag_no_events();
    test_detect_with_events_flag_creates_file();
    test_range_violation_emits_event();
    test_detect_pairs_accepted();
    test_detect_mismatch_accepted();
    test_bad_pair_syntax_rejected();

    if (g_failures) {
        fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_main_detect: all checks passed\n");
    return 0;
}