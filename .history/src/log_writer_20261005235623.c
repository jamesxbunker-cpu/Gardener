#include "gardener/log_writer.h"
#include "gardener/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------
 * Implementation notes.
 *
 * The writer uses the platform file API (plat_file_*) for everything,
 * so the only OS calls are in platform_posix.c.
 *
 * The file is preallocated by writing one byte at the max offset. This
 * is not guaranteed to allocate disk space on every filesystem, but on
 * ext4 and NTFS it does. If the underlying filesystem is sparse, the
 * file grows on demand and only uses what it needs, which is also fine.
 * ------------------------------------------------------------------- */

struct log_writer {
    plat_file_t *file;
    plat_file_t *index_file;
    char         path[256];
    char         index_path[260];

    log_header_t header;
    uint64_t     fsync_at_ns;
    uint64_t     rewrite_at_ns;

    log_index_entry_t *index_entries;
    size_t             index_count;
    size_t             index_cap;
};

/* ------------------------------------------------------------------ */
/* Path derivation                                                    */
/* ------------------------------------------------------------------ */

static void derive_index_path(const char *log_path, char *out, size_t cap) {
    size_t n = strlen(log_path);
    if (n >= 4 && strcmp(log_path + n - 4, ".bin") == 0) {
        if (n + 1 > cap) { out[0] = 0; return; }
        memcpy(out, log_path, n - 4);
        memcpy(out + n - 4, ".idx", 5);
    } else {
        snprintf(out, cap, "%s.idx", log_path);
    }
}

/* ------------------------------------------------------------------ */
/* Registry serialization                                             */
/* ------------------------------------------------------------------ */

static int serialize_registry(const registry_t *reg, uint8_t *buf, size_t cap) {
    if (!reg || reg->count == 0) return 0;

    size_t used = 0;
    for (size_t i = 0; i < reg->count; i++) {
        const channel_desc_t *c = &reg->entries[i];
        int n = snprintf((char *)buf + used, cap - used,
            "{\"id\":%u,\"name\":\"%s\",\"unit\":\"%s\","
            "\"type\":\"%s\",\"role\":\"%s\","
            "\"controllable\":%s,\"is_regime\":%s,"
            "\"has_range\":%s,\"min\":%g,\"max\":%g}\n",
            (unsigned)c->id,
            c->name ? c->name : "",
            c->unit ? c->unit : "",
            ch_type_name(c->type),
            ch_role_name(c->role),
            c->controllable ? "true" : "false",
            c->is_regime    ? "true" : "false",
            c->has_range    ? "true" : "false",
            (double)c->min_valid, (double)c->max_valid);
        if (n < 0 || used + (size_t)n >= cap) return -1;
        used += (size_t)n;
    }
    return (int)used;
}

/* ------------------------------------------------------------------ */
/* Header I/O                                                         */
/* ------------------------------------------------------------------ */

static int read_header(plat_file_t *f, log_header_t *out) {
    if (plat_file_seek(f, 0) != 0) return -1;
    if (plat_file_read(f, out, sizeof(*out)) != (int)sizeof(*out)) return -1;
    if (out->magic != LOG_MAGIC) return -1;
    if (out->version != LOG_VERSION) return -1;
    if (out->header_size != LOG_HEADER_SIZE) return -1;

    uint32_t stored = out->header_crc;
    out->header_crc = 0;
    uint16_t calc = packet_crc16((const uint8_t *)out, 80);
    out->header_crc = stored;
    if (calc != (uint16_t)stored) return -1;

    return 0;
}

static int write_header(log_writer_t *w) {
    w->header.header_crc = 0;
    uint16_t crc = packet_crc16((const uint8_t *)&w->header, 80);
    w->header.header_crc = (uint32_t)crc;
    w->header.last_wall_ns = plat_wall_ns();

    if (plat_file_seek(w->file, 0) != 0) return -1;
    if (plat_file_write(w->file, &w->header, sizeof(w->header))
            != (int)sizeof(w->header)) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Create / open                                                      */
/* ------------------------------------------------------------------ */

static int create_log(log_writer_t *w, const registry_t *registry,
                      uint64_t max_bytes) {
    memset(&w->header, 0, sizeof(w->header));
    w->header.magic         = LOG_MAGIC;
    w->header.version       = LOG_VERSION;
    w->header.header_size   = LOG_HEADER_SIZE;
    w->header.file_size     = max_bytes;
    w->header.start_wall_ns = plat_wall_ns();

    /* Serialize the registry. 1 MB scratch is generous: even 256
     * channels with full-length names produce under 64 KB. */
    static uint8_t regbuf[1024 * 1024];
    int reg_size = serialize_registry(registry, regbuf, sizeof(regbuf));
    if (reg_size < 0) {
        plat_log(LOG_ERROR, "log_writer: registry snapshot too large");
        return -1;
    }
    w->header.registry_size = (uint32_t)reg_size;
    w->header.channel_count = registry ? (uint32_t)registry->count : 0;
    w->header.data_start    = LOG_HEADER_SIZE + (uint64_t)reg_size;
    w->header.write_offset  = w->header.data_start;

    if (write_header(w) != 0) return -1;

    if (reg_size > 0) {
        if (plat_file_seek(w->file, LOG_HEADER_SIZE) != 0) return -1;
        if (plat_file_write(w->file, regbuf, (size_t)reg_size) != reg_size)
            return -1;
    }

    /* Preallocate: seek to max_bytes - 1 and write one zero byte. */
    if (plat_file_seek(w->file, max_bytes - 1) != 0) return -1;
    uint8_t zero = 0;
    if (plat_file_write(w->file, &zero, 1) != 1) return -1;

    /* Empty index. */
    return 0;
}

static int load_index(log_writer_t *w) {
    if (plat_file_seek(w->index_file, 0) != 0) return -1;
    log_index_header_t idx;
    if (plat_file_read(w->index_file, &idx, sizeof(idx)) != (int)sizeof(idx))
        return -1;
    if (idx.magic != LOG_INDEX_MAGIC) return -1;
    if (idx.version != LOG_INDEX_VERSION) return -1;
    if (idx.entry_size != LOG_INDEX_ENTRY) return -1;
    if (idx.interval != LOG_INDEX_INTERVAL) return -1;

    uint64_t max_entries = LOG_DEFAULT_MAX / (LOG_INDEX_INTERVAL * 22);
    if (idx.count > max_entries) return -1;

    if (idx.count == 0) {
        w->index_count = 0;
        return 0;
    }

    w->index_entries = (log_index_entry_t *)malloc(
        (size_t)idx.count * sizeof(log_index_entry_t));
    if (!w->index_entries) return -1;
    w->index_cap = (size_t)idx.count;

    size_t want = (size_t)idx.count * sizeof(log_index_entry_t);
    if (plat_file_read(w->index_file, w->index_entries, want)
            != (int)want) {
        free(w->index_entries);
        w->index_entries = NULL;
        w->index_cap = 0;
        w->index_count = 0;
        return -1;
    }
    w->index_count = (size_t)idx.count;
    return 0;
}

static int save_index(log_writer_t *w) {
    log_index_header_t idx;
    memset(&idx, 0, sizeof(idx));
    idx.magic      = LOG_INDEX_MAGIC;
    idx.version    = LOG_INDEX_VERSION;
    idx.entry_size = LOG_INDEX_ENTRY;
    idx.count      = (uint64_t)w->index_count;
    idx.interval   = LOG_INDEX_INTERVAL;

    if (plat_file_seek(w->index_file, 0) != 0) return -1;
    if (plat_file_write(w->index_file, &idx, sizeof(idx))
            != (int)sizeof(idx)) return -1;
    if (w->index_count > 0) {
        size_t n = w->index_count * sizeof(log_index_entry_t);
        if (plat_file_write(w->index_file, w->index_entries, n) != (int)n)
            return -1;
    }
    return plat_file_sync(w->index_file);
}

static int write_empty_index(log_writer_t *w) {
    if (!w->index_file) return -1;
    log_index_header_t idx;
    memset(&idx, 0, sizeof(idx));
    idx.magic      = LOG_INDEX_MAGIC;
    idx.version    = LOG_INDEX_VERSION;
    idx.entry_size = LOG_INDEX_ENTRY;
    idx.count      = 0;
    idx.interval   = LOG_INDEX_INTERVAL;
    if (plat_file_seek(w->index_file, 0) != 0) return -1;
    if (plat_file_write(w->index_file, &idx, sizeof(idx))
            != (int)sizeof(idx)) return -1;
    return 0;
}

log_writer_t *log_writer_open(const char *path,
                              uint64_t max_bytes,
                              const registry_t *registry,
                              bool start_fresh) {
    if (!path) return NULL;
    if (max_bytes == 0) max_bytes = LOG_DEFAULT_MAX;
    if (max_bytes < LOG_MIN_MAX) {
        plat_log(LOG_ERROR, "log_writer: max_bytes %llu below minimum %u",
                 (unsigned long long)max_bytes, (unsigned)LOG_MIN_MAX);
        return NULL;
    }

    log_writer_t *w = (log_writer_t *)calloc(1, sizeof(*w));
    if (!w) return NULL;

    snprintf(w->path, sizeof(w->path), "%s", path);
    derive_index_path(path, w->index_path, sizeof(w->index_path));

    /* Does the file already exist with content? */
    int exists = 0;
    {
        plat_file_t *probe = plat_file_open(path, PLAT_FILE_READ);
        if (probe) {
            uint32_t first4 = 0;
            int got = plat_file_read(probe, &first4, 4);
            plat_file_close(probe);
            exists = (got == 4);
        }
    }

    if (exists && !start_fresh) {
        w->file = plat_file_open(path, PLAT_FILE_UPDATE);
        if (!w->file) {
            plat_log(LOG_ERROR, "log_writer: cannot open %s for update", path);
            free(w);
            return NULL;
        }
        if (read_header(w->file, &w->header) != 0) {
            plat_log(LOG_ERROR,
                     "log_writer: %s has invalid header, refusing to "
                     "overwrite (use start_fresh to reset)", path);
            plat_file_close(w->file);
            free(w);
            return NULL;
        }
        if (w->header.file_size != max_bytes) {
            plat_log(LOG_WARN,
                     "log_writer: requested max_bytes %llu differs from "
                     "existing %llu; using existing",
                     (unsigned long long)max_bytes,
                     (unsigned long long)w->header.file_size);
        }
    } else {
        w->file = plat_file_open(path, PLAT_FILE_WRITE);
        if (!w->file) {
            plat_log(LOG_ERROR, "log_writer: cannot create %s", path);
            free(w);
            return NULL;
        }
        if (create_log(w, registry, max_bytes) != 0) {
            plat_log(LOG_ERROR, "log_writer: failed to initialize %s", path);
            plat_file_close(w->file);
            free(w);
            return NULL;
        }
    }

    /* Index file. Update mode if resuming, write mode if fresh. */
    plat_file_mode_t idx_mode =
        (exists && !start_fresh) ? PLAT_FILE_UPDATE : PLAT_FILE_WRITE;
    w->index_file = plat_file_open(w->index_path, idx_mode);
    if (!w->index_file && idx_mode == PLAT_FILE_UPDATE) {
        /* If the index is missing, start a fresh one. */
        w->index_file = plat_file_open(w->index_path, PLAT_FILE_WRITE);
        if (w->index_file) {
            log_index_header_t idx;
            memset(&idx, 0, sizeof(idx));
            idx.magic      = LOG_INDEX_MAGIC;
            idx.version    = LOG_INDEX_VERSION;
            idx.entry_size = LOG_INDEX_ENTRY;
            idx.count      = 0;
            idx.interval   = LOG_INDEX_INTERVAL;
            plat_file_write(w->index_file, &idx, sizeof(idx));
        }
    }
    if (!w->index_file) {
        plat_log(LOG_WARN,
                 "log_writer: cannot open index %s; continuing without",
                 w->index_path);
    } else if (exists && !start_fresh && idx_mode == PLAT_FILE_UPDATE) {
        if (load_index(w) != 0) {
            plat_log(LOG_WARN,
                     "log_writer: index %s unusable; starting empty",
                     w->index_path);
            w->index_count = 0;
        }
    }

    w->fsync_at_ns   = plat_now_ns()
                     + (uint64_t)LOG_FSYNC_INTERVAL_MS * 1000000ull;
    w->rewrite_at_ns = plat_now_ns()
                     + (uint64_t)LOG_HEADER_REWRITE_MS * 1000000ull;

    plat_log(LOG_INFO,
             "log_writer: opened %s (max=%llu, data_start=%llu, records=%llu)",
             path,
             (unsigned long long)max_bytes,
             (unsigned long long)w->header.data_start,
             (unsigned long long)w->header.record_count);
    return w;
}

/* ------------------------------------------------------------------ */
/* Append                                                             */
/* ------------------------------------------------------------------ */

static void wrap_ring(log_writer_t *w, uint64_t incoming_ts) {
    w->header.write_offset = w->header.data_start;
    w->header.wrap_count++;
    w->header.first_ts_ns = incoming_ts;
    if (w->header.write_offset >= w->header.file_size) {
        plat_log(LOG_ERROR,
                 "log_writer: data_start %llu >= file_size %llu",
                 (unsigned long long)w->header.write_offset,
                 (unsigned long long)w->header.file_size);
    }
    plat_log(LOG_DEBUG,
             "log_writer: wrapped ring (wrap_count=%u, new offset=%llu)",
             (unsigned)w->header.wrap_count,
             (unsigned long long)w->header.write_offset);
}

int log_writer_append(log_writer_t *w, const packet_t *p) {
    if (!w || !p) return -1;

    uint8_t buf[PACKET_BIN_MAX];
    int encoded = packet_encode(p, buf, sizeof(buf));
    if (encoded <= 0) return -1;

    uint64_t size = (uint64_t)encoded;

    if (w->header.write_offset + size > w->header.file_size) {
        wrap_ring(w, p->timestamp_ns);
    }

    if (plat_file_seek(w->file, w->header.write_offset) != 0) {
        plat_log(LOG_ERROR, "log_writer: seek to %llu failed",
                 (unsigned long long)w->header.write_offset);
        return -1;
    }
    if (plat_file_write(w->file, buf, size) != (int)size) {
        plat_log(LOG_ERROR, "log_writer: write of %llu bytes failed",
                 (unsigned long long)size);
        return -1;
    }

    uint64_t prev_count = w->header.record_count;
    uint64_t record_offset = w->header.write_offset;

    w->header.write_offset += size;
    w->header.record_count++;
    w->header.last_ts_ns = p->timestamp_ns;
    if (prev_count == 0) w->header.first_ts_ns = p->timestamp_ns;

    /* Index every LOG_INDEX_INTERVAL records. The entry points at the
     * record we just wrote, so a seek to its timestamp lands here. */
    if ((w->header.record_count % LOG_INDEX_INTERVAL) == 1
            || w->header.record_count == 1) {
        if (w->index_count == w->index_cap) {
            size_t new_cap = w->index_cap ? w->index_cap * 2 : 64;
            log_index_entry_t *p2 = (log_index_entry_t *)realloc(
                w->index_entries, new_cap * sizeof(log_index_entry_t));
            if (p2) {
                w->index_entries = p2;
                w->index_cap = new_cap;
            }
        }
        if (w->index_count < w->index_cap) {
            w->index_entries[w->index_count].timestamp_ns = p->timestamp_ns;
            w->index_entries[w->index_count].file_offset = record_offset;
            w->index_count++;
        }
    }

    uint64_t now = plat_now_ns();
    if (now >= w->rewrite_at_ns) {
        if (write_header(w) != 0) {
            plat_log(LOG_WARN, "log_writer: header rewrite failed");
        }
        w->rewrite_at_ns = now
                         + (uint64_t)LOG_HEADER_REWRITE_MS * 1000000ull;
    }
    if (now >= w->fsync_at_ns) {
        plat_file_sync(w->file);
        if (w->index_file && save_index(w) != 0) {
            plat_log(LOG_WARN, "log_writer: index save failed");
        }
        w->fsync_at_ns = now
                       + (uint64_t)LOG_FSYNC_INTERVAL_MS * 1000000ull;
    }

    return (int)size;
}

/* ------------------------------------------------------------------ */
/* Flush / close / introspection                                      */
/* ------------------------------------------------------------------ */

int log_writer_flush(log_writer_t *w) {
    if (!w) return -1;
    if (write_header(w) != 0) return -1;
    if (plat_file_sync(w->file) != 0) return -1;
    if (w->index_file) {
        if (save_index(w) != 0) return -1;
    }
    return 0;
}

void log_writer_close(log_writer_t *w) {
    if (!w) return;
    log_writer_flush(w);
    if (w->file)       plat_file_close(w->file);
    if (w->index_file) plat_file_close(w->index_file);
    free(w->index_entries);
    free(w);
}

uint64_t log_writer_record_count(const log_writer_t *w) {
    return w ? w->header.record_count : 0;
}

uint64_t log_writer_write_offset(const log_writer_t *w) {
    return w ? w->header.write_offset : 0;
}

uint32_t log_writer_wrap_count(const log_writer_t *w) {
    return w ? w->header.wrap_count : 0;
}

const char *log_writer_path(const log_writer_t *w) {
    return w ? w->path : NULL;
}