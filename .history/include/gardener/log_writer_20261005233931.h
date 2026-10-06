#ifndef GARDENER_LOG_WRITER_H
#define GARDENER_LOG_WRITER_H

#include "gardener/log.h"
#include "gardener/packet.h"
#include "gardener/registry.h"

#include <stdbool.h>
#include <stdint.h>

/* ---------------------------------------------------------------------
 * Log writer.
 *
 * Opens or creates a binary log file and its sidecar index. Appends
 * encoded packets, wrapping the file to the beginning when it reaches
 * the configured maximum size. Rewrites the header periodically with
 * updated counters; fsyncs periodically.
 *
 * The writer is not thread-safe. One writer per file.
 *
 * On open:
 *   - if the log does not exist, create it, preallocate max_bytes,
 *     write a fresh header and index, and start appending at data_start
 *   - if the log exists with a valid header, resume from write_offset
 *   - if the log exists with an invalid header, refuse (never
 *     silently overwrite a possibly-valuable file)
 *
 * The registry snapshot is written once at creation. Reopening with a
 * different registry than the file was created with fails unless
 * `start_fresh` is set.
 * ------------------------------------------------------------------- */

typedef struct log_writer log_writer_t;

/* Open or create a log.
 *
 * `path` is the log file path. The index file is derived: for
 * "X.bin", the index is "X.idx". For any other path, the index is
 * "<path>.idx".
 *
 * `max_bytes` is the preallocated file size. If 0, LOG_DEFAULT_MAX is
 * used. Values below LOG_MIN_MAX are refused.
 *
 * `registry` provides the channel descriptors written into the header
 * snapshot. May be NULL, in which case the log records no channels
 * (fine for tests, useless in practice).
 *
 * `start_fresh` causes an existing file to be truncated and rewritten.
 * Set this when the caller knows the file is expendable.
 *
 * Returns NULL on any failure. Errors are logged via plat_log. */
log_writer_t *log_writer_open(const char *path,
                              uint64_t max_bytes,
                              const registry_t *registry,
                              bool start_fresh);

/* Append one packet. Returns the number of bytes written (the encoded
 * record size), or -1 on error. On error, the file is left in its
 * previous state as much as possible: if the encode fails, nothing is
 * written; if the write fails partway, the header is not updated and
 * the next open will recover from the last known-good write_offset. */
int log_writer_append(log_writer_t *w, const packet_t *p);

/* Flush pending writes and rewrite the header. Called automatically on
 * close and on the fsync interval; calling explicitly is safe. */
int log_writer_flush(log_writer_t *w);

/* Close and free. Flushes first. Safe on NULL. */
void log_writer_close(log_writer_t *w);

/* Introspection, mostly for tests and the stats line in gardener. */
uint64_t log_writer_record_count(const log_writer_t *w);
uint64_t log_writer_write_offset(const log_writer_t *w);
uint32_t log_writer_wrap_count(const log_writer_t *w);
const char *log_writer_path(const log_writer_t *w);

#endif /* GARDENER_LOG_WRITER_H */