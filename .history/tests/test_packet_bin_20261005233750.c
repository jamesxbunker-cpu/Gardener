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

#define CHECK_NEAR(a, b, eps) do {                                    \
    double _a = (double)(a), _b = (double)(b);                        \
    if (fabs(_a - _b) > (eps)) {                                      \
        fprintf(stderr, "FAIL %s:%d: %g != %g (eps=%g)\n",            \
                __FILE__, __LINE__, _a, _b, (double)(eps));           \
        g_failures++;                                                 \
    }                                                                 \
} while (0)

/* ---- helpers --------------------------------------------------------- */

static sample_t mk_f32(uint16_t id, float v, uint64_t ts) {
    sample_t s; memset(&s, 0, sizeof(s));
    s.channel_id = id; s.type = CH_F32; s.role = ROLE_SENSOR;
    s.flags = CH_FLAG_VALID; s.v.f = v; s.timestamp_ns = ts;
    return s;
}

static sample_t mk_i32(uint16_t id, int32_t v, uint64_t ts) {
    sample_t s; memset(&s, 0, sizeof(s));
    s.channel_id = id; s.type = CH_I32; s.role = ROLE_COUNTER;
    s.flags = CH_FLAG_VALID; s.v.i = v; s.timestamp_ns = ts;
    return s;
}

static sample_t mk_bool(uint16_t id, int v, uint64_t ts) {
    sample_t s; memset(&s, 0, sizeof(s));
    s.channel_id = id; s.type = CH_BOOL; s.role = ROLE_STATUS;
    s.flags = CH_FLAG_VALID; s.v.b = (uint8_t)(v ? 1 : 0);
    s.timestamp_ns = ts;
    return s;
}

static packet_t mk_packet(uint64_t ts, uint16_t src,
                          const sample_t *samples, uint16_t n) {
    packet_t p; memset(&p, 0, sizeof(p));
    p.timestamp_ns = ts;
    p.source_id = src;
    p.channel_count = n;
    for (uint16_t i = 0; i < n; i++) p.samples[i] = samples[i];
    return p;
}

/* ---- size math ------------------------------------------------------- */

static void test_encoded_size_formula(void) {
    packet_t p; memset(&p, 0, sizeof(p));
    p.channel_count = 0;
    uint8_t buf[128];                              /* was 64 */
    int n = packet_encode(&p, buf, sizeof(buf));
    CHECK_EQ(n, PACKET_BIN_HEADER + PACKET_BIN_TRAILER);   /* 22 */

    p.channel_count = 3;
    n = packet_encode(&p, buf, sizeof(buf));
    CHECK_EQ(n, 22 + 3 * 18);   /* 76 */
}

static void test_max_size_fits_in_PACKET_BIN_MAX(void) {
    packet_t p; memset(&p, 0, sizeof(p));
    p.channel_count = PACKET_MAX_CH;
    static uint8_t buf[PACKET_BIN_MAX];
    int n = packet_encode(&p, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK_EQ(n, PACKET_BIN_MAX);
}

/* ---- round-trip ------------------------------------------------------ */

static void test_roundtrip_empty(void) {
    packet_t p = mk_packet(1000, 1, NULL, 0);
    uint8_t buf[64];
    int n = packet_encode(&p, buf, sizeof(buf));
    CHECK(n > 0);

    packet_t d;
    int m = packet_decode(buf, (size_t)n, &d);
    CHECK_EQ(m, n);
    CHECK_EQ(d.timestamp_ns, 1000);
    CHECK_EQ(d.source_id, 1);
    CHECK_EQ(d.channel_count, 0);
}

static void test_roundtrip_one_f32(void) {
    sample_t s = mk_f32(1, 20.5f, 500);
    packet_t p = mk_packet(1000, 2, &s, 1);

    uint8_t buf[64];
    int n = packet_encode(&p, buf, sizeof(buf));
    CHECK_EQ(n, 22 + 18);

    packet_t d;
    CHECK_EQ(packet_decode(buf, (size_t)n, &d), n);
    CHECK_EQ(d.timestamp_ns, 1000);
    CHECK_EQ(d.source_id, 2);
    CHECK_EQ(d.channel_count, 1);
    CHECK_EQ(d.samples[0].channel_id, 1);
    CHECK_EQ(d.samples[0].type, CH_F32);
    CHECK_EQ(d.samples[0].role, ROLE_SENSOR);
    CHECK_EQ(d.samples[0].flags, CH_FLAG_VALID);
    CHECK_NEAR(d.samples[0].v.f, 20.5, 1e-6);
    CHECK_EQ(d.samples[0].timestamp_ns, 500);
}

static void test_roundtrip_all_types(void) {
    sample_t s[5];
    s[0] = mk_f32(1, 20.5f, 100);
    s[1] = mk_i32(2, -42, 200);
    s[2] = mk_bool(3, 1, 300);
    s[3].channel_id = 4; s[3].type = CH_U16; s[3].role = ROLE_STATUS;
    s[3].flags = CH_FLAG_VALID; s[3].v.u = 65535; s[3].timestamp_ns = 400;
    s[4].channel_id = 5; s[4].type = CH_ENUM; s[4].role = ROLE_STATUS;
    s[4].flags = CH_FLAG_VALID; s[4].v.i = 3; s[4].timestamp_ns = 500;

    packet_t p = mk_packet(9999, 7, s, 5);
    uint8_t buf[256];
    int n = packet_encode(&p, buf, sizeof(buf));
    CHECK_EQ(n, 22 + 5 * 18);

    packet_t d;
    CHECK_EQ(packet_decode(buf, (size_t)n, &d), n);
    CHECK_EQ(d.channel_count, 5);

    CHECK_NEAR(d.samples[0].v.f, 20.5, 1e-6);
    CHECK_EQ(d.samples[1].v.i, -42);
    CHECK_EQ(d.samples[2].v.b, 1);
    CHECK_EQ(d.samples[3].v.u, 65535);
    CHECK_EQ(d.samples[4].v.i, 3);

    CHECK_EQ(d.samples[0].type, CH_F32);
    CHECK_EQ(d.samples[1].type, CH_I32);
    CHECK_EQ(d.samples[2].type, CH_BOOL);
    CHECK_EQ(d.samples[3].type, CH_U16);
    CHECK_EQ(d.samples[4].type, CH_ENUM);
}

static void test_roundtrip_all_flags(void) {
    sample_t s;
    memset(&s, 0, sizeof(s));
    s.channel_id = 1; s.type = CH_F32; s.role = ROLE_SENSOR;
    s.flags = CH_FLAG_VALID | CH_FLAG_SIMULATED;
    s.v.f = 1.0f;
    s.timestamp_ns = 100;

    packet_t p = mk_packet(1000, 1, &s, 1);
    p.flags = PACKET_FLAG_SIMULATED;

    uint8_t buf[64];
    packet_encode(&p, buf, sizeof(buf));

    packet_t d;
    packet_decode(buf, sizeof(buf), &d);
    CHECK_EQ(d.flags, PACKET_FLAG_SIMULATED);
    CHECK_EQ(d.samples[0].flags, CH_FLAG_VALID | CH_FLAG_SIMULATED);
}

/* ---- failure modes --------------------------------------------------- */

static void test_encode_null_args(void) {
    packet_t p; memset(&p, 0, sizeof(p));
    uint8_t buf[64];
    CHECK(packet_encode(NULL, buf, sizeof(buf)) == -1);
    CHECK(packet_encode(&p, NULL, 0) == -1);
}

static void test_encode_too_many_channels(void) {
    packet_t p; memset(&p, 0, sizeof(p));
    p.channel_count = PACKET_MAX_CH + 1;
    uint8_t buf[PACKET_BIN_MAX];
    CHECK(packet_encode(&p, buf, sizeof(buf)) == -1);
}

static void test_encode_cap_too_small(void) {
    sample_t s = mk_f32(1, 20.5f, 0);
    packet_t p = mk_packet(1000, 1, &s, 1);
    /* Needs 22 + 18 = 40 bytes. Give it 39. */
    uint8_t buf[39];
    CHECK(packet_encode(&p, buf, sizeof(buf)) == -1);
}

static void test_decode_null_args(void) {
    uint8_t buf[64];
    packet_t d;
    CHECK(packet_decode(NULL, 64, &d) == -1);
    CHECK(packet_decode(buf, 64, NULL) == -1);
}

static void test_decode_too_short(void) {
    uint8_t buf[8] = {0};
    packet_t d;
    CHECK(packet_decode(buf, sizeof(buf), &d) == -1);
}

static void test_decode_bad_magic(void) {
    sample_t s = mk_f32(1, 20.5f, 0);
    packet_t p = mk_packet(1000, 1, &s, 1);
    uint8_t buf[64];
    int n = packet_encode(&p, buf, sizeof(buf));
    /* Corrupt the magic. */
    buf[0] ^= 0xFF;

    packet_t d;
    CHECK(packet_decode(buf, (size_t)n, &d) == -1);
}

static void test_decode_bad_version(void) {
    sample_t s = mk_f32(1, 20.5f, 0);
    packet_t p = mk_packet(1000, 1, &s, 1);
    uint8_t buf[64];
    int n = packet_encode(&p, buf, sizeof(buf));
    /* Bump the version. */
    buf[4] = 99;

    packet_t d;
    CHECK(packet_decode(buf, (size_t)n, &d) == -1);
}

static void test_decode_bad_crc(void) {
    sample_t s = mk_f32(1, 20.5f, 0);
    packet_t p = mk_packet(1000, 1, &s, 1);
    uint8_t buf[64];
    int n = packet_encode(&p, buf, sizeof(buf));
    /* Flip a bit in the payload, leave CRC unchanged. */
    buf[10] ^= 0x01;

    packet_t d;
    CHECK(packet_decode(buf, (size_t)n, &d) == -1);
}

static void test_decode_channel_count_exceeds_max(void) {
    /* Encode a valid small packet, then overwrite the channel_count
     * field with an impossible value and recompute the CRC so the CRC
     * check passes and we reach the count check. */
    packet_t p; memset(&p, 0, sizeof(p));
    p.channel_count = 1;
    uint8_t buf[256];
    int n = packet_encode(&p, buf, sizeof(buf));

    /* channel_count lives at offset 16 (4 magic + 2 ver + 2 src + 8 ts). */
    buf[16] = 0xFF;
    buf[17] = 0xFF;

    /* Recompute CRC so the crc check doesn't fail first. */
    uint16_t crc = packet_crc16(buf, (size_t)n - 2);
    buf[n - 2] = (uint8_t)(crc & 0xFF);
    buf[n - 1] = (uint8_t)((crc >> 8) & 0xFF);

    packet_t d;
    CHECK(packet_decode(buf, (size_t)n, &d) == -1);
}

static void test_decode_buffer_shorter_than_claimed(void) {
    /* Encode a packet with 3 channels, then hand decode() only the
     * first 30 bytes. The header says 3 channels, so we need at least
     * 22 + 54 = 76 bytes. 30 should fail. */
    sample_t s[3] = {
        mk_f32(1, 1.0f, 0), mk_f32(2, 2.0f, 0), mk_f32(3, 3.0f, 0),
    };
    packet_t p = mk_packet(1000, 1, s, 3);
    uint8_t buf[128];
    int n = packet_encode(&p, buf, sizeof(buf));

    packet_t d;
    CHECK(packet_decode(buf, 30, &d) == -1);
    /* Sanity: with full length it works. */
    CHECK(packet_decode(buf, (size_t)n, &d) == n);
}

/* ---- stream consumption ---------------------------------------------- */

static void test_two_packets_concatenated(void) {
    sample_t s1 = mk_f32(1, 1.0f, 0);
    sample_t s2 = mk_f32(2, 2.0f, 0);
    packet_t p1 = mk_packet(1000, 1, &s1, 1);
    packet_t p2 = mk_packet(2000, 1, &s2, 1);

    uint8_t buf[256];
    int n1 = packet_encode(&p1, buf, sizeof(buf));
    int n2 = packet_encode(&p2, buf + n1, sizeof(buf) - n1);
    CHECK(n1 > 0 && n2 > 0);

    packet_t d1, d2;
    int m1 = packet_decode(buf, (size_t)(n1 + n2), &d1);
    CHECK_EQ(m1, n1);
    int m2 = packet_decode(buf + m1, (size_t)(n1 + n2 - m1), &d2);
    CHECK_EQ(m2, n2);

    CHECK_EQ(d1.timestamp_ns, 1000);
    CHECK_EQ(d2.timestamp_ns, 2000);
}

/* ---- CRC vector ------------------------------------------------------- */

static void test_crc_known_vector(void) {
    /* CRC-16/MODBUS of "123456789" is 0x4B37. Standard check value. */
    const uint8_t input[] = "123456789";
    CHECK_EQ(packet_crc16(input, 9), 0x4B37);
}

int main(void) {
    test_encoded_size_formula();
    test_max_size_fits_in_PACKET_BIN_MAX();
    test_roundtrip_empty();
    test_roundtrip_one_f32();
    test_roundtrip_all_types();
    test_roundtrip_all_flags();
    test_encode_null_args();
    test_encode_too_many_channels();
    test_encode_cap_too_small();
    test_decode_null_args();
    test_decode_too_short();
    test_decode_bad_magic();
    test_decode_bad_version();
    test_decode_bad_crc();
    test_decode_channel_count_exceeds_max();
    test_decode_buffer_shorter_than_claimed();
    test_two_packets_concatenated();
    test_crc_known_vector();

    if (g_failures) {
        fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_packet_bin: all checks passed\n");
    return 0;
}