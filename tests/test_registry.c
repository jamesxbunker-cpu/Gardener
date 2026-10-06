#include "gardener/registry.h"
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

#define CHECK_STR(a, b) do {                                          \
    const char *_a = (a), *_b = (b);                                  \
    if (!_a || !_b || strcmp(_a, _b) != 0) {                          \
        fprintf(stderr, "FAIL %s:%d: \"%s\" != \"%s\"\n",             \
                __FILE__, __LINE__, _a ? _a : "(null)", _b);          \
        g_failures++;                                                 \
    }                                                                 \
} while (0)

/* Write a config to a temp file. Caller owns the returned path. */
static const char *write_cfg(const char *contents) {
    const char *path = "test_registry_tmp.toml";
    FILE *f = fopen(path, "w");
    if (!f) { perror("fopen"); exit(2); }
    fputs(contents, f);
    fclose(f);
    return path;
}

static void test_empty_file(void) {
    const char *p = write_cfg("");
    static registry_t r;
    registry_init(&r);
    CHECK(registry_load_toml(&r, p) == 0);
    CHECK_EQ(r.count, 0);
    remove(p);
}

static void test_missing_file(void) {
    static registry_t r;
    registry_init(&r);
    CHECK(registry_load_toml(&r, "/nonexistent/nope.toml") == -1);
}

static void test_one_channel_all_fields(void) {
    const char *p = write_cfg(
        "[[channel]]\n"
        "id = 1\n"
        "name = \"temp\"\n"
        "unit = \"C\"\n"
        "type = \"f32\"\n"
        "role = \"sensor\"\n"
        "min = 0\n"
        "max = 150\n"
        "controllable = false\n"
        "is_regime = false\n");
    static registry_t r;
    registry_init(&r);
    CHECK(registry_load_toml(&r, p) == 0);
    CHECK_EQ(r.count, 1);
    CHECK_EQ(r.warnings, 0);

    const channel_desc_t *c = registry_get(&r, 1);
    CHECK(c != NULL);
    CHECK_STR(c->name, "temp");
    CHECK_STR(c->unit, "C");
    CHECK_EQ(c->type, CH_F32);
    CHECK_EQ(c->role, ROLE_SENSOR);
    CHECK(c->has_range);
    CHECK_EQ((int)c->min_valid, 0);
    CHECK_EQ((int)c->max_valid, 150);
    CHECK(!c->controllable);
    CHECK(!c->is_regime);
    remove(p);
}

static void test_multiple_channels(void) {
    const char *p = write_cfg(
        "[[channel]]\n"
        "id = 1\n"
        "name = \"a\"\n"
        "type = \"f32\"\n"
        "role = \"sensor\"\n"
        "\n"
        "[[channel]]\n"
        "id = 2\n"
        "name = \"b\"\n"
        "type = \"bool\"\n"
        "role = \"status\"\n"
        "is_regime = true\n");
    static registry_t r;
    registry_init(&r);
    CHECK(registry_load_toml(&r, p) == 0);
    CHECK_EQ(r.count, 2);
    CHECK(registry_get(&r, 1) != NULL);
    CHECK(registry_get(&r, 2) != NULL);
    CHECK(registry_get(&r, 999) == NULL);

    const channel_desc_t *b = registry_get(&r, 2);
    CHECK_EQ(b->type, CH_BOOL);
    CHECK_EQ(b->role, ROLE_STATUS);
    CHECK(b->is_regime);
    remove(p);
}

static void test_comments_and_blank_lines(void) {
    const char *p = write_cfg(
        "# leading comment\n"
        "\n"
        "[[channel]]\n"
        "# inline comment on its own line\n"
        "id = 7\n"
        "name = \"x\"  # trailing comment not supported, treat as part of value\n"
        "type = \"f32\"\n"
        "role = \"sensor\"\n");
    static registry_t r;
    registry_init(&r);
    CHECK(registry_load_toml(&r, p) == 0);
    /* name will include "  # trailing comment...", because we do not
     * strip trailing comments. That is a known limitation; verify the
     * channel still loads with the id and role correct. */
    const channel_desc_t *c = registry_get(&r, 7);
    CHECK(c != NULL);
    CHECK_EQ(c->role, ROLE_SENSOR);
    remove(p);
}

static void test_unknown_key_warns(void) {
    const char *p = write_cfg(
        "[[channel]]\n"
        "id = 1\n"
        "name = \"x\"\n"
        "role = \"sensor\"\n"
        "not_a_real_key = 5\n");
    static registry_t r;
    registry_init(&r);
    CHECK(registry_load_toml(&r, p) == 0);
    CHECK_EQ(r.count, 1);
    CHECK_EQ(r.warnings, 1);
    remove(p);
}

static void test_unknown_type_and_role(void) {
    const char *p = write_cfg(
        "[[channel]]\n"
        "id = 1\n"
        "name = \"x\"\n"
        "type = \"float64\"\n"   /* not supported */
        "role = \"sensor\"\n");
    static registry_t r;
    registry_init(&r);
    CHECK(registry_load_toml(&r, p) == 0);
    CHECK_EQ(r.count, 1);
    CHECK(r.warnings >= 1);
    /* type falls back to default f32 */
    const channel_desc_t *c = registry_get(&r, 1);
    CHECK(c != NULL);
    CHECK_EQ(c->type, CH_F32);
    remove(p);
}

static void test_missing_name_dropped(void) {
    const char *p = write_cfg(
        "[[channel]]\n"
        "id = 1\n"
        "role = \"sensor\"\n");    /* no name */
    static registry_t r;
    registry_init(&r);
    CHECK(registry_load_toml(&r, p) == 0);
    CHECK_EQ(r.count, 0);
    CHECK(r.warnings >= 1);
    remove(p);
}

static void test_get_or_default_synthesizes(void) {
    static registry_t r;
    registry_init(&r);
    const channel_desc_t *c = registry_get_or_default(&r, 42);
    CHECK(c != NULL);
    CHECK_EQ(c->id, 42);
    CHECK_EQ(c->role, ROLE_UNKNOWN);
    CHECK_EQ(c->type, CH_F32);
    CHECK(c->name != NULL);
    CHECK_EQ(r.count, 1);
    /* calling again should return the same entry, not add another */
    const channel_desc_t *c2 = registry_get_or_default(&r, 42);
    CHECK(c2 == c);
    CHECK_EQ(r.count, 1);
}

static void test_get_or_default_returns_existing(void) {
    const char *p = write_cfg(
        "[[channel]]\n"
        "id = 5\n"
        "name = \"known\"\n"
        "role = \"sensor\"\n");
    static registry_t r;
    registry_init(&r);
    CHECK(registry_load_toml(&r, p) == 0);
    const channel_desc_t *c = registry_get_or_default(&r, 5);
    CHECK(c != NULL);
    CHECK_STR(c->name, "known");
    CHECK_EQ(r.count, 1);
    remove(p);
}

static void test_capacity_limit(void) {
    /* Fill up the registry and confirm extra entries are rejected.
     * Generating 300 channels via string concat in C is tedious; a
     * simple template with sprintf works. */
    FILE *f = fopen("test_registry_many.toml", "w");
    if (!f) { perror("fopen"); exit(2); }
    for (int i = 0; i < REGISTRY_MAX_CHANNELS + 5; i++) {
        fprintf(f,
                "[[channel]]\n"
                "id = %d\n"
                "name = \"c%d\"\n"
                "type = \"f32\"\n"
                "role = \"sensor\"\n\n",
                i, i);
    }
    fclose(f);

    static registry_t r;
    registry_init(&r);
    CHECK(registry_load_toml(&r, "test_registry_many.toml") == 0);
    CHECK_EQ(r.count, REGISTRY_MAX_CHANNELS);
    CHECK(r.warnings >= 1);
    remove("test_registry_many.toml");
}

int main(void) {
    plat_log_set_level(LOG_ERROR);  /* silence expected warnings */

    test_empty_file();
    test_missing_file();
    test_one_channel_all_fields();
    test_multiple_channels();
    test_comments_and_blank_lines();
    test_unknown_key_warns();
    test_unknown_type_and_role();
    test_missing_name_dropped();
    test_get_or_default_synthesizes();
    test_get_or_default_returns_existing();
    test_capacity_limit();

    if (g_failures) {
        fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_registry: all checks passed\n");
    return 0;
}