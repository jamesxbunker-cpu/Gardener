#ifndef GARDENER_RING_H
#define GARDENER_RING_H

#include "gardener/channel.h"
#include <stddef.h>
#include <stdbool.h>

/* Fixed-capacity ring of samples. No allocation after ring_init. Storage
 * is provided by the caller so the ring itself stays trivially copyable
 * and can live inside a large static array (see ingest.h). */
typedef struct {
    sample_t *buf;
    size_t    cap;
    size_t    head;    /* next write index */
    size_t    count;   /* valid entries, <= cap */
} ring_t;

/* Attach storage. Returns 0 on success, -1 on bad arguments. */
int  ring_init(ring_t *r, sample_t *storage, size_t cap);

/* Push a sample, overwriting the oldest when full. Safe on an
 * uninitialized ring only if ring_init returned 0. */
void ring_push(ring_t *r, const sample_t *s);

/* Number of valid entries currently in the ring. */
size_t ring_count(const ring_t *r);

/* Capacity the ring was initialized with. */
size_t ring_capacity(const ring_t *r);

/* True if the ring has wrapped at least once (i.e. the oldest entry has
 * been overwritten). Useful for detectors that need a "full window" or
 * nothing. */
bool ring_full(const ring_t *r);

/* Indexed access. idx 0 = oldest, count-1 = newest. Returns NULL if the
 * ring is uninitialized or idx is out of range. */
const sample_t *ring_at(const ring_t *r, size_t idx);

/* Convenience: newest sample, or NULL if empty. */
const sample_t *ring_newest(const ring_t *r);

/* Convenience: oldest sample, or NULL if empty. */
const sample_t *ring_oldest(const ring_t *r);

/* Wipe contents without freeing storage. */
void ring_clear(ring_t *r);

#endif