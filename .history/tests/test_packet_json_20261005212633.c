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
   