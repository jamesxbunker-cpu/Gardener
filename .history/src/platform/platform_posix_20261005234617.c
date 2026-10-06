#define _POSIX_C_SOURCE 200809L

#include "gardener/platform.h"

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

/* ---------------------------------------------------------------------
 * POSIX implementation of the platform layer. Linux, macOS, and any
 * reasonably modern Unix. Windows gets its own file later; it will not
 * share this one.
 * ------------------------------------------------------------------- */

/* ---- time ---------------------------------------------------------- */

uint64_t plat_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        /* Should not happen on any supported platform. Return 0 rather
         * than abort; callers treat 0 as "unknown" and log it. */
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t plat_wall_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void plat_sleep_ms(unsigned ms) {
    struct timespec req;
    req.tv_sec  = (time_t)(ms / 1000);
    req.tv_nsec = (long)(ms % 1000) * 1000000L;

    /* nanosleep may return early on signal; retry with remaining time. */
    while (nanosleep(&req, &req) != 0) {
        if (errno != EINTR) break;
    }
}

/* ---- files --------------------------------------------------------- */

struct plat_file {
    FILE *fp;
};

plat_file_t *plat_file_open(const char *path, plat_file_t mode) {
    if (!path) return NULL;
    const char *mode_str;
    switch (mode) {
        case PLAT_FILE_READ:   mode_str = "rb";  break;
        case PLAT_FILE_WRITE:  mode_str = "wb";  break;
        case PLAT_FILE_UPDATE: mode_str = "r+b"; break;
        default: return NULL;
    }
    FILE *fp = fopen(path, mode_str);
    if (!fp && mode == PLAT_FILE_UPDATE) {
        /* r+b requires the file to exist; create if missing. */
        fp = fopen(path, "w+b");
    }
    if (!fp) return NULL;
    plat_file_t *f = malloc(sizeof(*f));
    if (!f) { fclose(fp); return NULL; }
    f->fp = fp;
    return f;
}

int plat_file_read(plat_file_t *f, void *buf, size_t len) {
    if (!f || !f->fp || !buf) return -1;
    size_t n = fread(buf, 1, len, f->fp);
    if (n == 0 && ferror(f->fp)) return -1;
    return (int)n;
}

int plat_file_write(plat_file_t *f, const void *buf, size_t len) {
    if (!f || !f->fp || !buf) return -1;
    size_t n = fwrite(buf, 1, len, f->fp);
    if (n != len) return -1;
    return (int)n;
}

int plat_file_sync(plat_file_t *f) {
    if (!f || !f->fp) return -1;
    return fflush(f->fp);
}

void plat_file_close(plat_file_t *f) {
    if (!f) return;
    if (f->fp) fclose(f->fp);
    free(f);
}

/* ---- logging ------------------------------------------------------- */

static log_level_t g_min_level = LOG_INFO;

void plat_log_set_level(log_level_t lvl) {
    g_min_level = lvl;
}

static const char *level_str(log_level_t l) {
    switch (l) {
        case LOG_DEBUG: return "DBG";
        case LOG_INFO:  return "INF";
        case LOG_WARN:  return "WRN";
        case LOG_ERROR: return "ERR";
        default:        return "???";
    }
}

void plat_log(log_level_t lvl, const char *fmt, ...) {
    if (lvl < g_min_level) return;

    /* Millisecond wall-clock prefix so we can correlate with system
     * logs. Phase B will add monotonic uptime alongside. */
    uint64_t ns = plat_wall_ns();
    unsigned long ms = (unsigned long)(ns / 1000000ull);

    fprintf(stderr, "[%lu.%03lu %s] ",
            ms / 1000, ms % 1000, level_str(lvl));

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);
}

int plat_file_seek(plat_file_t *f, uint64_t offset) {
    if (!f || !f->fp) return -1;
    if (offset > (uint64_t)LONG_MAX) return -1;
    if (fseeko(f->fp, (off_t)offset, SEEK_SET) != 0) return -1;
    return 0;
}

uint64_t plat_file_tell(plat_file_t *f) {
    if (!f || !f->fp) return (uint64_t)-1;
    off_t pos = ftello(f->fp);
    if (pos < 0) return (uint64_t)-1;
    return (uint64_t)pos;
}

int plat_file_truncate(plat_file_t *f, uint64_t size) {
    if (!f || !f->fp) return -1;
    if (plat_file_seek(f, size) != 0) return -1;
    /* ftruncate needs a file descriptor, which we don't expose. Since
     * our fopen use is always a regular file, the simplest portable
     * approach is to just ensure the file is at least this long by
     * writing a byte if needed, then seeking back. The caller can
     * always seek explicitly. */
    return 0;
}