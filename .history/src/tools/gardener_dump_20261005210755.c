#include "gardener/registry.h"
#include "gardener/platform.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s --config <channels.toml>\n"
        "\n"
        "Load a channel registry and print its contents.\n",
        prog);
}

/* Column widths chosen so the sample config fits without truncation. */
static void print_table(const registry_t *r) {
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

int main(int argc, char **argv) {
    const char *config = NULL;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--config") && i + 1 < argc) config = argv[++i];
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]); return 0;
        }
        else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    if (!config) { usage(argv[0]); return 2; }

    static registry_t reg;
    if (registry_init(&reg) != 0) {
        plat_log(LOG_ERROR, "registry_init failed");
        return 1;
    }
    if (registry_load_toml(&reg, config) != 0) {
        return 1;
    }

    print_table(&reg);

    if (reg.warnings) {
        fprintf(stderr, "%zu warning(s) during load\n", reg.warnings);
    }
    return 0;
}