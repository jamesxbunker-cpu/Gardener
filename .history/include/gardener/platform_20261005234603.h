#ifndef GARDENER_PLATFORM_H
#define GARDENER_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

/* ---------------------------------------------------------------------
 * Platform layer.
 *
 * This header is the ONLY place in Gardener that talks to the operating
 * system directly. Every other module calls through here. When Gardener
 * moves to an MCU (V2), we replace src/platform/platform_posix.c with
 * src/platform/platform_mcu.c and touch nothing else.
 *
 * Rule: no OS headers (unistd.h, pthread.h, sys, windows.h, and so
 * on) outside this folder. If a module needs a new OS capability, add
 * a function here first.
 * ------------------------------------------------------------------- */

/* ---- time ---------------------------------------------------------- */

/* Monotonic time in nanoseconds. Never goes backward. Not wall clock;
 * use plat_wall_ns() if you need something comparable across reboots. */
uint64_t plat_now_ns(void);

/* Wall-clock time in nanoseconds since the Unix epoch. May jump if the
 * system clock is adjusted; use for logging and persistence, not for
 * measuring intervals. */
uint64_t plat_wall_ns(void);

/* Sleep for at least this many milliseconds. */
void plat_sleep_ms(unsigned ms);

/* ---- files --------------------------------------------------------- */

typedef struct plat_file plat_file_t;

/* Open a file for reading (for_write=0) or writing (for_write=1).
 * Writing truncates any existing file. Returns NULL on failure. */
plat_file_t *plat_file_open(const char *path, plat_file_t mode);

/* Read up to len bytes; returns bytes read, 0 on EOF, -1 on error. */
int plat_file_read(plat_file_t *f, void *buf, size_t len);

/* Write len bytes; returns bytes written, -1 on error. */
int plat_file_write(plat_file_t *f, const void *buf, size_t len);

/* Flush to the OS. Does not guarantee physical media write; that's
 * platform-specific and costs more than we want per-sample. */
int plat_file_sync(plat_file_t *f);

/* Close and release. NULL is safe. */
void plat_file_close(plat_file_t *f);

/* ---- logging ------------------------------------------------------- */

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERROR = 3,
} log_level_t;

/* Write a formatted line to the current log sink. Phase A sends this to
 * stderr. Phase B will additionally append to the events file. */
void plat_log(log_level_t lvl, const char *fmt, ...);

/* Set the minimum level that will be printed. Default is LOG_INFO. */
void plat_log_set_level(log_level_t lvl);

/* Seek to an absolute byte offset in the file. Returns 0 on success. */
int plat_file_seek(plat_file_t *f, uint64_t offset);

/* Current byte offset, or (uint64_t)-1 on error. */
uint64_t plat_file_tell(plat_file_t *f);

/* Truncate the file to `size` bytes. Grows the file if it is shorter
 * (by writing zeros at the new end). Returns 0 on success. */
int plat_file_truncate(plat_file_t *f, uint64_t size);

#endif /* GARDENER_PLATFORM_H */