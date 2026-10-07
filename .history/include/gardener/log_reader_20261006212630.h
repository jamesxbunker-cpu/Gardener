#ifndef GARDENER_LOG_READER_H
#define GARDENER_LOG_READER_H

#include "gardener/log.h"
#include "gardener/packet.h"
#include "gardener/registry.h"

#include <stdbool.h>
#include <stdint.h>

/* ---------------------------------------------------------------------
 * Log reader.
 *
 * Opens a log written by log_writer and iterates its records in
 * logical order (oldest first when wraps have occurred). Supports
 * time-range queries via the sidecar index.
 *
 * The reader is not thread-safe. One reader per file.
 * ------------------------------------------------------------------- */

typedef struct log_reader log_reader_t;

/* Open a log for reading. Loads the header and registry snapshot.
 * Attempts to load the sidecar index; if the index is missing or
 * inconsistent, the reader falls back to linear scanning for time
 * queries.
 *
 * Returns NULL on any failure (bad header, missing file, unreadable).
 * Errors are logged via plat_log. */
log_reader_t *log_reader_open(const char *path);

/* Close and free. Safe on NULL. */
void log_reader_close(log_reader_t *r);

/* Reset to the oldest record. Call this after open, or to restart
 * iteration. */
int log_reader_rewind(log_reader_t *r);

/* Read the next record in logical order. Returns:
 *   1   packet decoded into *out
 *   0   end of log reached
 *  -1   error (bad record, I/O failure)
 * Caller must not assume channel_count on error. */
int log_reader_next(log_reader_t *r, packet_t *out);

/* Read the previous record in logical order (going backward in time).
 * Same return values as log_reader_next. */
int log_reader_prev(log_reader_t *r, packet_t *out);

/* Seek to the first record with timestamp >= ts, then leave the
 * reader positioned so a subsequent log_reader_next returns that
 * record. Returns 0 on success, -1 on error. If no such record
 * exists, the reader is positioned at end-of-log; the next
 * log_reader_next returns 0. */
int log_reader_seek_ns(log_reader_t *r, uint64_t ts);

/* Total records written to the file, including those overwritten
 * by wraps. Not the number of records currently readable — for that,
 * iterate and count. */
uint64_t log_reader_total_records(const log_reader_t *r);

/* Number of wraps the file has undergone. 0 means records are
 * contiguous from data_start to write_offset. */
uint32_t log_reader_wrap_count(const log_reader_t *r);

/* Read-only view of the header as it was when the log was opened. */
const log_header_t *log_reader_header(const log_reader_t *r);

/* Registry reconstructed from the header's JSONL snapshot. Owned by
 * the reader; valid until log_reader_close. May have zero channels. */
const registry_t *log_reader_registry(const log_reader_t *r);

/* Path this reader was opened with. */
const char *log_reader_path(const log_reader_t *r);

#endif /* GARDENER_LOG_READER_H */