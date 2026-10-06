#include "gardener/packet.h"
#include "gardener/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

/* ---------------------------------------------------------------------
 * JSONL encoder.
 *
 * Output is compact (no whitespace) and stable. Numbers are written with
 * enough precision to round-trip a float: %g with 9 significant digits
 * is the shortest representation that does.
 * ------------------------------------------------------------------- */

static int append(char *buf, size_t cap, size_t *used, const char *fmt, ...) {
    if (*used >= cap) return -1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *used, cap - *used, fmt, ap);
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

    /* vsnprintf returns the length it *would* have written, so `used`
     * may exceed cap. Report the true length so the caller can size up. */
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

/* Find "key": starting at *p, advance *p past the value, return pointer
 * to the value's first character. Returns NULL if the key is not found
 * before the end of the string. */
static const char *find_key(const char *start, const char *key) {
    char needle[32];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n <= 0 || (size_t)n >= sizeof(needle)) return NULL;
    const char *p = strstr(start, needle);
    if (!p) return NULL;
    p += (size_t)n;
    p = skip_ws(p);
    if (*p != ':') return NULL;
    p++;
    return skip_ws(p);
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
 * the type and value to the sample. */
static int parse_value(const char **p, sample_t *s) {
    const char *q = *p;
    if (!strncmp(q, "true", 4))  { s->type = CH_BOOL; s->v.b = 1; *p = q + 4; return 0; }
    if (!strncmp(q, "false", 5)) { s->type = CH_BOOL; s->v.b = 0; *p = q + 5; return 0; }
    if (!strncmp(q, "null", 4))  { s->type = CH_F32; s->v.f = 0; *p = q + 4; return 0; }

    double d;
    if (parse_number(p, &d) != 0) return -1;

    /* If the value has no fractional part and fits in int32, store as
     * i32 so integer channels round-trip cleanly. Otherwise f32. This
     * matches what the shorthand `v` map wants and does no harm to the
     * canonical shape, where the caller-set type wins anyway. */
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
 * Returns 0 on success, -1 if missing or malformed. Missing role is not
 * an error in the caller; this just reports whether one was found. */
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
    if (n == 3 && !strncmp(v, "f32", 3))       t = CH_F32;
    else if (n == 3 && !strncmp(v, "i32", 3))  t = CH_I32;
    else if (n == 3 && !strncmp(v, "u16", 3))  t = CH_U16;
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

    uint8_t explicit_type = 0xFF;
    if (parse_type_optional(&after_id, &explicit_type) == 0) s->type = explicit_type;

    const char *vpos = find_key(q, "v");
    if (!vpos) return -1;
    if (parse_value(&vpos, s) != 0) return -1;
    if (explicit_type != 0xFF) s->type = explicit_type;

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

    if (parse_value(&q, s) != 0) return -1;
    *p = q;
    return 0;
}

int packet_from_json(const char *line, packet_t *out) {
    if (!line || !out) return -1;
    memset(out, 0, sizeof(*out));

    /* t (required) */
    const char *p = find_key(line, "t");
    if (!p) return -1;
    if (parse_u64(&p, &out->timestamp_ns) != 0) return -1;

    /* src (optional) */
    const char *sp = find_key(line, "src");
    if (sp) {
        uint64_t v;
        if (parse_u64(&sp, &v) == 0 && v <= 0xFFFF) {
            out->source_id = (uint16_t)v;
        }
    }

    /* ch array (canonical) */
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

    /* v map (shorthand) */
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