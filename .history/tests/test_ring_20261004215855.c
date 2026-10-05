#include "gardener/ring.h"
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

static sample_t mk(uint16_t id, float v, uint64_t t) {
    sample_t s;
    memset(&s, 0, sizeof(s));
    s.channel_id   = id;
    s.type         = CH_F32;
    s.role         = ROLE_SENSOR;
    s.flags        = CH_FLAG_VALID;
    s.v.f          = v;
    s.timestamp_ns = t;
    return s;
}

static void test_init_rejects_bad_args(void) {
    ring_t r;
    sample_t storage[4];
    CHECK(ring_init(NULL, storage, 4) == -1);
    CHECK(ring_init(&r, NULL, 4) == -1);
    CHECK(ring_init(&r, storage, 0) == -1);
    CHECK(ring_init(&r, storage, 4) == 0);
    CHECK(ring_count(&r) == 0);
    CHECK(ring_capacity(&r) == 4);
    CHECK(!ring_full(&r));
    CHECK(ring_newest(&r) == NULL);
    CHECK(ring_oldest(&r) == NULL);
    CHECK(ring_at(&r, 0) == NULL);
}

static void test_push_under_capacity(void) {
    ring_t r;
    sample_t storage[4];
    ring_init(&r, storage, 4);

    for (int i = 0; i < 3; i++) {
        sample_t s = mk(1, (float)i, (uint64_t)(1000 + i));
        ring_push(&r, &s);
    }
    CHECK_EQ(ring_count(&r), 3);
    CHECK(!ring_full(&r));
    CHECK_EQ(ring_at(&r, 0)->v.f, 0.0f);
    CHECK_EQ(ring_at(&r, 1)->v.f, 1.0f);
    CHECK_EQ(ring_at(&r, 2)->v.f, 2.0f);
    CHECK(ring_at(&r, 3) == NULL);
    CHECK_EQ(ring_newest(&r)->v.f, 2.0f);
    CHECK_EQ(ring_oldest(&r)->v.f, 0.0f);
}

static void test_push_exactly_full(void) {
    ring_t r;
    sample_t storage[4];
    ring_init(&r, storage, 4);
    for (int i = 0; i < 4; i++) {
        sample_t s = mk(1, (float)i, (uint64_t)i);
        ring_push(&r, &s);
    }
    CHECK_EQ(ring_count(&r), 4);
    CHECK(ring_full(&r));
    CHECK_EQ(ring_at(&r, 0)->v.f, 0.0f);
    CHECK_EQ(ring_at(&r, 3)->v.f, 3.0f);
    CHECK_EQ(ring_newest(&r)->v.f, 3.0f);
    CHECK_EQ(ring_oldest(&r)->v.f, 0.0f);
}

static void test_wrap_overwrites_oldest(void) {
    ring_t r;
    sample_t storage[4];
    ring_init(&r, storage, 4);

    /* Push 6 into cap 4. Oldest two (0, 1) should be gone. */
    for (int i = 0; i < 6; i++) {
        sample_t s = mk(1, (float)i, (uint64_t)i);
        ring_push(&r, &s);
    }
    CHECK_EQ(ring_count(&r), 4);
    CHECK(ring_full(&r));
    CHECK_EQ(ring_at(&r, 0)->v.f, 2.0f);
    CHECK_EQ(ring_at(&r, 1)->v.f, 3.0f);
    CHECK_EQ(ring_at(&r, 2)->v.f, 4.0f);
    CHECK_EQ(ring_at(&r, 3)->v.f, 5.0f);
    CHECK_EQ(ring_newest(&r)->v.f, 5.0f);
    CHECK_EQ(ring_oldest(&r)->v.f, 2.0f);
}

static void test_wrap_twice_ordering(void) {
    ring_t r;
    sample_t storage[3];
    ring_init(&r, storage, 3);
    for (int i = 0; i < 10; i++) {
        sample_t s = mk(1, (float)i, (uint64_t)i);
        ring_push(&r, &s);
    }
    /* Last three should be 7, 8, 9 in order. */
    CHECK_EQ(ring_count(&r), 3);
    CHECK_EQ(ring_at(&r, 0)->v.f, 7.0f);
    CHECK_EQ(ring_at(&r, 1)->v.f, 8.0f);
    CHECK_EQ(ring_at(&r, 2)->v.f, 9.0f);
}

static void test_clear(void) {
    ring_t r;
    sample_t storage[4];
    ring_init(&r, storage, 4);
    for (int i = 0; i < 4; i++) {
        sample_t s = mk(1, (float)i, (uint64_t)i);
        ring_push(&r, &s);
    }
    ring_clear(&r);
    CHECK_EQ(ring_count(&r), 0);
    CHECK(!ring_full(&r));
    CHECK(ring_newest(&r) == NULL);
    CHECK(ring_at(&r, 0) == NULL);

    /* Should accept fresh pushes cleanly. */
    sample_t s = mk(1, 42.0f, 999);
    ring_push(&r, &s);
    CHECK_EQ(ring_count(&r), 1);
    CHECK_EQ(ring_newest(&r)->v.f, 42.0f);
}

static void test_capacity_one(void) {
    ring_t r;
    sample_t storage[1];
    ring_init(&r, storage, 1);
    sample_t a = mk(1, 1.0f, 1);
    sample_t b = mk(1, 2.0f, 2);
    ring_push(&r, &a);
    CHECK_EQ(ring_count(&r), 1);
    CHECK_EQ(ring_newest(&r)->v.f, 1.0f);
    ring_push(&r, &b);
    CHECK_EQ(ring_count(&r), 1);
    CHECK_EQ(ring_newest(&r)->v.f, 2.0f);
    CHECK(ring_full(&r));
}

static void test_sample_as_f32(void) {
    sample_t s;
    memset(&s, 0, sizeof(s));

    s.type = CH_F32; s.v.f = 3.5f;
    CHECK(sample_as_f32(&s) == 3.5f);

    s.type = CH_I32; s.v.i = -7;
    CHECK(sample_as_f32(&s) == -7.0f);

    s.type = CH_U16; s.v.u = 65535;
    CHECK(sample_as_f32(&s) == 65535.0f);

    s.type = CH_BOOL; s.v.b = 1;
    CHECK(sample_as_f32(&s) == 1.0f);
    s.v.b = 0;
    CHECK(sample_as_f32(&s) == 0.0f);

    s.type = CH_ENUM; s.v.i = 3;
    CHECK(sample_as_f32(&s) == 3.0f);
}

static void test_sample_size_is_24(void) {
    /* Lock the in-memory layout. If a compiler or a future field change
     * breaks this, it should fail loudly here, not silently corrupt the
     * wire format or the static ingest storage. */
    CHECK_EQ(sizeof(sample_t), 24);
    CHECK_EQ(offsetof(sample_t, channel_id), 0);
    CHECK_EQ(offsetof(sample_t, type), 2);
    CHECK_EQ(offsetof(sample_t, role), 3);
    CHECK_EQ(offsetof(sample_t, flags), 4);
    CHECK_EQ(offsetof(sample_t, _pad), 5);
    CHECK_EQ(offsetof(sample_t, v), 8);
    CHECK_EQ(offsetof(sample_t, timestamp_ns), 16);
}

int main(void) {
    test_init_rejects_bad_args();
    test_push_under_capacity();
    test_push_exactly_full();
    test_wrap_overwrites_oldest();
    test_wrap_twice_ordering();
    test_clear();
    test_capacity_one();
    test_sample_as_f32();
    test_sample_size_is_24();

    if (g_failures) {
        fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_ring: all checks passed\n");
    return 0;
}