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

/* The test binary runs from build/ under CTest. From there:
 *   - the gardener binary is ./gardener.exe (or ./gardener)
 *   - the config is at ../configs/channels.toml
 * Paths use forward slashes on both platforms; cmd.exe accepts them. */

#if defined(_WIN32)
  #define GARDENER_BIN  "gardener.exe"
  #define CONFIG_PATH   "../configs/channels.toml"
#else
  #define GARDENER_BIN  "./gardener"
  #define CONFIG_PATH   "../configs/channels.toml"
#endif

/* Run gardener with the given args, reading from `stdin_file` and
 * writing stdout/stderr to NUL or /dev/null. Returns the exit code.
 * On failure, stderr is retried without redirection so the failure
 * is visible. */
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
        /* Re-run without redirection so the failure shows up in the
         * test log. */
        fprintf(stderr, "  re-running for diagnostics: %s\n", cmd);
#if defined(_WIN32)
        snprintf(cmd, sizeof(cmd),
                 "%s %s%s%s", GARDENER_BIN, args,
                 stdin_file ? " < " : "", stdin_file ? stdin_file : "");
#else
        snprintf(cmd, sizeof(cmd),
                 "%s %s%s%s", GARDENER_BIN, args,
                 stdin_file ? " < " : "", stdin_file ? stdin_file : "");
#endif
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
    char idx[280];
    size_t n = strlen(path);
    if (n >= 4 && strcmp(path + n - 4, ".bin") == 0) {
        snprintf(idx, sizeof(idx), "%.*s.idx", (int)(n - 4), path);
    } else {
        snprintf(idx, sizeof(idx), "%s.idx", path);
    }
    remove(idx);
}

static void write_jsonl(const char *path, const char *contents) {
    FILE *f = fopen(path, "w");
    if (!f) { perror("fopen"); exit(2); }
    fputs(contents, f);
    fclose(f);
}

/* ------------------------------------------------------------------ */

static void test_version_flag(void) {
    int rc = run_gardener("--version", NULL);
    CHECK_EQ(rc, 0);
}

static void test_no_log_flag_creates_no_file(void) {
    const char *input = "test_main_in.jsonl";
    const char *log = "test_main_out.bin";
    cleanup(log);
    write_jsonl(input, "{\"t\":1,\"v\":{\"1\":20.5}}\n");

    char args[256];
    snprintf(args, sizeof(args), "--channels %s", CONFIG_PATH);
    int rc = run_gardener(args, input);
    CHECK_EQ(rc, 0);
    CHECK(!file_exists(log));
    remove(input);
}

static void test_log_flag_creates_file(void) {
    const char *input = "test_main_in2.jsonl";
    const char *log = "test_main_out2.bin";
    cleanup(log);
    write_jsonl(input,
        "{\"t\":1000,\"v\":{\"1\":20.5}}\n"
        "{\"t\":2000,\"v\":{\"1\":21.0}}\n"
        "{\"t\":3000,\"v\":{\"1\":21.5}}\n");

    char args[512];
    snprintf(args, sizeof(args),
             "--channels %s --log %s --log-fresh", CONFIG_PATH, log);
    int rc = run_gardener(args, input);
    CHECK_EQ(rc, 0);
    CHECK(file_exists(log));

    log_reader_t *r = log_reader_open(log);
    CHECK(r != NULL);
    if (r) {
        CHECK_EQ(log_reader_total_records(r), 3);
        packet_t p;
        int count = 0;
        while (log_reader_next(r, &p) == 1) count++;
        CHECK_EQ(count, 3);
        log_reader_close(r);
    }

    remove(input);
    cleanup(log);
}

static void test_log_refuses_bad_max(void) {
    const char *input = "test_main_in3.jsonl";
    const char *log = "test_main_bad_max.bin";
    cleanup(log);
    write_jsonl(input, "{\"t\":1,\"v\":{\"1\":20.5}}\n");

    char args[512];
    snprintf(args, sizeof(args),
             "--channels %s --log %s --log-max 1000 --log-fresh",
             CONFIG_PATH, log);
    int rc = run_gardener(args, input);
    CHECK_EQ(rc, 0);
    CHECK(!file_exists(log));
    remove(input);
    cleanup(log);
}

static void test_replay_with_log(void) {
    const char *input = "test_main_in4.jsonl";
    const char *log1 = "test_main_r1.bin";
    const char *log2 = "test_main_r2.bin";
    cleanup(log1);
    cleanup(log2);

    write_jsonl(input,
        "{\"t\":1000,\"v\":{\"1\":1.0}}\n"
        "{\"t\":2000,\"v\":{\"1\":2.0}}\n"
        "{\"t\":3000,\"v\":{\"1\":3.0}}\n");

    /* Session 1: write log1 from JSONL. */
    char args1[512];
    snprintf(args1, sizeof(args1),
             "--channels %s --log %s --log-fresh", CONFIG_PATH, log1);
    CHECK_EQ(run_gardener(args1, input), 0);
    CHECK(file_exists(log1));

    /* Session 2: replay log1 into log2. */
    char args2[512];
    snprintf(args2, sizeof(args2),
             "--channels %s --replay %s --log %s --log-fresh",
             CONFIG_PATH, log1, log2);
    CHECK_EQ(run_gardener(args2, NULL), 0);
    CHECK(file_exists(log2));

    log_reader_t *r1 = log_reader_open(log1);
    log_reader_t *r2 = log_reader_open(log2);
    CHECK(r1 != NULL);
    CHECK(r2 != NULL);
    if (r1 && r2) {
        CHECK_EQ(log_reader_total_records(r1), 3);
        CHECK_EQ(log_reader_total_records(r2), 3);

        packet_t p1, p2;
        int n1 = log_reader_next(r1, &p1);
        int n2 = log_reader_next(r2, &p2);
        CHECK_EQ(n1, 1);
        CHECK_EQ(n2, 1);
        CHECK_EQ(p1.timestamp_ns, p2.timestamp_ns);
    }
    if (r1) log_reader_close(r1);
    if (r2) log_reader_close(r2);

    remove(input);
    cleanup(log1);
    cleanup(log2);
}

int main(void) {
    plat_log_set_level(LOG_ERROR);

    test_version_flag();
    test_no_log_flag_creates_no_file();
    test_log_flag_creates_file();
    test_log_refuses_bad_max();
    test_replay_with_log();

    if (g_failures) {
        fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_main_args: all checks passed\n");
    return 0;
}