#include "gardener/platform.h"

#include <stdio.h>
#include <string.h>
#include <signal.h>

/* ---------------------------------------------------------------------
 * Gardener entry point. Phase A only knows how to print its version and
 * report platform time; the ingest loop arrives in A4.
 * ------------------------------------------------------------------- */

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
        "usage: %s [options]\n"
        "\n"
        "  --version        print version and exit\n"
        "  --verbose        log at DEBUG level\n"
        "  --quiet          log at WARN level and above\n"
        "  --help           print this message\n",
        prog);
}

int main(int argc, char **argv) {
    int verbose = 0;
    int quiet   = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if      (!strcmp(a, "--version")) { puts("gardener 0.1.0 (phase A0)"); return 0; }
        else if (!strcmp(a, "--verbose")) { verbose = 1; }
        else if (!strcmp(a, "--quiet"))   { quiet   = 1; }
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { print_usage(argv[0]); return 0; }
        else {
            fprintf(stderr, "unknown argument: %s\n", a);
            print_usage(argv[0]);
            return 2;
        }
    }

    if (verbose) plat_log_set_level(LOG_DEBUG);
    if (quiet)   plat_log_set_level(LOG_WARN);

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    plat_log(LOG_INFO, "gardener 0.1.0 starting (phase A0)");
    plat_log(LOG_DEBUG, "monotonic now = %llu ns",
             (unsigned long long)plat_now_ns());
    plat_log(LOG_DEBUG, "wall clock now = %llu ns",
             (unsigned long long)plat_wall_ns());

    /* Phase A0 has nothing to do yet. Loop briefly so Ctrl-C exercises
     * the signal handler and the shutdown path; remove this in A4 when
     * the real stdin loop arrives. */
    int ticks = 0;
    while (!g_stop && ticks < 10) {
        plat_sleep_ms(100);
        ticks++;
        plat_log(LOG_DEBUG, "tick %d", ticks);
    }

    plat_log(LOG_INFO, "gardener shutting down after %d ticks", ticks);
    return 0;
}