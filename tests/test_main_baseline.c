#include "gardener/baseline.h"
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

/* Find the first non-empty regime slot for a channel. Returns NULL if
 * the channel has no data. */
static const baseline_stat_t *first_regime(const baseline_t *b,
                                           uint16_t ch_id) {
    for (uint16_t r = 0; r < BASELINE_MAX_REGIMES; r++) {
        const baseline_stat_t *s = baseline_get(b, ch_id, r);
        if (s) return s;
    }
    return NULL;
}

/* Dump the loaded baseline for debugging. Prints channel id, regime
 * index, state, count, mean for every non-empty slot. */
static void dump_baseline(const baseline_t *b) {
    for (uint16_t i = 0; i < BASELINE_MAX_CHANNELS; i++) {
        uint16_t ch_id = b->channels[i].ch_id;
        if (ch_id == 0) continue;
        for (uint16_t r = 0; r < BASELINE_MAX_REGIMES; r++) {
            const baseline_stat_t *s = &b->stats[i][r];
            if (s->state == BASELINE_EMPTY) continue;
            fprintf(stderr, "  DEBUG: ch=%u regime=%u state=%u "
                            "count=%g mean=%g\n",
                    (unsigned)ch_id, (unsigned)r, (unsigned)s->state,
                    (double)s->count, (double)s->mean);
        }
    }
}

/* ------------------------------------------------------------------ */

static void test_baseline_no_flag_no_file(void) {
    const char *input = "test_mb_in1.jsonl";
    const char *bl = "test_mb_bl1.bin";
    cleanup(bl);
    write_jsonl(input, "{\"t\":1,\"v\":{\"1\":20.5,\"30\":true}}\n");

    char args[256];
    snprintf(args, sizeof(args), "--channels %s", CONFIG_PATH);
    CHECK_EQ(run_gardener(args, input), 0);
    CHECK(!file_exists(bl));

    remove(input);
}

static void test_baseline_fresh_creates_file(void) {
    const char *input = "test_mb_in2.jsonl";
    const char *bl = "test_mb_bl2.bin";
    cleanup(bl);
    write_jsonl(input,
        "{\"t\":1000,\"v\":{\"1\":20.5,\"30\":true}}\n"
        "{\"t\":2000,\"v\":{\"1\":21.0,\"30\":true}}\n"
        "{\"t\":3000,\"v\":{\"1\":21.5,\"30\":true}}\n");

    char args[512];
    snprintf(args, sizeof(args),
             "--channels %s --baseline %s --baseline-fresh",
             CONFIG_PATH, bl);
    CHECK_EQ(run_gardener(args, input), 0);
    CHECK(file_exists(bl));

    static baseline_t b;
    CHECK(baseline_load(&b, bl, NULL) == 0);
    /* Both ch 1 and ch 30 appear in the packets, so both get slots. */
    CHECK_EQ(baseline_channel_count(&b), 2);

    const baseline_stat_t *s = first_regime(&b, 1);
    if (!s) {
        fprintf(stderr, "  DEBUG: no regime has data for ch 1\n");
        dump_baseline(&b);
    }
    CHECK(s != NULL);
    if (s) {
        CHECK(s->count >= 1.0f);
        CHECK(s->mean >= 20.0f && s->mean <= 22.0f);
    }

    remove(input);
    cleanup(bl);
}

static void test_baseline_resume_across_runs(void) {
    const char *in1 = "test_mb_in3a.jsonl";
    const char *in2 = "test_mb_in3b.jsonl";
    const char *bl  = "test_mb_bl3.bin";
    cleanup(bl);

    write_jsonl(in1,
        "{\"t\":1000,\"v\":{\"1\":10.0,\"30\":true}}\n"
        "{\"t\":2000,\"v\":{\"1\":11.0,\"30\":true}}\n"
        "{\"t\":3000,\"v\":{\"1\":12.0,\"30\":true}}\n");
    write_jsonl(in2,
        "{\"t\":4000,\"v\":{\"1\":13.0,\"30\":true}}\n"
        "{\"t\":5000,\"v\":{\"1\":14.0,\"30\":true}}\n");

    char args[512];
    snprintf(args, sizeof(args),
             "--channels %s --baseline %s --baseline-fresh",
             CONFIG_PATH, bl);
    CHECK_EQ(run_gardener(args, in1), 0);

    snprintf(args, sizeof(args),
             "--channels %s --baseline %s", CONFIG_PATH, bl);
    CHECK_EQ(run_gardener(args, in2), 0);

    static baseline_t b;
    CHECK(baseline_load(&b, bl, NULL) == 0);
    const baseline_stat_t *s = first_regime(&b, 1);
    if (!s) {
        fprintf(stderr, "  DEBUG: no regime has data for ch 1\n");
        dump_baseline(&b);
    }
    CHECK(s != NULL);
    if (s) {
        CHECK(s->count >= 1.0f);
        CHECK(s->mean >= 10.0f && s->mean <= 15.0f);
    }

    remove(in1);
    remove(in2);
    cleanup(bl);
}

static void test_baseline_fresh_ignores_existing(void) {
    const char *in1 = "test_mb_in4a.jsonl";
    const char *in2 = "test_mb_in4b.jsonl";
    const char *bl  = "test_mb_bl4.bin";
    cleanup(bl);

    write_jsonl(in1,
        "{\"t\":1,\"v\":{\"1\":10.0,\"30\":true}}\n"
        "{\"t\":2,\"v\":{\"1\":11.0,\"30\":true}}\n"
        "{\"t\":3,\"v\":{\"1\":12.0,\"30\":true}}\n");
    write_jsonl(in2, "{\"t\":100,\"v\":{\"1\":50.0,\"30\":true}}\n");

    char args[512];
    snprintf(args, sizeof(args),
             "--channels %s --baseline %s --baseline-fresh",
             CONFIG_PATH, bl);
    CHECK_EQ(run_gardener(args, in1), 0);

    snprintf(args, sizeof(args),
             "--channels %s --baseline %s --baseline-fresh",
             CONFIG_PATH, bl);
    CHECK_EQ(run_gardener(args, in2), 0);

    static baseline_t b;
    CHECK(baseline_load(&b, bl, NULL) == 0);
    const baseline_stat_t *s = first_regime(&b, 1);
    if (!s) {
        fprintf(stderr, "  DEBUG: no regime has data for ch 1\n");
        dump_baseline(&b);
    }
    CHECK(s != NULL);
    if (s) {
        /* Only run 2's single sample should be present. */
        CHECK(s->count >= 0.99f && s->count <= 1.01f);
        CHECK(s->mean >= 49.0f && s->mean <= 51.0f);
    }

    remove(in1);
    remove(in2);
    cleanup(bl);
}

static void test_baseline_replay_mode(void) {
    const char *in  = "test_mb_in5.jsonl";
    const char *log = "test_mb_log5.bin";
    const char *bl  = "test_mb_bl5.bin";
    cleanup(log);
    cleanup(bl);

    write_jsonl(in,
        "{\"t\":1000,\"v\":{\"1\":10.0,\"30\":true}}\n"
        "{\"t\":2000,\"v\":{\"1\":20.0,\"30\":true}}\n"
        "{\"t\":3000,\"v\":{\"1\":30.0,\"30\":true}}\n");

    char args[512];
    snprintf(args, sizeof(args),
             "--channels %s --log %s --log-fresh",
             CONFIG_PATH, log);
    CHECK_EQ(run_gardener(args, in), 0);
    CHECK(file_exists(log));

    snprintf(args, sizeof(args),
             "--channels %s --replay %s --baseline %s --baseline-fresh",
             CONFIG_PATH, log, bl);
    CHECK_EQ(run_gardener(args, NULL), 0);
    CHECK(file_exists(bl));

    static baseline_t b;
    CHECK(baseline_load(&b, bl, NULL) == 0);
    const baseline_stat_t *s = first_regime(&b, 1);
    if (!s) {
        fprintf(stderr, "  DEBUG: no regime has data for ch 1\n");
        dump_baseline(&b);
    }
    CHECK(s != NULL);
    if (s) {
        CHECK(s->count >= 1.0f);
        CHECK(s->mean >= 10.0f && s->mean <= 30.0f);
    }

    remove(in);
    cleanup(log);
    cleanup(bl);
}

static void test_baseline_corrupt_file_starts_fresh(void) {
    const char *in = "test_mb_in6.jsonl";
    const char *bl = "test_mb_bl6.bin";
    cleanup(bl);
    write_jsonl(in, "{\"t\":1,\"v\":{\"1\":20.5,\"30\":true}}\n");

    FILE *f = fopen(bl, "wb");
    if (f) {
        uint8_t junk[100];
        memset(junk, 0xAA, sizeof(junk));
        fwrite(junk, 1, sizeof(junk), f);
        fclose(f);
    }

    char args[512];
    snprintf(args, sizeof(args),
             "--channels %s --baseline %s", CONFIG_PATH, bl);
    CHECK_EQ(run_gardener(args, in), 0);

    static baseline_t b;
    CHECK(baseline_load(&b, bl, NULL) == 0);
    CHECK_EQ(baseline_channel_count(&b), 2);

    remove(in);
    cleanup(bl);
}

int main(void) {
    plat_log_set_level(LOG_ERROR);

    test_baseline_no_flag_no_file();
    test_baseline_fresh_creates_file();
    test_baseline_resume_across_runs();
    test_baseline_fresh_ignores_existing();
    test_baseline_replay_mode();
    test_baseline_corrupt_file_starts_fresh();

    if (g_failures) {
        fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_main_baseline: all checks passed\n");
    return 0;
}