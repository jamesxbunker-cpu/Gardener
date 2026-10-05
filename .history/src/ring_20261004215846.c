#include "gardener/ring.h"
#include <string.h>

int ring_init(ring_t *r, sample_t *storage, size_t cap) {
    if (!r || !storage || cap == 0) return -1;
    r->buf   = storage;
    r->cap   = cap;
    r->head  = 0;
    r->count = 0;
    return 0;
}

void ring_push(ring_t *r, const sample_t *s) {
    if (!r || !r->buf || r->cap == 0) return;
    r->buf[r->head] = *s;
    r->head = (r->head + 1) % r->cap;
    if (r->count < r->cap) r->count++;
}

size_t ring_count(const ring_t *r) {
    return r ? r->count : 0;
}

size_t ring_capacity(const ring_t *r) {
    return r ? r->cap : 0;
}

bool ring_full(const ring_t *r) {
    return r && r->cap > 0 && r->count == r->cap;
}

const sample_t *ring_at(const ring_t *r, size_t idx) {
    if (!r || !r->buf || idx >= r->count) return NULL;
    /* Oldest element sits 'count' slots behind head. */
    size_t start = (r->head + r->cap - r->count) % r->cap;
    return &r->buf[(start + idx) % r->cap];
}

const sample_t *ring_newest(const ring_t *r) {
    if (!r || r->count == 0) return NULL;
    return ring_at(r, r->count - 1);
}

const sample_t *ring_oldest(const ring_t *r) {
    if (!r || r->count == 0) return NULL;
    return ring_at(r, 0);
}

void ring_clear(ring_t *r) {
    if (!r) return;
    r->head  = 0;
    r->count = 0;
}