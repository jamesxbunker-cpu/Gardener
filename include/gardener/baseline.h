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

/* The full model: header + per-channel entries + stats. ~130 KB. */
typedef struct {
    baseline_header_t        header;
    baseline_channel_entry_t channels[BASELINE_MAX_CHANNELS];
    baseline_stat_t          stats[BASELINE_MAX_CHANNELS][BASELINE_MAX_REGIMES];

    /* Derived at init time. slot_to_ch_id[i] is the channel id in
     * slot i, or 0 if unused. Channel id 0 is reserved (never valid)
     * so 0 works as a sentinel. */
    uint16_t                 slot_to_ch_id[BASELINE_MAX_CHANNELS];
    uint16_t                 next_slot;    /* next unused slot, or BASELINE_MAX_CHANNELS if full */
} baseline_t;

/* ---- compile-time invariants -------------------------------------- */

_Static_assert(sizeof(baseline_header_t) == BASELINE_HEADER_SIZE,
               "baseline_header_t must be 64 bytes");
_Static_assert(sizeof(baseline_channel_entry_t) == BASELINE_CHANNEL_ENTRY_SIZE,
               "baseline_channel_entry_t must be 8 bytes");
_Static_assert(sizeof(baseline_stat_t) == BASELINE_STAT_SIZE,
               "baseline_stat_t must be 40 bytes");
_Static_assert(BASELINE_FILE_SIZE == BASELINE_HEADER_SIZE
                                    + BASELINE_MAX_CHANNELS * BASELINE_CHANNEL_ENTRY_SIZE
                                    + BASELINE_MAX_CHANNELS * BASELINE_MAX_REGIMES * BASELINE_STAT_SIZE
                                    + 4,
               "file size must match layout");

/* ---- API ---------------------------------------------------------- */

/* Initialize a baseline model. All slots start EMPTY. No registry is
 * needed: channels register lazily as baseline_update sees them.
 *
 * halflife_ns = 0 uses BASELINE_DEFAULT_HALFLIFE_NS. Used only in C3;
 * C1 ignores it.
 *
 * Returns 0 on success, -1 on error (currently: only NULL argument). */
int baseline_init(baseline_t *b, uint64_t halflife_ns);

/* Feed one packet. For every sample:
 *   - find or create a slot for the channel
 *   - update slot 0 (single regime in C1)
 *   - track first/last timestamps, min, max
 *   - transition state: EMPTY → LEARNING at first sample,
 *                       LEARNING → READY at BASELINE_MIN_SAMPLES
 *
 * Returns the number of samples accepted (0 if packet had none),
 * or -1 on error. */
int baseline_update(baseline_t *b, const packet_t *p);

/* Variance of a slot. Returns 0 if count < 2. Uses M2 / (count - 1)
 * (sample variance; not population). */
float baseline_variance(const baseline_stat_t *s);

/* Standard deviation of a slot. */
float baseline_stddev(const baseline_stat_t *s);

/* Lookup: find the stat slot for a channel id, or NULL if the channel
 * has never been seen.
 *
 * In C1, regime is ignored (only slot 0 exists). In C2, regime selects
 * the appropriate slot. The signature is stable across C1–C3. */
const baseline_stat_t *baseline_get(const baseline_t *b, uint16_t ch_id,
                                    uint16_t regime);

/* Number of channels currently registered (have at least one sample). */
uint16_t baseline_channel_count(const baseline_t *b);

#endif /* GARDENER_BASELINE_H */