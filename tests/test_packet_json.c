#include "gardener/packet.h"
#include "gardener/platform.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

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

#define CHECK_NEAR(a, b, eps) do {                                    \
    double _a = (double)(a), _b = (double)(b);                        \
    if (fabs(_a - _b) > (eps)) {                                      \
        fprintf(stderr, "FAIL %s:%d: %g != %g (eps=%g)\n",            \
                __FILE__, __LINE__, _a, _b, (double)(eps));           \
        g_failures++;                                                 \
    }                                                                 \
} while (0)

static sample_t mk_f32(uint16_t id, float v) {
    sample_t s; memset(&s, 0, sizeof(s));
    s.channel_id = id; s.type = CH_F32; s.role = ROLE_SENSOR;
    s.flags = CH_FLAG_VALID; s.v.f = v;
    return s;
}

/* ---- decode: canonical shape ---------------------------------------- */

static void test_decode_one_f32(void) {
    packet_t p;
    int rc = packet_from_json(
        "{\"t\":1000,\"src\":1,\"ch\":[{\"id\":1,\"role\":1,\"v\":20.5}]}", &p);
    CHECK_EQ(rc, 0);
    CHECK_EQ(p.timestamp_ns, 1000);
    CHECK_EQ(p.source_id, 1);
    CHECK_EQ(p.channel_count, 1);
    CHECK_EQ(p.samples[0].channel_id, 1);
    CHECK_EQ(p.samples[0].role, ROLE_SENSOR);
    CHECK_EQ(p.samples[0].type, CH_F32);
    CHECK_NEAR(p.samples[0].v.f, 20.5, 1e-6);
}

static void test_decode_multiple(void) {
    packet_t p;
    int rc = packet_from_json(
        "{\"t\":1000,\"src\":2,\"ch\":["
        "{\"id\":1,\"v\":20.5},"
        "{\"id\":2,\"v\":50},"
        "{\"id\":3,\"type\":\"bool\",\"v\":true}"
        "]}", &p);
    CHECK_EQ(rc, 0);
    CHECK_EQ(p.channel_count, 3);
    CHECK_NEAR(p.samples[0].v.f, 20.5, 1e-6);
    CHECK_EQ(p.samples[1].v.i, 50);
    CHECK_EQ(p.samples[2].v.b, 1);
    CHECK_EQ(p.samples[2].type, CH_BOOL);
}

static void test_decode_explicit_type_wins(void) {
    /* A whole number with an explicit f32 type should stay f32, not be
     * silently reinterpreted as i32. This catches the bug the parse
     * order fix addresses. */
    packet_t p;
    int rc = packet_from_json(
        "{\"t\":1,\"ch\":[{\"id\":5,\"type\":\"f32\",\"v\":20}]}", &p);
    CHECK_EQ(rc, 0);
    CHECK_EQ(p.samples[0].type, CH_F32);
    CHECK_NEAR(p.samples[0].v.f, 20.0, 1e-6);
}

static void test_decode_shorthand(void) {
    packet_t p;
    int rc = packet_from_json("{\"t\":1000,\"v\":{\"1\":20.5,\"3\":true}}", &p);
    CHECK_EQ(rc, 0);
    CHECK_EQ(p.channel_count, 2);
    CHECK_EQ(p.samples[0].channel_id, 1);
    CHECK_NEAR(p.samples[0].v.f, 20.5, 1e-6);
    CHECK_EQ(p.samples[1].channel_id, 3);
    CHECK_EQ(p.samples[1].v.b, 1);
    CHECK_EQ(p.samples[1].type, CH_BOOL);
}

/* ---- decode: tolerance ---------------------------------------------- */

static void test_decode_whitespace(void) {
    packet_t p;
    int rc = packet_from_json(
        "  { \"t\" : 1000 , \"v\" : { \"1\" : 20.5 } }  ", &p);
    CHECK_EQ(rc, 0);
    CHECK_EQ(p.timestamp_ns, 1000);
    CHECK_EQ(p.channel_count, 1);
}

static void test_decode_unknown_keys_ignored(void) {
    packet_t p;
    int rc = packet_from_json(
        "{\"t\":1000,\"extra\":\"whatever\",\"src\":1,"
        "\"ch\":[{\"id\":1,\"v\":20.5,\"note\":\"hot\"}]}", &p);
    CHECK_EQ(rc, 0);
    CHECK_EQ(p.channel_count, 1);
    CHECK_NEAR(p.samples[0].v.f, 20.5, 1e-6);
}

static void test_decode_key_order_irrelevant(void) {
    packet_t p;
    int rc = packet_from_json(
        "{\"ch\":[{\"v\":20.5,\"id\":1}],\"src\":1,\"t\":1000}", &p);
    CHECK_EQ(rc, 0);
    CHECK_EQ(p.channel_count, 1);
    CHECK_EQ(p.samples[0].channel_id, 1);
}

static void test_decode_negative(void) {
    packet_t p;
    int rc = packet_from_json("{\"t\":1,\"ch\":[{\"id\":1,\"v\":-5.5}]}", &p);
    CHECK_EQ(rc, 0);
    CHECK_NEAR(p.samples[0].v.f, -5.5, 1e-6);
}

/* ---- decode: failure modes ------------------------------------------ */

static void test_decode_empty_string(void) {
    packet_t p;
    CHECK(packet_from_json("", &p) == -1);
}

static void test_decode_no_t(void) {
    packet_t p;
    CHECK(packet_from_json("{\"v\":{\"1\":1}}", &p) == -1);
}

static void test_decode_no_channels(void) {
    packet_t p;
    CHECK(packet_from_json("{\"t\":1}", &p) == -1);
}

static void test_decode_empty_ch(void) {
    packet_t p;
    CHECK(packet_from_json("{\"t\":1,\"ch\":[]}", &p) == -1);
}

static void test_decode_empty_v(void) {
    packet_t p;
    CHECK(packet_from_json("{\"t\":1,\"v\":{}}", &p) == -1);
}

static void test_decode_bad_json(void) {
    packet_t p;
    CHECK(packet_from_json("not json at all", &p) == -1);
}

/* ---- encode --------------------------------------------------------- */

static void test_encode_basic(void) {
    packet_t p; memset(&p, 0, sizeof(p));
    p.timestamp_ns = 1000;
    p.source_id = 1;
    p.channel_count = 1;
    p.samples[0] = mk_f32(1, 20.5f);

    char buf[512];
    int n = packet_to_json(&p, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK(strstr(buf, "\"t\":1000") != NULL);
    CHECK(strstr(buf, "\"src\":1") != NULL);
    CHECK(strstr(buf, "\"id\":1") != NULL);
    CHECK(strstr(buf, "\"v\":20.5") != NULL);
}

static void test_encode_bool(void) {
    packet_t p; memset(&p, 0, sizeof(p));
    p.timestamp_ns = 1;
    p.channel_count = 1;
    p.samples[0].channel_id = 3;
    p.samples[0].type = CH_BOOL;
    p.samples[0].role = ROLE_STATUS;
    p.samples[0].v.b = 1;

    char buf[256];
    int n = packet_to_json(&p, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK(strstr(buf, "\"v\":true") != NULL);
    CHECK(strstr(buf, "\"type\":\"bool\"") != NULL);
    CHECK(strstr(buf, "\"role\":5") != NULL);
}

static void test_encode_roundtrip(void) {
    packet_t src; memset(&src, 0, sizeof(src));
    src.timestamp_ns = 1234567890ull;
    src.source_id = 7;
    src.channel_count = 3;
    src.samples[0] = mk_f32(1, 20.5f);
    src.samples[1] = mk_f32(2, -3.25f);
    src.samples[2].channel_id = 3;
    src.samples[2].type = CH_BOOL;
    src.samples[2].role = ROLE_STATUS;
    src.samples[2].v.b = 1;

    char buf[1024];
    int n = packet_to_json(&src, buf, sizeof(buf));
    CHECK(n > 0);

    packet_t dst;
    CHECK(packet_from_json(buf, &dst) == 0);
    CHECK_EQ(dst.timestamp_ns, src.timestamp_ns);
    CHECK_EQ(dst.source_id, src.source_id);
    CHECK_EQ(dst.channel_count, src.channel_count);
    CHECK_NEAR(dst.samples[0].v.f, src.samples[0].v.f, 1e-6);
    CHECK_NEAR(dst.samples[1].v.f, src.samples[1].v.f, 1e-6);
    CHECK_EQ(dst.samples[2].v.b, src.samples[2].v.b);
    CHECK_EQ(dst.samples[2].type, CH_BOOL);
}

static void test_encode_null_and_zero_cap(void) {
    packet_t p; memset(&p, 0, sizeof(p));
    char buf[8];
    CHECK(packet_to_json(NULL, buf, sizeof(buf)) == -1);
    CHECK(packet_to_json(&p, NULL, 0) == -1);
}

static void test_encode_returns_needed_length(void) {
    /* Ask for way too little space; function should return the length
     * it needs, which will be > cap. */
    packet_t p; memset(&p, 0, sizeof(p));
    p.timestamp_ns = 1000;
    p.channel_count = 1;
    p.samples[0] = mk_f32(1, 20.5f);

    char tiny[4];
    int n = packet_to_json(&p, tiny, sizeof(tiny));
    CHECK(n > (int)sizeof(tiny));
}

int main(void) {
    plat_log_set_level(LOG_ERROR);

    test_decode_one_f32();
    test_decode_multiple();
    test_decode_explicit_type_wins();
    test_decode_shorthand();
    test_decode_whitespace();
    test_decode_unknown_keys_ignored();
    test_decode_key_order_irrelevant();
    test_decode_negative();
    test_decode_empty_string();
    test_decode_no_t();
    test_decode_no_channels();
    test_decode_empty_ch();
    test_decode_empty_v();
    test_decode_bad_json();
    test_encode_basic();
    test_encode_bool();
    test_encode_roundtrip();
    test_encode_null_and_zero_cap();
    test_encode_returns_needed_length();

    if (g_failures) {
        fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_packet_json: all checks passed\n");
    return 0;
}