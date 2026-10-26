#ifndef GARDENER_BASELINE_H
#define GARDENER_BASELINE_H

#include "gardener/channel.h"
#include "gardener/packet.h"
#include "gardener/registry.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------
 * Baseline model.
 *
 * Per (channel, regime), maintains a running summary of what "normal"
 * looks like: count, mean, variance, min, max, first and last sample
 * time, and a state indicator. Summaries are Welford's incremental
 * algorithm (numerically stable) with exponential decay (old samples
 * fade out gradually, so normal can shift over weeks without a
 * re-learn).
 *
 * A "regime" is a discrete operating state derived from one or more
 * is_regime channels. For example, a system with a single `running`
 * bool has 2 regimes. A system with `mode` (enum, 4 values) and
 * `running` (bool) has 8. Regimes are keyed by a hash of the current
 * regime-channel values, modulo BASELINE_MAX_REGIMES. Collisions merge
 * two regimes; with 16 slots and typical systems using 2–8 regimes,
 * collisions are rare and harmless.
 *
 * The baseline model persists to disk. On-disk layout is fixed-size and
 * versioned. See the "On-disk format" comment below.
 * ------------------------------------------------------------------- */

/* ---- constants ---------------------------------------------------- */

#define BASELINE_MAGIC          0x42415345u   /* "BASE" */
#define BASELINE_VERSION        1
#define BASELINE_HEADER_SIZE    64

#define BASELINE_MAX_CHANNELS   256           /* matches REGISTRY_MAX_CHANNELS */
#define BASELINE_MAX_REGIMES    16
#define BASELINE_MAX_REGIME_CH  4             /* max channels in the regime key */

/* Per-(channel, regime) statistics slot. 40 bytes, packed. */
#define BASELINE_STAT_SIZE      40

/* Per-channel table entry. 8 bytes, packed. */
#define BASELINE_CHANNEL_ENTRY_SIZE  8

/* File layout offsets. Everything is packed little-endian. */
#define BASELINE_CHANNEL_TABLE_OFF  BASELINE_HEADER_SIZE
#define BASELINE_STATS_OFF          (BASELINE_CHANNEL_TABLE_OFF + \
                                     BASELINE_MAX_CHANNELS * BASELINE_CHANNEL_ENTRY_SIZE)
#define BASELINE_STATS_SIZE         (BASELINE_MAX_CHANNELS * \
                                     BASELINE_MAX_REGIMES * BASELINE_STAT_SIZE)
#define BASELINE_TRAILER_OFF        (BASELINE_STATS_OFF + BASELINE_STATS_SIZE)
#define BASELINE_FILE_SIZE          (BASELINE_TRAILER_OFF + 4)

/* Cold-start threshold: a (channel, regime) is READY once it has seen
 * at least this many samples. Detectors should not fire while a slot
 * is LEARNING. */
#define BASELINE_MIN_SAMPLES        100

/* Default exponential half-life for decay, in nanoseconds (300 s). A
 * sample loses half its weight every half-life. At 10 Hz, 300 s is
 * 3000 samples. */
#define BASELINE_DEFAULT_HALFLIFE_NS  (300ull * 1000000000ull)

/* ---- state enum --------------------------------------------------- */

typedef enum {
    BASELINE_EMPTY     = 0,   /* never seen a sample */
    BASELINE_LEARNING  = 1,   /* seen < BASELINE_MIN_SAMPLES */
    BASELINE_READY     = 2,   /* enough samples; detector-safe */
    BASELINE_STALE     = 3,   /* not seen recently; decayed below threshold */
} baseline_state_t;

/* ---- on-disk types ------------------------------------------------ */

/* Header, 64 bytes, packed. */
typedef struct {
    uint32_t magic;                          /* 0 */
    uint16_t version;                        /* 4 */
    uint16_t header_size;                    /* 6 */
    uint32_t channel_count;                  /* 8 */
    uint32_t regime_count;                   /* 12 */
    uint64_t created_ns;                     /* 16 */
    uint64_t saved_ns;                       /* 24 */
    uint64_t halflife_ns;                    /* 32 */
    uint16_t regime_ch_ids[BASELINE_MAX_REGIME_CH];  /* 40 */
    uint8_t  regime_ch_count;                /* 48 */
    uint8_t  _pad1[3];                       /* 49..51 */
    uint32_t _reserved;                      /* 52 */
    uint8_t  _pad2[BASELINE_HEADER_SIZE - 56];  /* 56..63 */
} baseline_header_t;

/* Channel table entry, 8 bytes. */
typedef struct {
    uint16_t ch_id;
    uint16_t n_regimes_used;    /* highest regime index with data + 1 */
    uint32_t _pad;
} baseline_channel_entry_t;

/* Per-(channel, regime) statistics slot, 32 bytes, packed.
 *
 * Fields are in the order Welford wants:
 *   count:   number of samples (may be fractional after decay)
 *   mean:    running mean
 *   m2:      sum of squares of differences from the mean (variance * count)
 *   min/max: observed extremes
 *   state:   current baseline_state_t
 *   flags:   bit 0 = has_range_from_config (informational)
 *   _pad:    alignment / future
 *
 * After decay, count and m2 shrink while mean stays. This keeps the
 * mean accurate and the variance estimate consistent as old samples
 * age out.
 */
#pragma pack(push, 1)
typedef struct {
    float    count;         /* 0 */
    float    mean;          /* 4 */
    float    m2;            /* 8 */
    float    min_val;       /* 12 */
    float    max_val;       /* 16 */
    uint64_t first_ts_ns;   /* 20 */
    uint64_t last_ts_ns;    /* 28 */
    uint8_t  state;         /* 36 */
    uint8_t  flags;         /* 37 */
    uint16_t _pad;          /* 38..39 */
} baseline_stat_t;
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct {
    uint32_t crc;               /* CRC-16/MODBUS over [0, BASELINE_TRAILER_OFF) */
} baseline_trailer_t;
#pragma pack(pop)

/* ---- in-memory model ---------------------------------------------- */

typedef struct {
    baseline_header_t        header;
    baseline_channel_entry_t channels[BASELINE_MAX_CHANNELS];
    baseline_stat_t          stats[BASELINE_MAX_CHANNELS][BASELINE_MAX_REGIMES];

    uint16_t                 slot_to_ch_id[BASELINE_MAX_CHANNELS];
    uint16_t                 next_slot;

    /* Derived at init from the registry. Kept in memory for fast
     * regime derivation on every packet. */
    uint16_t                 regime_ch_ids[BASELINE_MAX_REGIME_CH];
    uint8_t                  regime_ch_count;

    /* Per-regime counter of samples seen (used to fill the header's
     * regime_count on save). Reset to zero on init. */
    uint64_t                 regime_sample_count[BASELINE_MAX_REGIMES];
} baseline_t;

/* ---- API ---------------------------------------------------------- */

/* Initialize a baseline model. Populates regime channels from the
 * registry: any channel with is_regime = true, up to
 * BASELINE_MAX_REGIME_CH. Extras are ignored with a warning.
 *
 * registry may be NULL (no regime channels; single-regime model).
 * halflife_ns = 0 uses BASELINE_DEFAULT_HALFLIFE_NS. */
int baseline_init(baseline_t *b, const registry_t *registry,
                  uint64_t halflife_ns);

/* Feed one packet. For every valid sample:
 *   - derive the current regime from the packet's regime-channel values
 *   - find or register the channel's slot
 *   - update the (channel, regime) slot
 *   - transition state as in C1
 *
 * Returns number of samples accepted, or -1 on error. */
int baseline_update(baseline_t *b, const packet_t *p);

/* Variance / stddev: same as C1. */
float baseline_variance(const baseline_stat_t *s);
float baseline_stddev(const baseline_stat_t *s);

/* Lookup: find the stat slot for (channel, regime). Returns NULL if
 * the channel is unknown or that regime slot is EMPTY. */
const baseline_stat_t *baseline_get(const baseline_t *b, uint16_t ch_id,
                                    uint16_t regime);

/* Derive the current regime index from a packet. Returns 0 if the
 * baseline has no regime channels configured. Otherwise hashes the
 * regime-channel values and returns the result mod BASELINE_MAX_REGIMES.
 *
 * If any regime channel is missing from the packet, returns 0xFFFF
 * (unknown regime). Callers should skip that packet's samples. */
uint16_t baseline_regime_for_packet(const baseline_t *b, const packet_t *p);

/* Number of channels registered (have at least one sample). */
uint16_t baseline_channel_count(const baseline_t *b);

/* Number of non-empty regime slots (used for stats). */
uint16_t baseline_regime_count(const baseline_t *b);

/* Decay all slots toward the given time. For each slot:
 *   - if the slot is EMPTY, skip
 *   - dt = now_ns - last_ts_ns (clamped to >= 0)
 *   - factor = pow(0.5, dt / halflife_ns)
 *   - count *= factor, m2 *= factor
 *   - READY slots whose count falls below BASELINE_MIN_SAMPLES go to STALE
 *
 * Called periodically by C5 (default every 30 s), and on save (C4).
 * Safe to call at any frequency; calling it twice in quick succession
 * just applies two small decays that compose to one. */
void baseline_decay(baseline_t *b, uint64_t now_ns);

/* Effective sample count for a slot, after decay. Equivalent to
 * stat->count. Provided as a named function so callers don't think
 * the raw count field is an integer. Returns 0 for NULL. */
float baseline_effective_count(const baseline_stat_t *s);

#endif /* GARDENER_BASELINE_H */