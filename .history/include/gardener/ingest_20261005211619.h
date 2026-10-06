#ifndef GARDENER_INGEST_H
#define GARDENER_INGEST_H

#include "gardener/packet.h"
#include "gardener/registry.h"
#include "gardener/ring.h"

#include <stdbool.h>
#include <stdint.h>

/* ---------------------------------------------------------------------
 * Ingest engine.
 *
 * Owns the per-channel sample rings and the "latest" snapshot of every
 * channel that has ever been seen. Packets come in through
 * ingest_feed(); every other module that needs history or the current
 * value of a channel reads from here.
 *
 * The whole structure is static: no heap, no allocation after init. It
 * embeds a registry_t, so registry lookup, unknown-channel synthesis,
 * and channel metadata are all available at ingest time without extra
 * plumbing.
 * ------------------------------------------------------------------- */

/* Samples retained per channel. 1024 samples at 10 Hz = ~102 s of
 * history. Enough for the baseline and rate-of-change detectors in
 * Phase C; resize when their windows become concrete. */
#define INGEST_RING_CAP 1024

/* Per-channel state. */
typedef struct {
    bool     seen;       /* has this channel ever received a sample? */
    ring_t   ring;       /* sliding window of recent samples */
    sample_t latest;     /* most recent sample, valid iff seen */
} channel_state_t;

typedef struct {
    registry_t       registry;
    channel_state_t  ch[REGISTRY_MAX_CHANNELS];
    sample_t         storage[REGISTRY_MAX_CHANNELS][INGEST_RING_CAP];

    /* Counters for health and the dump tool. Monotonic. */
    uint64_t packets_seen;
    uint64_t samples_seen;
    uint64_t samples_dropped;   /* bad id, bad type, zero-length, etc. */
    uint64_t unknown_channels;  /* times a sample arrived for an unknown id */
} ingest_t;

/* Initialize. If channels_toml is non-NULL, loads the registry from it.
 * Returns 0 on success, -1 if the registry could not be initialized or
 * loaded. */
int ingest_init(ingest_t *ing, const char *channels_toml);

/* Feed one packet. Returns the number of samples accepted (>= 0), or
 * -1 on hard error (NULL arguments, packet with too many channels). A
 * packet with an unknown channel id is accepted: the registry
 * synthesizes a placeholder and the sample is stored. */
int ingest_feed(ingest_t *ing, const packet_t *p);

/* Latest sample for a channel, or NULL if never seen. */
const sample_t *ingest_latest(const ingest_t *ing, uint16_t channel_id);

/* Full per-channel state, or NULL if id is out of range. Note: returns
 * non-NULL even for never-seen channels (so callers can check ->seen).
 * Use ingest_latest() if you only want the sample. */
const channel_state_t *ingest_state(const ingest_t *ing, uint16_t channel_id);

/* Convenience: is this channel currently producing samples? */
bool ingest_has_channel(const ingest_t *ing, uint16_t channel_id);

#endif /* GARDENER_INGEST_H */