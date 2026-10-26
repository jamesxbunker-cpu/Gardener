#include "gardener/baseline.h"
#include "gardener/platform.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

/* ---------------------------------------------------------------------
 * Baseline model, C3: per-channel, per-regime Welford statistics with
 * exponential decay.
 *
 * In C3, a "regime" is derived from the is_regime channels configured
 * in the registry. Their current values are hashed to produce a
 * regime index in [0, BASELINE_MAX_REGIMES). Every (channel, regime)
 * pair maintains its own Welford statistics.
 *
 * Decay is explicit: baseline_decay(b, now_ns) ages all slots by the
 * elapsed time since their last update. baseline_update does NOT
 * decay; it only accumulates. This keeps "count" a clean measure of
 * samples fed and makes decay a separate, testable operation.
 *
 * No regime channels → single regime 0. This preserves the C1
 * behavior for systems that don't use regimes.
 * ------------------------------------------------------------------- */

/* MurmurHash3 finalizer. Spreads the low bits of an input hash across
 * the full 32-bit output. Required whenever a hash is taken modulo a
 * power of two, since the modulo discards the high bits. */
static uint32_t mix32(uint32_t h) {
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

int baseline_init(baseline_t *b, const registry_t *registry,
                  uint64_t halflife_ns) {
    if (!b) return -1;

    memset(b, 0, sizeof(*b));

    b->header.magic          = BASELINE_MAGIC;
    b->header.version        = BASELINE_VERSION;
    b->header.header_size    = BASELINE_HEADER_SIZE;
    b->header.channel_count  = 0;
    b->header.regime_count   = 0;
    b->header.created_ns     = plat_wall_ns();
    b->header.saved_ns       = 0;
    b->header.halflife_ns    = halflife_ns ? halflife_ns
                                           : BASELINE_DEFAULT_HALFLIFE_NS;

    b->next_slot = 0;
    b->regime_ch_count = 0;

    /* Collect regime channels from the registry. */
    if (registry) {
        for (size_t i = 0; i < registry->count; i++) {
            const channel_desc_t *c = &registry->entries[i];
            if (!c->is_regime) continue;
            if (b->regime_ch_count >= BASELINE_MAX_REGIME_CH) {
                plat_log(LOG_WARN,
                         "baseline: ignoring regime channel '%s' "
                         "(max %d already collected)",
                         c->name ? c->name : "?",
                         BASELINE_MAX_REGIME_CH);
                continue;
            }
            b->regime_ch_ids[b->regime_ch_count] = c->id;
            b->header.regime_ch_ids[b->regime_ch_count] = c->id;
            b->regime_ch_count++;
        }
        b->header.regime_ch_count = b->regime_ch_count;
    }

    return 0;
}

static uint16_t find_or_register_slot(baseline_t *b, uint16_t ch_id) {
    for (uint16_t i = 0; i < b->next_slot; i++) {
        if (b->slot_to_ch_id[i] == ch_id) return i;
    }
    if (b->next_slot >= BASELINE_MAX_CHANNELS) return 0xFFFF;
    uint16_t slot = b->next_slot++;
    b->slot_to_ch_id[slot] = ch_id;
    b->channels[slot].ch_id = ch_id;
    b->header.channel_count++;
    return slot;
}

static uint16_t find_slot(const baseline_t *b, uint16_t ch_id) {
    for (uint16_t i = 0; i < b->next_slot; i++) {
        if (b->slot_to_ch_id[i] == ch_id) return i;
    }
    return 0xFFFF;
}

static const sample_t *find_sample(const packet_t *p, uint16_t ch_id) {
    for (uint16_t i = 0; i < p->channel_count; i++) {
        if (p->samples[i].channel_id == ch_id) return &p->samples[i];
    }
    return NULL;
}

uint16_t baseline_regime_for_packet(const baseline_t *b, const packet_t *p) {
    if (!b || !p) return 0xFFFF;
    if (b->regime_ch_count == 0) return 0;

    /* Mix each regime channel's quantized value independently, then
     * XOR the results. This gives every channel's value full influence
     * over every bit of the output. FNV on short inputs has poor
     * low-bit distribution, which matters when the modulus is a power
     * of two. */
    uint32_t h = 0x811C9DC5u;
    for (uint8_t i = 0; i < b->regime_ch_count; i++) {
        const sample_t *s = find_sample(p, b->regime_ch_ids[i]);
        if (!s) return 0xFFFF;
        float v = sample_as_f32(s);
        int32_t q = (int32_t)(v * 1000.0f);

        h ^= mix32((uint32_t)q);
        h = mix32(h);
    }
    return (uint16_t)(h % BASELINE_MAX_REGIMES);
}

static void welford_update(baseline_stat_t *s, float v, uint64_t ts_ns) {
    if (s->state == BASELINE_EMPTY) {
        s->count       = 1.0f;
        s->mean        = v;
        s->m2          = 0.0f;
        s->min_val     = v;
        s->max_val     = v;
        s->first_ts_ns = ts_ns;
        s->last_ts_ns  = ts_ns;
        s->state       = BASELINE_LEARNING;
        return;
    }

    s->count += 1.0f;
    float delta = v - s->mean;
    s->mean += delta / s->count;
    float delta2 = v - s->mean;
    s->m2 += delta * delta2;

    if (v < s->min_val) s->min_val = v;
    if (v > s->max_val) s->max_val = v;

    if (ts_ns < s->first_ts_ns) s->first_ts_ns = ts_ns;
    if (ts_ns > s->last_ts_ns)  s->last_ts_ns  = ts_ns;

    /* LEARNING or STALE both transition to READY when count crosses
     * the threshold. EMPTY was handled above. READY stays READY. */
    if ((s->state == BASELINE_LEARNING || s->state == BASELINE_STALE) &&
        s->count >= (float)BASELINE_MIN_SAMPLES) {
        s->state = BASELINE_READY;
    }
}

int baseline_update(baseline_t *b, const packet_t *p) {
    if (!b || !p) return -1;

    uint16_t regime = baseline_regime_for_packet(b, p);
    if (regime == 0xFFFF) return 0;

    int accepted = 0;
    for (uint16_t i = 0; i < p->channel_count; i++) {
        const sample_t *sample = &p->samples[i];

        if (sample->flags & CH_FLAG_STALE) continue;
        if (!(sample->flags & CH_FLAG_VALID)) continue;

        uint16_t slot = find_or_register_slot(b, sample->channel_id);
        if (slot == 0xFFFF) continue;

        uint64_t ts = sample->timestamp_ns ? sample->timestamp_ns
                                           : p->timestamp_ns;

        baseline_stat_t *stat = &b->stats[slot][regime];
        int was_empty = (stat->state == BASELINE_EMPTY);
        welford_update(stat, sample_as_f32(sample), ts);
        accepted++;

        if (was_empty) b->regime_sample_count[regime]++;
        if (regime + 1 > b->channels[slot].n_regimes_used) {
            b->channels[slot].n_regimes_used = regime + 1;
        }
    }

    uint16_t rc = 0;
    for (uint16_t i = 0; i < BASELINE_MAX_REGIMES; i++) {
        if (b->regime_sample_count[i] > 0) rc++;
    }
    b->header.regime_count = rc;

    return accepted;
}

static void decay_slot(baseline_stat_t *s, uint64_t dt_ns,
                       uint64_t halflife_ns) {
    if (s->state == BASELINE_EMPTY) return;
    if (dt_ns == 0 || halflife_ns == 0) return;
    if (s->count <= 0.0f) return;

    /* factor = 2 ^ (-dt / halflife). Compute as exp(-ln2 * dt/hl). */
    double exponent = -0.6931471805599453 * ((double)dt_ns / (double)halflife_ns);
    double factor_d = exp(exponent);
    if (factor_d > 1.0) factor_d = 1.0;
    if (factor_d < 0.0) factor_d = 0.0;
    float factor = (float)factor_d;

    s->count *= factor;
    s->m2    *= factor;

    /* Extend last_ts_ns so the next decay call measures from here,
     * not from the last sample's timestamp. */
    s->last_ts_ns += dt_ns;

    /* READY slots that decay below the threshold go STALE. */
    if (s->state == BASELINE_READY &&
        s->count < (float)BASELINE_MIN_SAMPLES) {
        s->state = BASELINE_STALE;
    }
}

void baseline_decay(baseline_t *b, uint64_t now_ns) {
    if (!b) return;
    uint64_t halflife = b->header.halflife_ns;
    if (halflife == 0) return;

    for (uint16_t slot = 0; slot < b->next_slot; slot++) {
        for (uint16_t r = 0; r < BASELINE_MAX_REGIMES; r++) {
            baseline_stat_t *s = &b->stats[slot][r];
            if (s->state == BASELINE_EMPTY) continue;
            if (now_ns <= s->last_ts_ns) continue;
            uint64_t dt = now_ns - s->last_ts_ns;
            decay_slot(s, dt, halflife);
        }
    }
}

float baseline_effective_count(const baseline_stat_t *s) {
    if (!s) return 0.0f;
    return s->count;
}

float baseline_variance(const baseline_stat_t *s) {
    if (!s) return 0.0f;
    if (s->count < 2.0f) return 0.0f;
    return s->m2 / (s->count - 1.0f);
}

float baseline_stddev(const baseline_stat_t *s) {
    float v = baseline_variance(s);
    if (v <= 0.0f) return 0.0f;
    return sqrtf(v);
}

const baseline_stat_t *baseline_get(const baseline_t *b, uint16_t ch_id,
                                    uint16_t regime) {
    if (!b) return NULL;
    if (regime >= BASELINE_MAX_REGIMES) return NULL;
    uint16_t slot = find_slot(b, ch_id);
    if (slot == 0xFFFF) return NULL;
    const baseline_stat_t *s = &b->stats[slot][regime];
    if (s->state == BASELINE_EMPTY) return NULL;
    return s;
}

uint16_t baseline_channel_count(const baseline_t *b) {
    return b ? b->next_slot : 0;
}

uint16_t baseline_regime_count(const baseline_t *b) {
    return b ? b->header.regime_count : 0;
}

/* ---------------------------------------------------------------------
 * Persistence.
 *
 * File layout (see baseline.h for constants):
 *   [0, 64)             header (baseline_header_t)
 *   [64, 2112)          channel table (256 × 8 bytes)
 *   [2112, 165952)      stats table (256 × 16 × 40 bytes)
 *   [165952, 165956)    trailer (u32 CRC over [0, 165952))
 *
 * Save: write header (placeholder CRC) + channel table + stats +
 * trailer. Fsync. Rewrite header with final saved_ns and CRC. Fsync.
 * Rename. All within <path>.tmp before the rename makes the final
 * path point at the new content.
 *
 * Load: read header. Validate magic/version/header_size. Read whole
 * file. Verify CRC. Copy into *b. Rebuild derived fields.
 * ------------------------------------------------------------------- */

static int write_all(plat_file_t *f, const void *buf, size_t len) {
    return plat_file_write(f, buf, len) == (int)len ? 0 : -1;
}

int baseline_save(baseline_t *b, const char *path) {
    if (!b || !path) return -1;

    /* Build the full file in memory, then write it in one go. The
     * model is ~166 KB, so this is fine. */
    static uint8_t image[BASELINE_FILE_SIZE];

    /* Update header fields that change on save. */
    b->header.saved_ns = plat_wall_ns();

    /* Serialize header, channel table, and stats directly. The structs
     * are already packed and little-endian (assuming LE host; on BE
     * this would need byte-by-byte writes, which we'll add if we ever
     * port to a BE platform). */
    memcpy(image, &b->header, sizeof(b->header));
    memcpy(image + BASELINE_CHANNEL_TABLE_OFF,
           b->channels, sizeof(b->channels));
    memcpy(image + BASELINE_STATS_OFF,
           b->stats, sizeof(b->stats));

    /* Compute CRC over everything before the trailer, and write it. */
    uint16_t crc = packet_crc16(image, BASELINE_TRAILER_OFF);
    image[BASELINE_TRAILER_OFF + 0] = (uint8_t)(crc & 0xFF);
    image[BASELINE_TRAILER_OFF + 1] = (uint8_t)((crc >> 8) & 0xFF);
    /* Trailer is 4 bytes in the format spec but CRC is 2 bytes; the
     * upper 2 bytes are reserved zero. */
    image[BASELINE_TRAILER_OFF + 2] = 0;
    image[BASELINE_TRAILER_OFF + 3] = 0;

    /* Write to a temp file. */
    char tmp_path[512];
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path)
            >= (int)sizeof(tmp_path)) return -1;

    plat_file_t *f = plat_file_open(tmp_path, PLAT_FILE_WRITE);
    if (!f) return -1;

    if (write_all(f, image, sizeof(image)) != 0) {
        plat_file_close(f);
        remove(tmp_path);
        return -1;
    }
    plat_file_sync(f);
    plat_file_close(f);

    /* Atomic rename. POSIX rename() is atomic; on Windows, MoveFileEx
     * with MOVEFILE_REPLACE_EXISTING is atomic. The platform layer
     * doesn't expose rename yet, so we do remove + rename, which is
     * not atomic but preserves the old file on failure of the rename
     * half. A proper plat_file_rename is C5 or later. */
    remove(path);
    if (rename(tmp_path, path) != 0) {
        remove(tmp_path);
        return -1;
    }
    return 0;
}

int baseline_load(baseline_t *b, const char *path,
                  const registry_t *registry) {
    if (!b || !path) return -1;

    /* Zero up front so any failure leaves *b in a clean state. This
     * matches the header contract ("leaves *b zeroed — no partial
     * load"). The success path fills it in below. */
    memset(b, 0, sizeof(*b));

    plat_file_t *f = plat_file_open(path, PLAT_FILE_READ);
    if (!f) return -1;

    static uint8_t image[BASELINE_FILE_SIZE];
    int got = plat_file_read(f, image, sizeof(image));
    plat_file_close(f);
    if (got != (int)sizeof(image)) return -1;

    /* Validate header. */
    baseline_header_t hdr;
    memcpy(&hdr, image, sizeof(hdr));
    if (hdr.magic != BASELINE_MAGIC) return -1;
    if (hdr.version != BASELINE_VERSION) return -1;
    if (hdr.header_size != BASELINE_HEADER_SIZE) return -1;
    if (hdr.channel_count > BASELINE_MAX_CHANNELS) return -1;
    if (hdr.regime_count > BASELINE_MAX_REGIMES) return -1;

    /* Validate CRC. */
    uint16_t crc_stored = (uint16_t)(image[BASELINE_TRAILER_OFF + 0]
                                   | (image[BASELINE_TRAILER_OFF + 1] << 8));
    uint16_t crc_calc = packet_crc16(image, BASELINE_TRAILER_OFF);
    if (crc_stored != crc_calc) return -1;

    /* Everything validates. Populate *b. */
    memcpy(&b->header, &hdr, sizeof(hdr));
    memcpy(b->channels, image + BASELINE_CHANNEL_TABLE_OFF,
           sizeof(b->channels));
    memcpy(b->stats, image + BASELINE_STATS_OFF,
           sizeof(b->stats));

    /* Rebuild derived fields. */
    b->next_slot = 0;
    for (uint16_t i = 0; i < BASELINE_MAX_CHANNELS; i++) {
        if (b->channels[i].ch_id != 0) {
            b->slot_to_ch_id[i] = b->channels[i].ch_id;
            b->next_slot = i + 1;
        }
    }

    b->regime_ch_count = b->header.regime_ch_count;
    for (uint8_t i = 0; i < b->regime_ch_count &&
                        i < BASELINE_MAX_REGIME_CH; i++) {
        b->regime_ch_ids[i] = b->header.regime_ch_ids[i];
    }

    for (uint16_t r = 0; r < BASELINE_MAX_REGIMES; r++) {
        b->regime_sample_count[r] = 0;
    }
    for (uint16_t slot = 0; slot < b->next_slot; slot++) {
        for (uint16_t r = 0; r < BASELINE_MAX_REGIMES; r++) {
            if (b->stats[slot][r].state != BASELINE_EMPTY) {
                b->regime_sample_count[r] = 1;
            }
        }
    }

    (void)registry;
    return 0;
}