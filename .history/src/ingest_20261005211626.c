#include "gardener/ingest.h"
#include "gardener/platform.h"

#include <string.h>

int ingest_init(ingest_t *ing, const char *channels_toml) {
    if (!ing) return -1;

    memset(ing, 0, sizeof(*ing));

    if (registry_init(&ing->registry) != 0) {
        plat_log(LOG_ERROR, "ingest: registry_init failed");
        return -1;
    }

    if (channels_toml) {
        if (registry_load_toml(&ing->registry, channels_toml) != 0) {
            plat_log(LOG_ERROR, "ingest: failed to load %s", channels_toml);
            return -1;
        }
    }

    for (size_t i = 0; i < REGISTRY_MAX_CHANNELS; i++) {
        if (ring_init(&ing->ch[i].ring, ing->storage[i], INGEST_RING_CAP) != 0) {
            plat_log(LOG_ERROR, "ingest: ring_init failed for channel %zu", i);
            return -1;
        }
    }

    plat_log(LOG_INFO, "ingest: ready, ring_cap=%d, max_channels=%d",
             (int)INGEST_RING_CAP, (int)REGISTRY_MAX_CHANNELS);
    return 0;
}

/* Normalize a sample against the registry entry for its channel.
 * Fills in type/role if the packet left them zero, validates the
 * channel_id range, and stamps the timestamp if missing. Returns 0 on
 * success, -1 if the sample should be dropped. */
static int normalize_sample(ingest_t *ing, sample_t *s,
                            uint64_t packet_ts, uint16_t packet_flags) {
    if (s->channel_id >= REGISTRY_MAX_CHANNELS) {
        plat_log(LOG_WARN, "ingest: channel id %u out of range",
                 (unsigned)s->channel_id);
        return -1;
    }

    /* Look up; synthesize a placeholder if this is an unknown id. */
    const channel_desc_t *d = registry_get(&ing->registry, s->channel_id);
    if (!d) {
        ing->unknown_channels++;
        d = registry_get_or_default(&ing->registry, s->channel_id);
        if (!d) {
            plat_log(LOG_WARN, "ingest: registry full, dropping channel %u",
                     (unsigned)s->channel_id);
            return -1;
        }
    }

    /* Fill missing per-sample metadata from the descriptor. A well-formed
     * packet carries type and role; a minimal one can omit them and let
     * the registry provide them. */
    if (s->type == 0 && d->type != 0) {
        s->type = (uint8_t)d->type;
    }
    if (s->role == 0 && d->role != ROLE_UNKNOWN) {
        s->role = (uint8_t)d->role;
    }

    /* Timestamp: sample may carry its own, otherwise inherit from packet. */
    if (s->timestamp_ns == 0) {
        s->timestamp_ns = packet_ts;
    }

    /* Propagate SIMULATED flag from the packet if the sample does not
     * already carry one. Useful for detectors that want to down-weight
     * simulated input in a mixed environment. */
    if ((packet_flags & PACKET_FLAG_SIMULATED) &&
        !(s->flags & CH_FLAG_SIMULATED)) {
        s->flags |= CH_FLAG_SIMULATED;
    }

    /* VALID is implicit unless the packet marked the sample stale. */
    if (!(s->flags & CH_FLAG_STALE)) {
        s->flags |= CH_FLAG_VALID;
    }

    return 0;
}

int ingest_feed(ingest_t *ing, const packet_t *p) {
    if (!ing || !p) return -1;
    if (p->channel_count > PACKET_MAX_CH) {
        plat_log(LOG_WARN, "ingest: packet has %u channels, max is %d",
                 (unsigned)p->channel_count, PACKET_MAX_CH);
        return -1;
    }

    ing->packets_seen++;

    int accepted = 0;
    for (uint16_t i = 0; i < p->channel_count; i++) {
        sample_t s = p->samples[i];

        if (normalize_sample(ing, &s, p->timestamp_ns, p->flags) != 0) {
            ing->samples_dropped++;
            continue;
        }

        channel_state_t *cs = &ing->ch[s.channel_id];
        cs->seen = true;
        cs->latest = s;
        ring_push(&cs->ring, &s);
        ing->samples_seen++;
        accepted++;
    }
    return accepted;
}

const sample_t *ingest_latest(const ingest_t *ing, uint16_t channel_id) {
    if (!ing || channel_id >= REGISTRY_MAX_CHANNELS) return NULL;
    if (!ing->ch[channel_id].seen) return NULL;
    return &ing->ch[channel_id].latest;
}

const channel_state_t *ingest_state(const ingest_t *ing, uint16_t channel_id) {
    if (!ing || channel_id >= REGISTRY_MAX_CHANNELS) return NULL;
    return &ing->ch[channel_id];
}

bool ingest_has_channel(const ingest_t *ing, uint16_t channel_id) {
    if (!ing || channel_id >= REGISTRY_MAX_CHANNELS) return false;
    return ing->ch[channel_id].seen;
}