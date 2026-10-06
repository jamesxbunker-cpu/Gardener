#ifndef GARDENER_LOG_H
#define GARDENER_LOG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---------------------------------------------------------------------
 * Binary log format.
 *
 * One file (`gardener.bin` by default) plus a sidecar index
 * (`gardener.idx`). The log is preallocated at LOG_DEFAULT_MAX bytes
 * and wraps to the beginning when full. The index is sparse and
 * advisory; the log file alone is sufficient to recover all records.
 *
 * Layout:
 *
 *   ┌─────────────────────────────────────────┐  0
 *   │ Header (4096 bytes, see log_header_t)   │
 *   ├─────────────────────────────────────────┤  4096
 *   │ Registry snapshot (JSONL, registry_size)│
 *   ├─────────────────────────────────────────┤  data_start
 *   │ Record 0 (packet_encode output)         │
 *   │ Record 1                                │
 *   │ ...                                     │
 *   │ Record N-1                              │
 *   ├─────────────────────────────────────────┤  file_size
 *   │ (zero padding if not yet wrapped)       │
 *   └─────────────────────────────────────────┘
 *
 * After a wrap, records are circular: the newest record ends at
 * write_offset, the oldest begins at data_start. The reader must
 * consult header.write_offset to know where the logical start is.
 *
 * Every record is exactly one packet_encode() output. Records have no
 * framing beyond that; to walk them, read the packet header to get
 * channel_count, compute 22 + 18 × channel_count, and advance.
 *
 * Each record is protected by its own CRC (inside packet_encode).
 * The header has its own CRC. The index has no CRC and is not
 * authoritative — a reader that finds an inconsistency falls back to
 * a linear scan.
 * ------------------------------------------------------------------- */

/* ---- log constants ------------------------------------------------ */

#define LOG_MAGIC              0x47415244u   /* "GARD" */
#define LOG_VERSION            1
#define LOG_HEADER_SIZE        4096

/* Default maximum log size. Files are preallocated at this size. */
#define LOG_DEFAULT_MAX        (100u * 1024u * 1024u)

/* Refuse to open a log configured smaller than this. A ring has to be
 * at least a few records deep to be useful, and the header itself is
 * 4 KB. */
#define LOG_MIN_MAX            (1024u * 1024u)

/* How often the writer fsyncs the log to disk. Every write would wear
 * out an SD card; never flushing loses too much on power loss. */
#define LOG_FSYNC_INTERVAL_MS  2000

/* How often the writer rewrites the header block with updated
 * counters. Cheap enough to do often, expensive enough not to do
 * per-record. */
#define LOG_HEADER_REWRITE_MS  10000

/* ---- index file constants ----------------------------------------- */

#define LOG_INDEX_MAGIC        0x47494458u   /* "GIDX" */
#define LOG_INDEX_VERSION      1
#define LOG_INDEX_HEADER       64            /* bytes */
#define LOG_INDEX_ENTRY        16            /* bytes */
#define LOG_INDEX_INTERVAL     256           /* records between entries */

/* ---- header block ------------------------------------------------- */

/* On-disk header. Field offsets are locked by a test; do not reorder
 * without bumping LOG_VERSION. */
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;             /* LOG_MAGIC */
    uint16_t version;           /* LOG_VERSION */
    uint16_t header_size;       /* LOG_HEADER_SIZE */
    uint64_t file_size;         /* bytes, == LOG_DEFAULT_MAX at creation */
    uint64_t data_start;        /* header_size + registry_size */
    uint64_t write_offset;      /* next byte to write; wraps at file_size */
    uint64_t record_count;      /* monotonic across wraps */
    uint64_t first_ts_ns;       /* oldest record in file, 0 if empty */
    uint64_t last_ts_ns;        /* newest record in file, 0 if empty */
    uint64_t start_wall_ns;     /* wall clock when log was created */
    uint64_t last_wall_ns;      /* wall clock at last header rewrite */
    uint32_t channel_count;     /* descriptors in the registry snapshot */
    uint32_t registry_size;     /* bytes of JSONL at offset 4096 */
    uint32_t header_crc;        /* CRC-16/MODBUS over bytes [0, 80) */
    uint32_t wrap_count;        /* number of times the ring has wrapped */
    uint8_t  _reserved[LOG_HEADER_SIZE - 88];
} log_header_t;
#pragma pack(pop)

/* ---- index file --------------------------------------------------- */

/* Header of the sidecar index. Followed by `count` entries of
 * log_index_entry_t. */
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;             /* LOG_INDEX_MAGIC */
    uint16_t version;           /* LOG_INDEX_VERSION */
    uint16_t entry_size;        /* sizeof(log_index_entry_t) */
    uint64_t count;             /* number of valid entries */
    uint64_t interval;          /* records between entries */
    uint8_t  _reserved[LOG_INDEX_HEADER - 32];
} log_index_header_t;

typedef struct {
    uint64_t timestamp_ns;
    uint64_t file_offset;
} log_index_entry_t;
#pragma pack(pop)

/* ---- compile-time checks ------------------------------------------ */

/* The on-disk header must fit in LOG_HEADER_SIZE. This is a build-time
 * guard, not a runtime check. If it fails, either the struct grew or
 * LOG_HEADER_SIZE shrank, and either way the format changed. */
_Static_assert(sizeof(log_header_t) == LOG_HEADER_SIZE,
               "log_header_t must be exactly LOG_HEADER_SIZE bytes");
_Static_assert(sizeof(log_index_entry_t) == LOG_INDEX_ENTRY,
               "log_index_entry_t must be 16 bytes");
_Static_assert(sizeof(log_index_header_t) == LOG_INDEX_HEADER,
               "log_index_header_t must be LOG_INDEX_HEADER bytes");

/* ---- helper accessors --------------------------------------------- */

/* Size in bytes of the record that a packet of N channels will produce
 * on disk. Matches packet_encode's return value. Callers that have a
 * decoded packet_t pass p->channel_count. */
static inline uint64_t log_record_size(uint16_t channel_count) {
    return 22u + 18u * (uint64_t)channel_count;
}

#endif /* GARDENER_LOG_H */