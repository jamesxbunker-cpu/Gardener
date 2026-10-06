#include "gardener/packet.h"
#include "gardener/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>
#include <stddef.h>


/* ---------------------------------------------------------------------
 * JSONL encoder.
 *
 * Output is compact (no whitespace) and stable. Numbers are written with
 * enough precision to round-trip a float: %g with 9 significant digits
 * is the shortest representation that does.
 *
 * Contract: packet_to_json returns the number of bytes the full encoding
 * requires, whether or not it fit in the caller's buffer. Callers detect
 * truncation by comparing the return value to cap. Returns -1 only on a
 * genuine formatting error.
 * ------------------------------------------------------------------- */

/* Append to a bounded buffer, always advancing `used` by the length the
 * format *would* have produced. Callers detect truncation by comparing
 * used to cap. Returns 0 on success, -1 on a genuine formatting error. */
static int append(char *buf, size_t cap, size_t *used, const char *fmt, ...) {
    va_list ap;

    if (buf && *used < cap) {
        va_start(ap, fmt);
        int n = vsnprintf(buf + *used, cap - *used, fmt, ap);
        va_end(ap);
        if (n < 0) return -1;
        *used += (size_t)n;
        return 0;
    }

    /* No room: measure the size we would have produced, but write
     * nothing. vsnprintf into a 1-byte scratch returns the length the
     * format needs. */
    va_start(ap, fmt);
    char scratch[1];
    int n = vsnprintf(scratch, sizeof(scratch), fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    *used += (size_t)n;
    return 0;
}

static void write_value(char *buf, size_t cap, size_t *used, const sample_t *s) {
    switch (s->type) {
        case CH_F32:
            append(buf, cap, used, "%g", (double)s->v.f);
            break;
        case CH_I32:
            append(buf, cap, used, "%d", s->v.i);
            break;
        case CH_U16:
            append(buf, cap, used, "%u", (unsigned)s->v.u);
            break;
        case CH_BOOL:
            append(buf, cap, used, "%s", s->v.b ? "true" : "false");
            break;
        case CH_ENUM:
            append(buf, cap, used, "%d", s->v.i);
            break;
        default:
            append(buf, cap, used, "null");
            break;
    }
}

int packet_to_json(const packet_t *p, char *buf, size_t cap) {
    if (!p || !buf || cap == 0) return -1;
    size_t used = 0;

    if (append(buf, cap, &used, "{\"t\":%llu,\"src\":%u,\"ch\":[",
               (unsigned long long)p->timestamp_ns, (unsigned)p->source_id) != 0)
        return -1;

    for (uint16_t i = 0; i < p->channel_count; i++) {
        const sample_t *s = &p->samples[i];
        if (i) append(buf, cap, &used, ",");
        append(buf, cap, &used, "{\"id\":%u", (unsigned)s->channel_id);
        if (s->role != ROLE_UNKNOWN)
            append(buf, cap, &used, ",\"role\":%u", (unsigned)s->role);
        if (s->type != CH_F32)
            append(buf, cap, &used, ",\"type\":\"%s\"", ch_type_name(s->type));
        if (s->flags) append(buf, cap, &used, ",\"flags\":%u", (unsigned)s->flags);
        append(buf, cap, &used, ",\"v\":");
        write_value(buf, cap, &used, s);
        append(buf, cap, &used, "}");
    }

    if (append(buf, cap, &used, "]}") != 0) return -1;

    /* `used` accumulated the full length even when truncated, so this
     * is the true needed size. Callers compare to cap to detect
     * truncation. */
    return (int)used;
}

/* ---------------------------------------------------------------------
 * JSONL parser.
 *
 * Walk-based, tolerant of whitespace and key order. Does not validate
 * that the input is well-formed JSON in general — only that it contains
 * the pieces we need, in a place where we can find them.
 *
 * The strategy:
 *   1. Find "t": and parse the uint64.
 *   2. Find "src": and parse the uint16 (optional).
 *   3. If "ch":[ is present, walk the array of objects.
 *      Otherwise if "v":{ is present, walk the shorthand map.
 *   4. Anything else is ignored.
 *
 * Failure modes: returns -1 if "t" is missing, or if neither "ch" nor
 * "v" is present, or if the array/map contains no valid entries.
 * ------------------------------------------------------------------- */

/* Skip whitespace in a const string. Returns pointer to first non-space. */
static const char *skip_ws(const char *p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

/* Find "key": starting at `start`, but do not look past the end of the
 * current object — i.e. stop at the first unquoted '}' or ']' that
 * closes the scope we started in. This is what keeps `find_key(q, "v")`
 * from picking up a `"v"` belonging to a *later* entry in the same
 * packet.
 *
 * Returns a pointer to the value's first non-whitespace character, or
 * NULL if the key is not present in this scope.
 *
 * Assumes input is well-formed enough for our purposes: strings are
 * double-quoted, backslash escapes are respected, and no `{`/`[` opens
 * without eventually closing. Keys inside embedded strings will not be
 * matched because we track string state. */
static const char *find_key(const char *start, const char *key) {
    char needle[32];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n <= 0 || (size_t)n >= sizeof(needle)) return NULL;

    const char *p = start;
    while ((p = strstr(p, needle)) != NULL) {
        /* Walk from `start` to `p` and check whether we cross out of
         * the current scope. If we do, the key is not in our scope. */
        int depth  = 0;
        int in_str = 0;
        for (const char *q = start; q < p; q++) {
            if (in_str) {
                if (*q == '\\' && q + 1 < p) { q++; continue; }
                if (*q == '"') in_str = 0;
                continue;
            }
            if (*q == '"') { in_str = 1; continue; }
            if (*q == '{' || *q == '[') depth++;
            else if (*q == '}' || *q == ']') {
                if (depth == 0) return NULL;
                depth--;
            }
        }
        if (in_str) { p++; continue; }   /* matched inside a string */

        /* Confirm the character after the needle is a colon, possibly
         * with whitespace. If not, this was a string that happened to
         * contain the quoted key. Advance past it. */
        const char *after = p + n;
        after = skip_ws(after);
        if (*after != ':') { p++; continue; }
        after++;
        return skip_ws(after);
    }
    return NULL;
}

/* Parse an unsigned integer at *p (which points at the first digit).
 * Advances *p past the number. Returns 0 on success, -1 if no digits. */
static int parse_u64(const char **p, uint64_t *out) {
    const char *s = *p;
    if (!isdigit((unsigned char)*s)) return -1;
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (end == s || errno == ERANGE) return -1;
    *out = (uint64_t)v;
    *p = end;
    return 0;
}

/* Parse a number (int or float) at *p. Advances *p. Returns 0 on
 * success, -1 on failure. */
static int parse_number(const char **p, double *out) {
    const char *s = *p;
    char *end = NULL;
    errno = 0;
    double v = strtod(s, &end);
    if (end == s || errno == ERANGE) return -1;
    *out = v;
    *p = end;
    return 0;
}

/* Parse a value: number, true, false, or "null". Advances *p. Writes
 * both the type tag and the union bytes consistently.
 *
 * If `hint` is 0xFF, the type is inferred: whole numbers become i32,
 * anything else f32, true/false bool. If `hint` is one of the concrete
 * CH_* tags, the value is stored as that type regardless of shape. */
static int parse_value(const char **p, sample_t *s, uint8_t hint) {
    const char *q = *p;

    if (hint == CH_BOOL) {
        if (!strncmp(q, "true", 4))  { s->type = CH_BOOL; s->v.b = 1; *p = q + 4; return 0; }
        if (!strncmp(q, "false", 5)) { s->type = CH_BOOL; s->v.b = 0; *p = q + 5; return 0; }
        double d;
        const char *qq = q;
        if (parse_number(&qq, &d) != 0) return -1;
        s->type = CH_BOOL;
        s->v.b = (d != 0.0) ? 1 : 0;
        *p = qq;
        return 0;
    }

    if (hint == CH_F32 || hint == CH_I32 || hint == CH_U16 || hint == CH_ENUM) {
        if (!strncmp(q, "null", 4)) {
            s->type = hint;
            memset(&s->v, 0, sizeof(s->v));
            *p = q + 4;
            return 0;
        }
        double d;
        if (parse_number(p, &d) != 0) return -1;
        s->type = hint;
        switch (hint) {
            case CH_F32:  s->v.f = (float)d;            break;
            case CH_I32:  s->v.i = (int32_t)d;          break;
            case CH_U16:  s->v.u = (uint16_t)d;         break;
            case CH_ENUM: s->v.i = (int32_t)d;          break;
            default: break;
        }
        return 0;
    }

    /* hint == 0xFF: infer from value shape. */
    if (!strncmp(q, "true", 4))  { s->type = CH_BOOL; s->v.b = 1; *p = q + 4; return 0; }
    if (!strncmp(q, "false", 5)) { s->type = CH_BOOL; s->v.b = 0; *p = q + 5; return 0; }
    if (!strncmp(q, "null", 4))  { s->type = CH_F32;  s->v.f = 0; *p = q + 4; return 0; }

    double d;
    if (parse_number(p, &d) != 0) return -1;

    if (d == (double)(int32_t)d && d >= -2147483648.0 && d < 2147483648.0) {
        s->type = CH_I32;
        s->v.i = (int32_t)d;
    } else {
        s->type = CH_F32;
        s->v.f = (float)d;
    }
    return 0;
}

/* Parse an "id": <uint16> pair at *p, advancing past the value.
 * Returns 0 on success, -1 if "id" is missing or malformed. */
static int parse_id(const char **p, uint16_t *out) {
    const char *v = find_key(*p, "id");
    if (!v) return -1;
    uint64_t id;
    if (parse_u64(&v, &id) != 0) return -1;
    if (id > 0xFFFF) return -1;
    *out = (uint16_t)id;
    *p = v;
    return 0;
}

/* Parse a "role": <uint8> pair at *p, advancing past the value.
 * Returns 0 on success, -1 if missing or malformed. */
static int parse_role_optional(const char **p, uint8_t *out) {
    const char *v = find_key(*p, "role");
    if (!v) return -1;
    uint64_t role;
    if (parse_u64(&v, &role) != 0) return -1;
    if (role > 255) return -1;
    *out = (uint8_t)role;
    *p = v;
    return 0;
}

/* Parse a "type": "f32"|"i32"|"u16"|"bool"|"enum" pair, advancing past
 * the value. Returns 0 on success, -1 if missing or unrecognized. */
static int parse_type_optional(const char **p, uint8_t *out) {
    const char *v = find_key(*p, "type");
    if (!v || *v != '"') return -1;
    v++;
    const char *end = strchr(v, '"');
    if (!end) return -1;
    size_t n = (size_t)(end - v);

    uint8_t t = 0xFF;
    if      (n == 3 && !strncmp(v, "f32",  3)) t = CH_F32;
    else if (n == 3 && !strncmp(v, "i32",  3)) t = CH_I32;
    else if (n == 3 && !strncmp(v, "u16",  3)) t = CH_U16;
    else if (n == 4 && !strncmp(v, "bool", 4)) t = CH_BOOL;
    else if (n == 4 && !strncmp(v, "enum", 4)) t = CH_ENUM;

    if (t == 0xFF) return -1;
    *out = t;
    *p = end + 1;
    return 0;
}

/* Parse one {"id":..,"role":..,"type":..,"v":..} object into a sample.
 * Advances *p past the closing brace. Returns 0 on success, -1 on
 * failure. */
static int parse_ch_entry(const char **p, sample_t *s) {
    const char *q = skip_ws(*p);
    if (*q != '{') return -1;
    q++;

    memset(s, 0, sizeof(*s));
    s->role  = ROLE_UNKNOWN;
    s->type  = CH_F32;
    s->flags = CH_FLAG_VALID;

    const char *after_id = q;
    if (parse_id(&after_id, &s->channel_id) != 0) return -1;

    uint8_t r;
    if (parse_role_optional(&after_id, &r) == 0) s->role = r;

    uint8_t hint = 0xFF;
    if (parse_type_optional(&after_id, &hint) != 0) hint = 0xFF;

    const char *vpos = find_key(q, "v");
    if (!vpos) return -1;
    if (parse_value(&vpos, s, hint) != 0) return -1;

    const char *close = strchr(vpos, '}');
    if (!close) return -1;
    *p = close + 1;
    return 0;
}

/* Parse one "id":value pair from the shorthand "v" map. Advances *p
 * past the value (not past any comma). */
static int parse_v_pair(const char **p, sample_t *s) {
    const char *q = *p;
    if (*q != '"') return -1;
    q++;
    const char *id_end = strchr(q, '"');
    if (!id_end || id_end == q) return -1;

    char idbuf[8];
    size_t idlen = (size_t)(id_end - q);
    if (idlen >= sizeof(idbuf)) return -1;
    memcpy(idbuf, q, idlen);
    idbuf[idlen] = 0;

    char *end = NULL;
    long id = strtol(idbuf, &end, 10);
    if (end == idbuf || *end != 0 || id < 0 || id > 0xFFFF) return -1;

    q = skip_ws(id_end + 1);
    if (*q != ':') return -1;
    q++;
    q = skip_ws(q);

    memset(s, 0, sizeof(*s));
    s->channel_id = (uint16_t)id;
    s->role       = ROLE_UNKNOWN;
    s->type       = CH_F32;
    s->flags      = CH_FLAG_VALID;

    if (parse_value(&q, s, 0xFF) != 0) return -1;
    *p = q;
    return 0;
}

int packet_from_json(const char *line, packet_t *out) {
    if (!line || !out) return -1;
    memset(out, 0, sizeof(*out));

    /* t (required) — searched from the top of the line. */
    const char *p = find_key(line, "t");
    if (!p) return -1;
    if (parse_u64(&p, &out->timestamp_ns) != 0) return -1;

    /* src (optional) — also from the top. */
    const char *sp = find_key(line, "src");
    if (sp) {
        uint64_t v;
        if (parse_u64(&sp, &v) == 0 && v <= 0xFFFF) {
            out->source_id = (uint16_t)v;
        }
    }

    /* ch array (canonical). The find_key here is called on the whole
     * line, so it searches to the end. That's fine: "ch" appears once
     * at the top level of a packet. */
    const char *ch = find_key(line, "ch");
    if (ch && *ch == '[') {
        const char *q = ch + 1;
        uint16_t count = 0;
        while (*q) {
            q = skip_ws(q);
            if (*q == ']' || *q == 0) break;
            if (*q == ',') { q++; continue; }
            if (*q != '{') break;
            if (count >= PACKET_MAX_CH) {
                plat_log(LOG_WARN, "packet_from_json: too many channels");
                return -1;
            }
            if (parse_ch_entry(&q, &out->samples[count]) != 0) {
                plat_log(LOG_WARN, "packet_from_json: bad entry at %u", count);
                return -1;
            }
            count++;
        }
        if (count == 0) return -1;
        out->channel_count = count;
        return 0;
    }

    /* v map (shorthand). Same reasoning as "ch". */
    const char *v = find_key(line, "v");
    if (v && *v == '{') {
        const char *q = v + 1;
        uint16_t count = 0;
        while (*q) {
            q = skip_ws(q);
            if (*q == '}' || *q == 0) break;
            if (*q == ',') { q++; continue; }
            if (*q != '"') break;
            if (count >= PACKET_MAX_CH) {
                plat_log(LOG_WARN, "packet_from_json: too many channels in v map");
                return -1;
            }
            if (parse_v_pair(&q, &out->samples[count]) != 0) {
                plat_log(LOG_WARN, "packet_from_json: bad v pair at %u", count);
                return -1;
            }
            count++;
        }
        if (count == 0) return -1;
        out->channel_count = count;
        return 0;
    }

    return -1;
}

/* ---------------------------------------------------------------------
 * Binary codec.
 *
 * Little-endian, packed, CRC-16/ARC. See packet.h for the format spec.
 * The wire format is deliberately not the same as the in-memory struct:
 * sample_t is 24 bytes natural-aligned, packed is 18. This lets the
 * struct stay fast to access in memory while keeping the wire compact.
 * ------------------------------------------------------------------- */
/* CRC-16/MODBUS. Polynomial 0x8005 reflected as 0xA001, init 0xFFFF,
 * refin=true, refout=true, xorout=0x0000. Check value of "123456789"
 * is 0x4B37. Same CRC as Modbus RTU, which is why we use it. */
uint16_t packet_crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 1) crc = (uint16_t)((crc >> 1) ^ 0xA001);
            else         crc = (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

/* Little-endian writers. We write byte by byte rather than memcpy from
 * a struct so the byte order is explicit and does not depend on host
 * endianness or alignment. */
static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void put_u64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
}

static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint64_t get_u64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= ((uint64_t)p[i]) << (8 * i);
    return v;
}

int packet_encode(const packet_t *p, uint8_t *buf, size_t cap) {
    if (!p || !buf) return -1;
    if (p->channel_count > PACKET_MAX_CH) return -1;

    size_t need = PACKET_BIN_HEADER
                + (size_t)p->channel_count * PACKET_BIN_SAMPLE
                + PACKET_BIN_TRAILER;
    if (cap < need) return -1;

    uint8_t *o = buf;

    put_u32(o, PACKET_MAGIC);           o += 4;
    put_u16(o, PACKET_VERSION);         o += 2;
    put_u16(o, p->source_id);           o += 2;
    put_u64(o, p->timestamp_ns);        o += 8;
    put_u16(o, p->channel_count);       o += 2;
    put_u16(o, p->flags);               o += 2;

    for (uint16_t i = 0; i < p->channel_count; i++) {
        const sample_t *s = &p->samples[i];
        put_u16(o, s->channel_id);      o += 2;
        *o++ = s->type;
        *o++ = s->role;
        *o++ = s->flags;
        *o++ = 0;                       /* _pad, always zero */

        /* Value: 4 bytes, using the union member selected by type.
         * Copy the raw bits so a bool isn't sign-extended and an f32
         * isn't converted. */
        uint32_t bits = 0;
        memcpy(&bits, &s->v, 4);
        put_u32(o, bits);               o += 4;

        put_u64(o, s->timestamp_ns);    o += 8;
    }

    /* CRC covers everything before the trailer. */
    uint16_t crc = packet_crc16(buf, (size_t)(o - buf));
    put_u16(o, crc);                    o += 2;

    return (int)(o - buf);
}

int packet_decode(const uint8_t *buf, size_t len, packet_t *out) {
    if (!buf || !out) return -1;
    if (len < PACKET_BIN_HEADER + PACKET_BIN_TRAILER) return -1;

    const uint8_t *i = buf;

    uint32_t magic = get_u32(i);        i += 4;
    if (magic != PACKET_MAGIC) return -1;

    uint16_t version = get_u16(i);      i += 2;
    if (version != PACKET_VERSION) return -1;

    uint16_t source_id = get_u16(i);    i += 2;
    uint64_t ts        = get_u64(i);    i += 8;
    uint16_t ch_n      = get_u16(i);    i += 2;
    uint16_t flags     = get_u16(i);    i += 2;

    if (ch_n > PACKET_MAX_CH) return -1;

    size_t need = PACKET_BIN_HEADER
                + (size_t)ch_n * PACKET_BIN_SAMPLE
                + PACKET_BIN_TRAILER;
    if (len < need) return -1;

    /* Verify CRC before trusting any of the payload. */
    uint16_t crc_stored = get_u16(buf + need - 2);
    uint16_t crc_calc   = packet_crc16(buf, need - 2);
    if (crc_stored != crc_calc) return -1;

    memset(out, 0, sizeof(*out));
    out->timestamp_ns  = ts;
    out->source_id     = source_id;
    out->flags         = flags;
    out->channel_count = ch_n;

    for (uint16_t k = 0; k < ch_n; k++) {
        sample_t *s = &out->samples[k];
        s->channel_id = get_u16(i);     i += 2;
        s->type       = *i++;
        s->role       = *i++;
        s->flags      = *i++;
        i++;                            /* _pad, ignored */

        uint32_t bits = get_u32(i);     i += 4;
        memcpy(&s->v, &bits, 4);

        s->timestamp_ns = get_u64(i);   i += 8;
    }

    /* `i` now points at the CRC. We've already verified it above.
     * Return the total packet size including the trailer, so a caller
     * can advance past it in a concatenated stream. */
    return (int)need;
}