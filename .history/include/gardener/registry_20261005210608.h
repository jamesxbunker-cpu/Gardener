#ifndef GARDENER_REGISTRY_H
#define GARDENER_REGISTRY_H

#include "gardener/channel.h"
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------
 * Channel registry.
 *
 * Loads channel descriptors from a config file and provides id -> desc
 * lookup. Every other module that needs to interpret a raw channel id
 * goes through here; nothing else should be looking at the config file
 * directly.
 *
 * Strings in a channel_desc_t are borrowed from the registry's internal
 * arena and live as long as the registry itself. Do not free them, do
 * not store them past the registry's lifetime.
 * ------------------------------------------------------------------- */

#define REGISTRY_MAX_CHANNELS 256
#define REGISTRY_ARENA_BYTES  (REGISTRY_MAX_CHANNELS * 64)

typedef struct {
    channel_desc_t entries[REGISTRY_MAX_CHANNELS];
    size_t         count;

    /* Backing store for the borrowed name/unit strings. Allocated
     * inline so the registry itself is trivially copyable and needs no
     * heap. */
    char           arena[REGISTRY_ARENA_BYTES];
    size_t         arena_used;

    /* Stats for the load: how many entries parsed, how many warnings
     * were emitted. Useful in the dump tool and later in the events log. */
    size_t         warnings;
} registry_t;

/* Zero a registry. Must be called before registry_load_*. */
int registry_init(registry_t *r);

/* Load channel descriptors from a TOML file. Returns 0 on success, -1
 * on hard errors (cannot open, cannot parse the header). Individual bad
 * channel entries log a warning, increment r->warnings, and are skipped
 * rather than failing the whole load. */
int registry_load_toml(registry_t *r, const char *path);

/* Look up a channel by id. Returns NULL if unknown. */
const channel_desc_t *registry_get(const registry_t *r, uint16_t id);

/* Look up a channel; if unknown, synthesize a placeholder entry with
 * role=ROLE_UNKNOWN, type=CH_F32, and a generated name, then return it.
 * Logs a warning and bumps warnings. Returns NULL only if the registry
 * is full. */
const channel_desc_t *registry_get_or_default(registry_t *r, uint16_t id);

#endif /* GARDENER_REGISTRY_H */