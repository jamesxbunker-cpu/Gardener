#include "gardener/baseline.h"
#include "gardener/registry.h"
#include "gardener/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------
 * gardener-stats — read-only inspector for baseline files.
 *
 *   gardener-stats --file <baseline.bin> [options]
 *
 * Options:
 *   --file <path>       baseline file to read (required)
 *   --registry <path>   optional; resolves channel names for display
 *   --channel <id>      only show this channel id
 *   --regime <n>        only show this regime index
 *   --jsonl             machine-readable output
 *   --ready-only        only show READY slots
 *   --quiet             suppress warnings
 *   --help, -h          this message
 *
 * Exit codes:
 *   0   printed successfully
 *   1   could not load file
 *   2   usage error
 * ------------------------------------------------------------------- */

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s --file <baseline.bin> [options]\n"
        "\n"
        "Options:\n"
        "  --file <path>       baseline file to read (required)\n"
        "  --registry <path>   optional; resolves channel names\n"
        "  --channel <id>      only show this channel id\n"
        "  --regime <n>        only show this regime index\n"
        "  --jsonl             machine-readable output\n"
        "  --ready-only        only show READY slots\n"
        "  --quiet             suppress warnings\n"
        "  --help, -h          this message\n",
        prog);
}

static const char *state_name(baseline_state_t s) {
    switch (s) {
        case BASELINE_EMPTY:    return "EMPTY";
        case BASELINE_LEARNING: return "LEARNING";
        case BASELINE_READY:    return "READY";
        case BASELINE_STALE:    return "STALE";
        default:                return "?";
    }
}

/* Find a channel's name from the registry, or NULL. */
static const char *lookup_name(const registry_t *reg, uint16_t id) {
    if (!reg) return NULL;
    const channel_desc_t *c = registry_get(reg, id);
    return c ? c->name : NULL;
}

/* Look up the halflife in seconds for display. */
static double halflife_sec(const baseline_t *b) {
    if (b->header.halflife_ns == 0) return 0.0;
    return (double)b->header.halflife_ns / 1e9;
}

static void print_header(const baseline_t *b, const char *path,
                         const registry_t *reg) {
    printf("file:       %s\n", path ? path : "(null)");
    printf("magic:      0x%08X\n", (unsigned)b->header.magic);
    printf("version:    %u\n", (unsigned)b->header.version);
    printf("created:    %llu\n", (unsigned long long)b->header.created_ns);
    printf("saved:      %llu\n", (unsigned long long)b->header.saved_ns);
    printf("halflife:   %.1fs\n", halflife_sec(b));
    printf("channels:   %u\n", (unsigned)baseline_channel_count(b));
    printf("regimes:    %u\n", (unsigned)baseline_regime_count(b));

    if (b->header.regime_ch_count > 0) {
        printf("regime_ch:  ");
        for (uint8_t i = 0; i < b->header.regime_ch_count; i++) {
            uint16_t id = b->header.regime_ch_ids[i];
            const char *name = lookup_name(reg, id);
            if (i) printf(", ");
            if (name) printf("%s(id=%u)", name, (unsigned)id);
            else      printf("id=%u", (unsigned)id);
        }
        printf("\n");
    } else {
        printf("regime_ch:  (none; single-regime model)\n");
    }
}

/* ---- table mode ---- */

static void print_table(const baseline_t *b, const registry_t *reg,
                        int only_channel, uint16_t ch_filter,
                        int only_regime, uint16_t regime_filter,
                        int ready_only) {
    printf("\n");

    /* Column headers. Widths tuned for typical values. */
    printf("%-6s %-20s %-7s %-9s %-8s %-10s %-10s %-10s %-10s\n",
           "ch", "name", "regime", "state", "count", "mean",
           "stddev", "min", "max");
    printf("%-6s %-20s %-7s %-9s %-8s %-10s %-10s %-10s %-10s\n",
           "------", "--------------------", "-------", "---------",
           "--------", "----------", "----------", "----------",
           "----------");

    int printed = 0;
    for (uint16_t slot = 0; slot < b->next_slot; slot++) {
        uint16_t ch_id = b->channels[slot].ch_id;
        if (ch_id == 0) continue;
        if (only_channel && ch_id != ch_filter) continue;

        const char *name = lookup_name(reg, ch_id);
        char name_buf[32];
        if (!name) {
            snprintf(name_buf, sizeof(name_buf), "(id %u)", (unsigned)ch_id);
            name = name_buf;
        }

        for (uint16_t r = 0; r < BASELINE_MAX_REGIMES; r++) {
            const baseline_stat_t *s = &b->stats[slot][r];
            if (s->state == BASELINE_EMPTY) continue;
            if (only_regime && r != regime_filter) continue;
            if (ready_only && s->state != BASELINE_READY) continue;

            printf("%-6u %-20s %-7u %-9s %-8.1f %-10.3f %-10.3f "
                   "%-10.3f %-10.3f\n",
                   (unsigned)ch_id,
                   name,
                   (unsigned)r,
                   state_name((baseline_state_t)s->state),
                   (double)s->count,
                   (double)s->mean,
                   (double)baseline_stddev(s),
                   (double)s->min_val,
                   (double)s->max_val);
            printed++;
        }
    }

    if (printed == 0) {
        printf("(no matching slots)\n");
    }
    printf("\n%u slot(s) printed\n", (unsigned)printed);
}

/* ---- jsonl mode ---- */

static void print_jsonl(const baseline_t *b, const registry_t *reg,
                        int only_channel, uint16_t ch_filter,
                        int only_regime, uint16_t regime_filter,
                        int ready_only) {
    for (uint16_t slot = 0; slot < b->next_slot; slot++) {
        uint16_t ch_id = b->channels[slot].ch_id;
        if (ch_id == 0) continue;
        if (only_channel && ch_id != ch_filter) continue;

        const char *name = lookup_name(reg, ch_id);

        for (uint16_t r = 0; r < BASELINE_MAX_REGIMES; r++) {
            const baseline_stat_t *s = &b->stats[slot][r];
            if (s->state == BASELINE_EMPTY) continue;
            if (only_regime && r != regime_filter) continue;
            if (ready_only && s->state != BASELINE_READY) continue;

            if (name) {
                printf("{\"ch\":%u,\"name\":\"%s\",\"regime\":%u,"
                       "\"state\":\"%s\",\"count\":%g,\"mean\":%g,"
                       "\"stddev\":%g,\"min\":%g,\"max\":%g,"
                       "\"first_ts_ns\":%llu,\"last_ts_ns\":%llu}\n",
                       (unsigned)ch_id, name, (unsigned)r,
                       state_name((baseline_state_t)s->state),
                       (double)s->count, (double)s->mean,
                       (double)baseline_stddev(s),
                       (double)s->min_val, (double)s->max_val,
                       (unsigned long long)s->first_ts_ns,
                       (unsigned long long)s->last_ts_ns);
            } else {
                printf("{\"ch\":%u,\"regime\":%u,"
                       "\"state\":\"%s\",\"count\":%g,\"mean\":%g,"
                       "\"stddev\":%g,\"min\":%g,\"max\":%g,"
                       "\"first_ts_ns\":%llu,\"last_ts_ns\":%llu}\n",
                       (unsigned)ch_id, (unsigned)r,
                       state_name((baseline_state_t)s->state),
                       (double)s->count, (double)s->mean,
                       (double)baseline_stddev(s),
                       (double)s->min_val, (double)s->max_val,
                       (unsigned long long)s->first_ts_ns,
                       (unsigned long long)s->last_ts_ns);
            }
        }
    }
}

/* ---- main ---- */

int main(int argc, char **argv) {
    const char *file_path = NULL;
    const char *reg_path = NULL;
    int only_channel = 0;
    uint16_t ch_filter = 0;
    int only_regime = 0;
    uint16_t regime_filter = 0;
    int jsonl = 0;
    int ready_only = 0;
    int quiet = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if      (!strcmp(a, "--file") && i + 1 < argc)     file_path = argv[++i];
        else if (!strcmp(a, "--registry") && i + 1 < argc) reg_path = argv[++i];
        else if (!strcmp(a, "--channel") && i + 1 < argc) {
            ch_filter = (uint16_t)strtoul(argv[++i], NULL, 10);
            only_channel = 1;
        }
        else if (!strcmp(a, "--regime") && i + 1 < argc) {
            regime_filter = (uint16_t)strtoul(argv[++i], NULL, 10);
            only_regime = 1;
        }
        else if (!strcmp(a, "--jsonl"))      jsonl = 1;
        else if (!strcmp(a, "--ready-only")) ready_only = 1;
        else if (!strcmp(a, "--quiet"))      quiet = 1;
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(argv[0]); return 0;
        }
        else {
            fprintf(stderr, "unknown argument: %s\n", a);
            usage(argv[0]);
            return 2;
        }
    }

    if (!file_path) { usage(argv[0]); return 2; }
    if (quiet) plat_log_set_level(LOG_ERROR);

    /* Optional registry for name resolution. */
    static registry_t reg;
    int have_reg = 0;
    if (reg_path) {
        if (registry_init(&reg) == 0 &&
            registry_load_toml(&reg, reg_path) == 0) {
            have_reg = 1;
        } else {
            plat_log(LOG_WARN,
                     "could not load registry %s; showing channel ids only",
                     reg_path);
        }
    }

    /* Load the baseline. The static buffer is large; use static
     * storage to avoid a stack overflow. */
    static baseline_t b;
    if (baseline_load(&b, file_path, have_reg ? &reg : NULL) != 0) {
        plat_log(LOG_ERROR, "cannot load baseline %s", file_path);
        return 1;
    }

    if (jsonl) {
        print_jsonl(&b, have_reg ? &reg : NULL,
                    only_channel, ch_filter,
                    only_regime, regime_filter,
                    ready_only);
    } else {
        print_header(&b, file_path, have_reg ? &reg : NULL);
        print_table(&b, have_reg ? &reg : NULL,
                    only_channel, ch_filter,
                    only_regime, regime_filter,
                    ready_only);
    }
    return 0;
}