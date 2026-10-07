#include "gardener/log_reader.h"
#include "gardener/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---------------------------------------------------------------------
 * Log reader.
 *
 * Reads records from a file written by log_writer. Records are
 * variable-size packets (22 + 18*N bytes). Iteration state is
 * (current_offset, more_to_read), where current_offset walks from
 * data_start toward write_offset, wrapping to data_start when the
 * file wraps.
 *
 * The reader caches the header. It does not re-read it after open, so
 * if the log is being appended to concurrently, callers should
 * reopen to see new data. Concurrency is not supported.
 * ------------------------------------------------------------------- */

struct log_reader {
    plat_file_t  *file;
    char          path[256];
    log_header_t  header;
    registry_t    registry;

    /* Index for time-range queries. Loaded if available; NULL if the
     * index file is missing or unusable. */
    log_index_entry_t *index;
    size_t             index_count;

    /* Iteration state. */
    uint64_t cur_offset;    /* position of the *next* record to read */
    bool     at_end;        /* true when cur_offset has caught up to
                             * the logical end */
    bool     started;       /* false until the first next()/prev() */
};

/* ------------------------------------------------------------------ */
/* Index loading                                                      */
/* ------------------------------------------------------------------ */

static void load_index_optional(log_reader_t *r) {
    /* Derive index path from log path. */
    char idx_path[260];
    size_t n = strlen(r->path);
    if (n >= 4 && strcmp(r->path + n - 4, ".bin") == 0) {
        if (n + 1 > sizeof(idx_path)) return;
        memcpy(idx_path, r->path, n - 4);
        memcpy(idx_path + n - 4, ".idx", 5);
    } else {
        snprintf(idx_path, sizeof(idx_path), "%s.idx", r->path);
    }

    plat_file_t *f = plat_file_open(idx_path, PLAT_FILE_READ);
    if (!f) return;

    log_index_header_t idx;
    if (plat_file_read(f, &idx, sizeof(idx)) != (int)sizeof(idx)) {
        plat_file_close(f);
        return;
    }
    if (idx.magic != LOG_INDEX_MAGIC ||
        idx.version != LOG_INDEX_VERSION ||
        idx.entry_size != LOG_INDEX_ENTRY ||
        idx.interval != LOG_INDEX_INTERVAL ||
        idx.count == 0) {
        plat_file_close(f);
        return;
    }

    /* Sanity cap. */
    if (idx.count > 1024 * 1024) {
        plat_file_close(f);
        return;
    }

    size_t bytes = (size_t)idx.count * sizeof(log_index_entry_t);
    log_index_entry_t *entries = (log_index_entry_t *)malloc(bytes);
    if (!entries) {
        plat_file_close(f);
        return;
    }
    if (plat_file_read(f, entries, bytes) != (int)bytes) {
        free(entries);
        plat_file_close(f);
        return;
    }
    plat_file_close(f);

    r->index = entries;
    r->index_count = (size_t)idx.count;
}

/* ------------------------------------------------------------------ */
/* Registry reconstruction                                            */
/* ------------------------------------------------------------------ */

/* Parse one JSONL descriptor line from the registry snapshot. Reuses
 * the packet JSON parser's find-key approach, but on a single line
 * whose shape is fixed. Returns 0 on success. */
static int parse_descriptor_line(registry_t *reg, const char *line) {
    if (reg->count >= REGISTRY_MAX_CHANNELS) return -1;

    channel_desc_t *c = &reg->entries[reg->count];
    memset(c, 0, sizeof(*c));
    c->type = CH_F32;
    c->role = ROLE_UNKNOWN;
    c->min_valid = 0.0f;
    c->max_valid = 0.0f;

    /* Find "id":N */
    const char *p = strstr(line, "\"id\":");
    if (!p) return -1;
    c->id = (uint16_t)strtoul(p + 5, NULL, 10);

    /* name */
    p = strstr(line, "\"name\":\"");
    if (p) {
        p += 8;
        const char *end = strchr(p, '"');
        if (end) {
            size_t len = (size_t)(end - p);
            if (reg->arena_used + len + 1 <= sizeof(reg->arena)) {
                char *dst = reg->arena + reg->arena_used;
                memcpy(dst, p, len);
                dst[len] = 0;
                reg->arena_used += len + 1;
                c->name = dst;
            }
        }
    }

    /* unit */
    p = strstr(line, "\"unit\":\"");
    if (p) {
        p += 8;
        const char *end = strchr(p, '"');
        if (end) {
            size_t len = (size_t)(end - p);
            if (reg->arena_used + len + 1 <= sizeof(reg->arena)) {
                char *dst = reg->arena + reg->arena_used;
                memcpy(dst, p, len);
                dst[len] = 0;
                reg->arena_used += len + 1;
                c->unit = dst;
            }
        }
    }

    /* type */
    p = strstr(line, "\"type\":\"");
    if (p) {
        p += 8;
        if (!strncmp(p, "f32\"", 4))       c->type = CH_F32;
        else if (!strncmp(p, "i32\"", 4))  c->type = CH_I32;
        else if (!strncmp(p, "u16\"", 4))  c->type = CH_U16;
        else if (!strncmp(p, "bool\"", 5)) c->type = CH_BOOL;
        else if (!strncmp(p, "enum\"", 5)) c->type = CH_ENUM;
    }

    /* role */
    p = strstr(line, "\"role\":\"");
    if (p) {
        p += 8;
        if      (!strncmp(p, "sensor\"",   7)) c->role = ROLE_SENSOR;
        else if (!strncmp(p, "setpoint\"", 9)) c->role = ROLE_SETPOINT;
        else if (!strncmp(p, "ctrl_out\"", 9)) c->role = ROLE_CTRL_OUT;
        else if (!strncmp(p, "actuator\"", 9)) c->role = ROLE_ACTUATOR;
        else if (!strncmp(p, "status\"",   7)) c->role = ROLE_STATUS;
        else if (!strncmp(p, "counter\"",  8)) c->role = ROLE_COUNTER;
    }

    /* controllable, is_regime, has_range */
    p = strstr(line, "\"controllable\":");
    if (p && !strncmp(p + 15, "true", 4)) c->controllable = true;
    p = strstr(line, "\"is_regime\":");
    if (p && !strncmp(p + 12, "true", 4)) c->is_regime = true;
    p = strstr(line, "\"has_range\":");
    if (p && !strncmp(p + 12, "true", 4)) c->has_range = true;

    /* min, max */
    p = strstr(line, "\"min\":");
    if (p) c->min_valid = strtof(p + 6, NULL);
    p = strstr(line, "\"max\":");
    if (p) c->max_valid = strtof(p + 6, NULL);

    reg->count++;
    return 0;
}

static void load_registry(log_reader_t *r, plat_file_t *f) {
    registry_init(&r->registry);
    if (r->header.registry_size == 0) return;

    /* Read the whole snapshot into a buffer. It's small (a few KB). */
    size_t size = r->header.registry_size;
    if (size > 1024 * 1024) {
        plat_log(LOG_WARN,
                 "log_reader: registry snapshot too large (%zu), skipping",
                 size);
        return;
    }
    char *buf = (char *)malloc(size + 1);
    if (!buf) return;
    if (plat_file_seek(f, LOG_HEADER_SIZE) != 0) {
        free(buf);
        return;
    }
    if (plat_file_read(f, buf, size) != (int)size) {
        free(buf);
        return;
    }
    buf[size] = 0;

    /* Parse line by line. */
    char *p = buf;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (!nl) break;
        *nl = 0;
        if (*p) parse_descriptor_line(&r->registry, p);
        p = nl + 1;
    }

    free(buf);
}

/* ------------------------------------------------------------------ */
/* Open / close                                                       */
/* ------------------------------------------------------------------ */

log_reader_t *log_reader_open(const char *path) {
    if (!path) return NULL;

    log_reader_t *r = (log_reader_t *)calloc(1, sizeof(*r));
    if (!r) return NULL;
    snprintf(r->path, sizeof(r->path), "%s", path);

    r->file = plat_file_open(path, PLAT_FILE_READ);
    if (!r->file) {
        plat_log(LOG_ERROR, "log_reader: cannot open %s", path);
        free(r);
        return NULL;
    }

    if (plat_file_read(r->file, &r->header, sizeof(r->header))
            != (int)sizeof(r->header)) {
        plat_log(LOG_ERROR, "log_reader: header read failed");
        plat_file_close(r->file);
        free(r);
        return NULL;
    }

    if (r->header.magic != LOG_MAGIC) {
        plat_log(LOG_ERROR, "log_reader: bad magic in %s", path);
        plat_file_close(r->file);
        free(r);
        return NULL;
    }
    if (r->header.version != LOG_VERSION) {
        plat_log(LOG_ERROR, "log_reader: unsupported version %u",
                 (unsigned)r->header.version);
        plat_file_close(r->file);
        free(r);
        return NULL;
    }
    if (r->header.header_size != LOG_HEADER_SIZE) {
        plat_log(LOG_ERROR, "log_reader: bad header_size %u",
                 (unsigned)r->header.header_size);
        plat_file_close(r->file);
        free(r);
        return NULL;
    }

    /* Verify header CRC. */
    log_header_t tmp = r->header;
    tmp.header_crc = 0;
    uint16_t calc = packet_crc16((const uint8_t *)&tmp, 80);
    if (calc != (uint16_t)r->header.header_crc) {
        plat_log(LOG_ERROR, "log_reader: header CRC mismatch");
        plat_file_close(r->file);
        free(r);
        return NULL;
    }

    load_registry(r, r->file);
    load_index_optional(r);

    log_reader_rewind(r);

    plat_log(LOG_INFO,
             "log_reader: opened %s (records=%llu, wraps=%u, channels=%zu)",
             path,
             (unsigned long long)r->header.record_count,
             (unsigned)r->header.wrap_count,
             r->registry.count);
    return r;
}

void log_reader_close(log_reader_t *r) {
    if (!r) return;
    if (r->file) plat_file_close(r->file);
    free(r->index);
    free(r);
}

/* ------------------------------------------------------------------ */
/* Iteration                                                          */
/* ------------------------------------------------------------------ */

/* Logical iteration: records are ordered oldest-first. If no wrap has
 * occurred, records go from data_start to write_offset. If a wrap has
 * occurred, records go from write_offset (the oldest surviving) to
 * file_size, then from data_start to write_offset. */

int log_reader_rewind(log_reader_t *r) {
    if (!r) return -1;
    if (r->header.wrap_count == 0) {
        /* No wrap: oldest is at data_start. */
        r->cur_offset = r->header.data_start;
    } else {
        /* Wrapped: oldest is at write_offset (the record we're about
         * to overwrite next time). */
        r->cur_offset = r->header.write_offset;
    }
    r->at_end = false;
    r->started = false;
    return 0;
}

/* Read the record at cur_offset into *out. Advances cur_offset to the
 * next logical record. Returns 1 on success, 0 on end, -1 on error. */
static int read_and_advance(log_reader_t *r, packet_t *out) {
    /* Has the reader caught up to the logical end? */
    uint64_t end = r->header.write_offset;
    if (r->header.record_count == 0) {
        /* No records. */
        r->at_end = true;
        return 0;
    }
    if (r->started && r->cur_offset == end) {
        r->at_end = true;
        return 0;
    }

    /* If we're at file_size, wrap to data_start. */
    if (r->cur_offset >= r->header.file_size) {
        r->cur_offset = r->header.data_start;
    }

    /* Read the fixed 22-byte prefix to learn channel_count. */
    uint8_t hdr[22];
    if (plat_file_seek(r->file, r->cur_offset) != 0) return -1;
    if (plat_file_read(r->file, hdr, sizeof(hdr)) != (int)sizeof(hdr))
        return -1;

    uint32_t magic = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8)
                   | ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
    if (magic != PACKET_MAGIC) {
        plat_log(LOG_WARN,
                 "log_reader: bad record magic at offset %llu",
                 (unsigned long long)r->cur_offset);
        return -1;
    }

    uint16_t ch_n = (uint16_t)hdr[16] | ((uint16_t)hdr[17] << 8);
    if (ch_n > PACKET_MAX_CH) {
        plat_log(LOG_WARN,
                 "log_reader: bad channel_count %u at offset %llu",
                 (unsigned)ch_n, (unsigned long long)r->cur_offset);
        return -1;
    }

    uint64_t record_size = 22 + 18ull * ch_n;

    /* Read the whole record. */
    uint8_t buf[PACKET_BIN_MAX];
    if (record_size > sizeof(buf)) return -1;
    if (plat_file_seek(r->file, r->cur_offset) != 0) return -1;
    if (plat_file_read(r->file, buf, (size_t)record_size)
            != (int)record_size) return -1;

    int consumed = packet_decode(buf, (size_t)record_size, out);
    if (consumed < 0) {
        plat_log(LOG_WARN,
                 "log_reader: packet decode failed at offset %llu",
                 (unsigned long long)r->cur_offset);
        return -1;
    }

    r->cur_offset += record_size;
    r->started = true;
    return 1;
}

int log_reader_next(log_reader_t *r, packet_t *out) {
    if (!r || !out) return -1;
    if (r->at_end) return 0;
    return read_and_advance(r, out);
}

int log_reader_prev(log_reader_t *r, packet_t *out) {
    if (!r || !out) return -1;

    /* Reverse iteration is not implemented yet. It requires knowing
     * the previous record's offset, which is not stored. Two options:
     * scan forward and remember the last two offsets, or maintain a
     * small history. Neither is needed by any current caller; the
     * reader supports forward iteration and time-range queries, which
     * cover the current use cases.
     *
     * Returning -1 rather than pretending to work. When a caller needs
     * it, implement via "scan from rewind, remember last N offsets" or
     * "index tells us the block; scan forward within the block and
     * return the record before the target". */
    plat_log(LOG_WARN, "log_reader_prev: not implemented");
    return -1;
}

/* ------------------------------------------------------------------ */
/* Time-range seek                                                    */
/* ------------------------------------------------------------------ */

int log_reader_seek_ns(log_reader_t *r, uint64_t ts) {
    if (!r) return -1;

    /* Fast path: use the index if we have one. Find the last index
     * entry with timestamp <= ts, seek to that offset, then scan
     * forward. */
    if (r->index && r->index_count > 0) {
        /* Binary search for the greatest index entry with ts <= target. */
        size_t lo = 0, hi = r->index_count;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (r->index[mid].timestamp_ns <= ts) lo = mid + 1;
            else                                   hi = mid;
        }
        /* lo is the first entry with timestamp > ts; lo - 1 is the
         * last entry with timestamp <= ts, if lo > 0. */
        if (lo > 0) {
            size_t k = lo - 1;
            uint64_t start = r->index[k].file_offset;
            /* Sanity: the offset must be within the payload area. */
            if (start >= r->header.data_start &&
                start <  r->header.file_size) {
                r->cur_offset = start;
                r->at_end = false;
                r->started = true;

                /* Scan forward until we reach a record with ts >= target. */
                packet_t p;
                for (;;) {
                    uint64_t prev = r->cur_offset;
                    int rc = read_and_advance(r, &p);
                    if (rc <= 0) {
                        /* End of log. Restore position to end. */
                        return 0;
                    }
                    if (p.timestamp_ns >= ts) {
                        /* Back up so the next log_reader_next returns
                         * this record. We know its size. */
                        r->cur_offset = prev;
                        r->at_end = false;
                        return 0;
                    }
                }
            }
        }
    }

    /* Fallback: linear scan from the logical start. */
    log_reader_rewind(r);
    packet_t p;
    for (;;) {
        uint64_t prev = r->cur_offset;
        int rc = log_reader_next(r, &p);
        if (rc <= 0) return rc < 0 ? -1 : 0;
        if (p.timestamp_ns >= ts) {
            r->cur_offset = prev;
            r->at_end = false;
            return 0;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Introspection                                                      */
/* ------------------------------------------------------------------ */

uint64_t log_reader_total_records(const log_reader_t *r) {
    return r ? r->header.record_count : 0;
}

uint32_t log_reader_wrap_count(const log_reader_t *r) {
    return r ? r->header.wrap_count : 0;
}

const log_header_t *log_reader_header(const log_reader_t *r) {
    return r ? &r->header : NULL;
}

const registry_t *log_reader_registry(const log_reader_t *r) {
    return r ? &r->registry : NULL;
}

const char *log_reader_path(const log_reader_t *r) {
    return r ? r->path : NULL;
}