#include "gardener/ingest.h"
#include "gardener/packet.h"
#include "gardener/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

/* ---------------------------------------------------------------------
 * gardener-dump — two modes:
 *
 *   gardener-dump --config <channels.toml>
 *     Load the registry and print the channel table. No feed.
 *
 *   gardener-dump --feed <packets.jsonl> [--channels <channels.toml>]
 *     Load the registry, feed every JSONL packet from the file through
 *     ingest, then print a snapshot table: latest value per channel,
 *     with optional sample counts.
 *
 * Options:
 *   --channels <path>   channel registry (default: configs/channels.toml)
 *   --counts            include per-channel sample count column
 *   --quiet             suppress warnings from ingest (unknown channels)
 *   --help, -h          print usage
 *
 * Exit code: 0 ok, 1 hard error (missing file), 2 usage error.
 * ------------------------------------------------------------------- */

#define LINE_MAX 8192

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s --config <channels.toml>\n"
        "       %s --feed <packets.jsonl> [--channels <channels.toml>] [--counts]\n"
        "\n"
        "Without --feed, prints the channel registry table.\n"
        "With --feed, reads JSONL packets, feeds them through ingest, and\n"
        "prints the resulting snapshot.\n",
        prog, prog);
}

/* ---------- table printers ---------- */

static void print_registry_table(const registry_t *r) {
    printf("%-6s %-20s %-6s %-9s %-6s %-6s %s\n",
           "id", "name", "type", "role", "ctrl", "regime", "range");
    printf("%-6s %-20s %-6s %-9s %-6s %-6s %s\n",
           "------", "--------------------",
           "------", "---------",
           "------", "------", "-----");

    for (size_t i = 0; i < r->count; i++) {
        const channel_desc_t *c = &r->entries[i];

        char range[32] = "-";
        if (c->has_range) {
            snprintf(range, sizeof(range), "[%g, %g]",
                     (double)c->min_valid, (double)c->max_valid);
        }

        printf("%-6u %-20s %-6s %-9s %-6s %-6s %s\n",
               (unsigned)c->id,
               c->name ? c->name : "(null)",
               ch_type_name(c->type),
               ch_role_name(c->role),
               c->controllable ? "yes" : "no",
               c->is_regime    ? "yes" : "no",
               range);
    }
}

/* Format a sample's value into `out`. Bool prints as true/false, enum
 * as an integer, floats with %g. */
static void format_value(const sample_t *s, char *out, size_t cap) {
    switch (s->type) {
        case CH_F32:  snprintf(out, cap, "%g", (double)s->v.f); break;
        case CH_I32:  snprintf(out, cap, "%d", s->v.i); break;
        case CH_U16:  snprintf(out, cap, "%u", (unsigned)s->v.u); break;
        case CH_BOOL: snprintf(out, cap, "%s", s->v.b ? "true" : "false"); break;
        case CH_ENUM: snprintf(out, cap, "%d", s->v.i); break;
        default:      snprintf(out, cap, "?"); break;
    }
}

/* Format the flags byte as a short tag: "V" valid, "S" stale, "M" simulated.
 * Order is fixed so columns line up. */
static void format_flags(uint8_t flags, char *out, size_t cap) {
    if (cap < 4) { out[0] = 0; return; }
    size_t i = 0;
    out[i++] = (flags & CH_FLAG_VALID)     ? 'V' : '-';
    out[i++] = (flags & CH_FLAG_STALE)     ? 'S' : '-';
    out[i++] = (flags & CH_FLAG_SIMULATED) ? 'M' : '-';
    out[i] = 0;
}

static void print_snapshot_table(const ingest_t *ing, bool show_counts) {
    /* Header — always prints the same columns so the alignment is stable
     * regardless of flags. */
    if (show_counts) {
        printf("%-6s %-20s %-10s %-9s %-6s %-8s %s\n",
               "id", "name", "role", "flags", "n", "latest", "unit");
        printf("%-6s %-20s %-10s %-9s %-6s %-8s %s\n",
               "------", "--------------------", "---------",
               "---------", "------", "--------", "----");
    } else {
        printf("%-6s %-20s %-10s %-9s %s\n",
               "id", "name", "role", "flags", "latest");
        printf("%-6s %-20s %-10s %-9s %s\n",
               "------", "--------------------", "---------",
               "---------", "--------");
    }

    size_t seen_count = 0;
    for (size_t i = 0; i < ing->registry.count; i++) {
        const channel_desc_t *d = &ing->registry.entries[i];
        const channel_state_t *cs = ingest_state(ing, d->id);
        if (!cs) continue;

        /* Only print channels that have been seen. Unseen entries are
         * summarized below the table. */
        if (!cs->seen) continue;

        seen_count++;

        char val[32];
        format_value(&cs->latest, val, sizeof(val));

        char flags[4];
        format_flags(cs->latest.flags, flags, sizeof(flags));

        const char *role = ch_role_name(d->role);
        /* Mark synthesized entries so a typo in the feed is visible. */
        char role_tag[16];
        if (d->role == ROLE_UNKNOWN) {
            snprintf(role_tag, sizeof(role_tag), "%s(synth)", role);
            role = role_tag;
        }

        if (show_counts) {
            printf("%-6u %-20s %-10s %-9s %-6zu %-8s %s\n",
                   (unsigned)d->id,
                   d->name ? d->name : "(null)",
                   role,
                   flags,
                   ring_count(&cs->ring),
                   val,
                   d->unit ? d->unit : "");
        } else {
            printf("%-6u %-20s %-10s %-9s %s\n",
                   (unsigned)d->id,
                   d->name ? d->name : "(null)",
                   role,
                   flags,
                   val);
        }
    }

    /* Summary line: how many channels reported, how many are known but
     * silent, how many total. */
    size_t known = ing->registry.count;
    size_t silent = known - seen_count;
    printf("\n%zu channel(s) reported, %zu known but silent, "
           "%zu total in registry\n",
           seen_count, silent, known);

    if (silent) {
        printf("silent: ");
        size_t printed = 0;
        for (size_t i = 0; i < ing->registry.count; i++) {
            const channel_desc_t *d = &ing->registry.entries[i];
            const channel_state_t *cs = ingest_state(ing, d->id);
            if (cs && !cs->seen) {
                if (printed++) printf(", ");
                printf("%s(id=%u)", d->name ? d->name : "?", (unsigned)d->id);
            }
        }
        printf("\n");
    }
}

/* ---------- feed ---------- */

/* Read JSONL from `path`, feed each packet to ingest. Returns 0 on
 * success, -1 on file-open error. Malformed lines are counted and
 * reported; they do not stop the feed. */
static int run_feed(ingest_t *ing, const char *path,
                    unsigned long long *out_lines,
                    unsigned long long *out_bad) {
    FILE *f = fopen(path, "r");
    if (!f) {
        plat_log(LOG_ERROR, "cannot open %s", path);
        return -1;
    }

    char line[LINE_MAX];
    unsigned long long lines = 0, bad = 0;

    while (fgets(line, sizeof(line), f)) {
        lines++;

        size_t len = strlen(line);
        while (len && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = 0;
        }
        if (len == 0) continue;
        if (line[0] == '#') continue;

        packet_t p;
        if (packet_from_json(line, &p) != 0) {
            bad++;
            plat_log(LOG_WARN, "%s:%llu: bad packet", path, lines);
            continue;
        }
        if (ingest_feed(ing, &p) < 0) {
            bad++;
            plat_log(LOG_WARN, "%s:%llu: ingest rejected packet", path, lines);
            continue;
        }
    }
    fclose(f);

    *out_lines = lines;
    *out_bad = bad;
    return 0;
}

/* ---------- main ---------- */

int main(int argc, char **argv) {
    const char *config = "configs/channels.toml";
    const char *feed   = NULL;
    int show_counts    = 0;
    int quiet          = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if      (!strcmp(a, "--config")   && i + 1 < argc) config = argv[++i];
        else if (!strcmp(a, "--feed")     && i + 1 < argc) feed   = argv[++i];
        else if (!strcmp(a, "--channels") && i + 1 < argc) config = argv[++i];
        else if (!strcmp(a, "--counts"))   show_counts = 1;
        else if (!strcmp(a, "--quiet"))    quiet = 1;
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(argv[0]); return 0;
        }
        else {
            fprintf(stderr, "unknown argument: %s\n", a);
            usage(argv[0]);
            return 2;
        }
    }

    if (quiet) plat_log_set_level(LOG_ERROR);

    /* --config mode: no feed, just print the registry table. */
    if (!feed) {
        static registry_t reg;
        if (registry_init(&reg) != 0) return 1;
        if (registry_load_toml(&reg, config) != 0) return 1;
        print_registry_table(&reg);
        if (reg.warnings) {
            fprintf(stderr, "%zu warning(s) during load\n", reg.warnings);
        }
        return 0;
    }

    /* --feed mode: ingest the file, print the snapshot. */
    static ingest_t ing;
    if (ingest_init(&ing, config) != 0) return 1;

    unsigned long long lines = 0, bad = 0;
    if (run_feed(&ing, feed, &lines, &bad) != 0) return 1;

    print_snapshot_table(&ing, show_counts);

    fprintf(stderr,
            "\n%d line(s) read, %llu bad, %llu packet(s), %llu sample(s), "
            "%llu unknown-channel feed(s)\n",
            (int)lines, bad,
            (unsigned long long)ing.packets_seen,
            (unsigned long long)ing.samples_seen,
            (unsigned long long)ing.unknown_channels);

    return 0;
}