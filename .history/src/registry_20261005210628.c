#include "gardener/registry.h"
#include "gardener/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

/* ---------------------------------------------------------------------
 * A deliberately small TOML subset. It handles the shape we actually
 * write by hand for channels:
 *
 *   [[channel]]
 *   id           = 1
 *   name         = "coolant_temp"
 *   unit         = "C"
 *   type         = "f32"
 *   role         = "sensor"
 *   min          = 0
 *   max          = 150
 *   controllable = false
 *   is_regime    = false
 *
 * Not supported (and we don't need): nested tables, arrays, inline
 * tables, multi-line strings, dates, dotted keys. When we do need more,
 * swap this for a real TOML parser behind the same header.
 * ------------------------------------------------------------------- */

#define LINE_MAX 512

static char *arena_dup(registry_t *r, const char *s) {
    size_t n = strlen(s) + 1;
    if (r->arena_used + n > sizeof(r->arena)) {
        plat_log(LOG_WARN, "registry: arena full, dropping string '%s'", s);
        r->warnings++;
        return NULL;
    }
    char *dst = r->arena + r->arena_used;
    memcpy(dst, s, n);
    r->arena_used += n;
    return dst;
}

static ch_type_t parse_type(const char *s) {
    if (!strcmp(s, "f32"))  return CH_F32;
    if (!strcmp(s, "i32"))  return CH_I32;
    if (!strcmp(s, "u16"))  return CH_U16;
    if (!strcmp(s, "bool")) return CH_BOOL;
    if (!strcmp(s, "enum")) return CH_ENUM;
    return (ch_type_t)-1;
}

static ch_role_t parse_role(const char *s) {
    if (!strcmp(s, "sensor"))   return ROLE_SENSOR;
    if (!strcmp(s, "setpoint")) return ROLE_SETPOINT;
    if (!strcmp(s, "ctrl_out")) return ROLE_CTRL_OUT;
    if (!strcmp(s, "actuator")) return ROLE_ACTUATOR;
    if (!strcmp(s, "status"))   return ROLE_STATUS;
    if (!strcmp(s, "counter"))  return ROLE_COUNTER;
    return (ch_role_t)-1;
}

static int parse_bool(const char *s, int *out) {
    if (!strcmp(s, "true"))  { *out = 1; return 0; }
    if (!strcmp(s, "false")) { *out = 0; return 0; }
    return -1;
}

/* Strip whitespace from both ends in place. Returns pointer to start. */
static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n-1])) s[--n] = 0;
    return s;
}

/* Strip surrounding double quotes in place. Returns pointer to start. */
static char *unquote(char *s) {
    if (*s != '"') return s;
    s++;
    size_t n = strlen(s);
    if (n && s[n-1] == '"') s[n-1] = 0;
    return s;
}

int registry_init(registry_t *r) {
    if (!r) return -1;
    memset(r, 0, sizeof(*r));
    return 0;
}

/* Apply one "key = value" pair to a channel descriptor. Returns 0 on
 * success, -1 if the key is unknown or the value is malformed. */
static int apply_kv(registry_t *r, channel_desc_t *c, const char *key,
                    const char *val) {
    if (!strcmp(key, "id")) {
        long v = strtol(val, NULL, 10);
        if (v < 0 || v > 65535) {
            plat_log(LOG_WARN, "registry: bad id '%s'", val);
            return -1;
        }
        c->id = (uint16_t)v;
    } else if (!strcmp(key, "name")) {
        c->name = arena_dup(r, val);
    } else if (!strcmp(key, "unit")) {
        c->unit = arena_dup(r, val);
    } else if (!strcmp(key, "type")) {
        ch_type_t t = parse_type(val);
        if ((int)t == -1) {
            plat_log(LOG_WARN, "registry: unknown type '%s'", val);
            return -1;
        }
        c->type = t;
    } else if (!strcmp(key, "role")) {
        ch_role_t t = parse_role(val);
        if ((int)t == -1) {
            plat_log(LOG_WARN, "registry: unknown role '%s'", val);
            return -1;
        }
        c->role = t;
    } else if (!strcmp(key, "controllable")) {
        int b;
        if (parse_bool(val, &b) != 0) {
            plat_log(LOG_WARN, "registry: bad bool '%s' for controllable", val);
            return -1;
        }
        c->controllable = b ? true : false;
    } else if (!strcmp(key, "is_regime")) {
        int b;
        if (parse_bool(val, &b) != 0) {
            plat_log(LOG_WARN, "registry: bad bool '%s' for is_regime", val);
            return -1;
        }
        c->is_regime = b ? true : false;
    } else if (!strcmp(key, "min")) {
        c->min_valid = strtof(val, NULL);
        c->has_range = true;
    } else if (!strcmp(key, "max")) {
        c->max_valid = strtof(val, NULL);
        c->has_range = true;
    } else {
        plat_log(LOG_WARN, "registry: unknown key '%s'", key);
        return -1;
    }
    return 0;
}

/* Final validation once a [[channel]] block ends. Called both when the
 * next block starts and at EOF. Returns 0 if the block is usable, -1
 * if it should be dropped. */
static int finalize_channel(registry_t *r, channel_desc_t *c) {
    if (c->name == NULL) {
        plat_log(LOG_WARN, "registry: channel %u has no name, dropping", c->id);
        r->warnings++;
        return -1;
    }
    if (c->role == ROLE_UNKNOWN) {
        plat_log(LOG_WARN, "registry: channel '%s' has no role, defaulting to unknown",
                 c->name);
        r->warnings++;
    }
    return 0;
}

int registry_load_toml(registry_t *r, const char *path) {
    if (!r || !path) return -1;

    FILE *f = fopen(path, "r");
    if (!f) {
        plat_log(LOG_ERROR, "registry: cannot open %s: %s", path, strerror(errno));
        return -1;
    }

    char line[LINE_MAX];
    channel_desc_t *cur = NULL;
    int lineno = 0;
    int dropped = 0;

    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *p = trim(line);
        if (*p == 0 || *p == '#' || *p == ';') continue;

        if (*p == '[') {
            /* End the previous block before starting a new one. */
            if (cur) {
                if (finalize_channel(r, cur) != 0) {
                    dropped++;
                }
                cur = NULL;
            }

            if (strncmp(p, "[[channel]]", 11) != 0) {
                plat_log(LOG_WARN, "registry: %s:%d unknown table '%s'",
                         path, lineno, p);
                r->warnings++;
                continue;
            }
            if (r->count >= REGISTRY_MAX_CHANNELS) {
                plat_log(LOG_WARN, "registry: too many channels, ignoring rest");
                r->warnings++;
                continue;
            }
            cur = &r->entries[r->count];
            memset(cur, 0, sizeof(*cur));
            cur->type     = CH_F32;
            cur->role     = ROLE_UNKNOWN;
            cur->min_valid = 0.0f;
            cur->max_valid = 0.0f;
            cur->has_range = false;
            r->count++;
            continue;
        }

        if (!cur) {
            plat_log(LOG_WARN, "registry: %s:%d key outside any table",
                     path, lineno);
            r->warnings++;
            continue;
        }

        /* key = value */
        char *eq = strchr(p, '=');
        if (!eq) {
            plat_log(LOG_WARN, "registry: %s:%d malformed line", path, lineno);
            r->warnings++;
            continue;
        }
        *eq = 0;
        char *key = trim(p);
        char *val = trim(eq + 1);
        val = unquote(val);

        if (apply_kv(r, cur, key, val) != 0) {
            r->warnings++;
        }
    }

    if (cur) {
        if (finalize_channel(r, cur) != 0) {
            dropped++;
        }
    }

    fclose(f);

    /* Drop any blocks that failed finalize by compacting the array. */
    if (dropped) {
        size_t w = 0;
        for (size_t i = 0; i < r->count; i++) {
            if (r->entries[i].name != NULL) {
                if (w != i) r->entries[w] = r->entries[i];
                w++;
            }
        }
        r->count = w;
    }

    plat_log(LOG_INFO, "registry: loaded %zu channels from %s (%zu warnings)",
             r->count, path, r->warnings);
    return 0;
}

const channel_desc_t *registry_get(const registry_t *r, uint16_t id) {
    if (!r) return NULL;
    for (size_t i = 0; i < r->count; i++) {
        if (r->entries[i].id == id) return &r->entries[i];
    }
    return NULL;
}

const channel_desc_t *registry_get_or_default(registry_t *r, uint16_t id) {
    if (!r) return NULL;
    const channel_desc_t *d = registry_get(r, id);
    if (d) return d;
    if (r->count >= REGISTRY_MAX_CHANNELS) return NULL;

    channel_desc_t *n = &r->entries[r->count++];
    memset(n, 0, sizeof(*n));
    n->id     = id;
    n->type   = CH_F32;
    n->role   = ROLE_UNKNOWN;
    n->min_valid = 0.0f;
    n->max_valid = 0.0f;

    char tmp[32];
    snprintf(tmp, sizeof(tmp), "unknown_%u", (unsigned)id);
    n->name = arena_dup(r, tmp);

    plat_log(LOG_WARN, "registry: unknown channel %u, synthesized '%s'",
             (unsigned)id, tmp);
    r->warnings++;
    return n;
}