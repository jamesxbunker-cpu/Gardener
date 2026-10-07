#include "gardener/log_reader.h"
#include "gardener/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

/* ---------------------------------------------------------------------
 * gardener-log — read a binary log and print its records.
 *
 *   gardener-log --file <log.bin> [options]
 *
 * Options:
 *   --from <ts_ns>    start at this timestamp (inclusive)
 *   --to <ts_ns>      stop before this timestamp (exclusive)
 *   --limit <n>       print at most n records (default: all)
 *   --jsonl           output as JSONL instead of the table
 *   --header          print the log header and registry, then exit
 *   --quiet           suppress warnings
 *   --help, -h        this message
 * ------------------------------------------------------------------- */

#define LINE_MAX 8192

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int s) { (void)s; g_stop = 1; }

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s --file <log.bin> [options]\n"
        "\n"
        "Options:\n"
        "  --from <ts_ns>   start at this timestamp (inclusive)\n"
        "  --to <ts_ns>     stop before this timestamp (exclusive)\n"
        "  --limit <n>      print at most n records\n"
        "  --jsonl          output as JSONL\n"
        "  --header         print header and registry, then exit\n"
        "  --quiet          suppress warnings\n"
        "  --help, -h       this message\n",
        prog);
}

static void print_header(const log_reader_t *r) {
    const log_header_t *h = log_reader_header(r);
    printf("file:          %s\n", log_reader_path(r));
    printf("magic:         0x%08X\n", (unsigned)h->magic);
    printf("version:       %u\n", (unsigned)h->version);
    printf("file_size:     %llu\n", (unsigned long long)h->file_size);
    printf("data_start:    %llu\n", (unsigned long long)h->data_start);
    printf("write_offset:  %llu\n", (unsigned long long)h->write_offset);
    printf("record_count:  %llu\n", (unsigned long long)h->record_count);
    printf("first_ts_ns:   %llu\n", (unsigned long long)h->first_ts_ns);
    printf("last_ts_ns:    %llu\n", (unsigned long long)h->last_ts_ns);
    printf("start_wall_ns: %llu\n", (unsigned long long)h->start_wall_ns);
    printf("last_wall_ns:  %llu\n", (unsigned long long)h->last_wall_ns);
    printf("channel_count: %u\n", (unsigned)h->channel_count);
    printf("registry_size: %u\n", (unsigned)h->registry_size);
    printf("wrap_count:    %u\n", (unsigned)h->wrap_count);
    printf("\nRegistry (%zu channel(s)):\n", log_reader_registry(r)->count);
    const registry_t *reg = log_reader_registry(r);
    for (size_t i = 0; i < reg->count; i++) {
        const channel_desc_t *c = &reg->entries[i];
        printf("  %3u  %-20s  %-8s  %s\n",
               (unsigned)c->id,
               c->name ? c->name : "?",
               ch_role_name(c->role),
               ch_type_name(c->type));
    }
}

static void print_table_line(const packet_t *p) {
    /* One line per packet: timestamp, source, sample count, and a
     * compact summary of the first few samples. */
    printf("%llu  src=%-3u  ch=%u  ",
           (unsigned long long)p->timestamp_ns,
           (unsigned)p->source_id,
           (unsigned)p->channel_count);
    int shown = 0;
    for (uint16_t i = 0; i < p->channel_count && shown < 4; i++) {
        const sample_t *s = &p->samples[i];
        printf("%sid=%u:", shown ? " " : "", (unsigned)s->channel_id);
        switch (s->type) {
            case CH_F32:  printf("%g", (double)s->v.f); break;
            case CH_I32:  printf("%d", s->v.i); break;
            case CH_U16:  printf("%u", (unsigned)s->v.u); break;
            case CH_BOOL: printf("%s", s->v.b ? "true" : "false"); break;
            case CH_ENUM: printf("%d", s->v.i); break;
            default:      printf("?"); break;
        }
        shown++;
    }
    if (p->channel_count > (uint16_t)shown) printf(" ...");
    printf("\n");
}

int main(int argc, char **argv) {
    const char *path = NULL;
    uint64_t from = 0, to = 0;
    uint64_t limit = 0;   /* 0 = no limit */
    int jsonl = 0;
    int show_header = 0;
    int quiet = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if      (!strcmp(a, "--file") && i + 1 < argc) path = argv[++i];
        else if (!strcmp(a, "--from") && i + 1 < argc) from = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(a, "--to")   && i + 1 < argc) to   = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(a, "--limit")&& i + 1 < argc) limit = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(a, "--jsonl"))   jsonl = 1;
        else if (!strcmp(a, "--header"))  show_header = 1;
        else if (!strcmp(a, "--quiet"))   quiet = 1;
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(argv[0]); return 0; }
        else { fprintf(stderr, "unknown argument: %s\n", a); usage(argv[0]); return 2; }
    }

    if (!path) { usage(argv[0]); return 2; }
    if (quiet) plat_log_set_level(LOG_ERROR);

    signal(SIGINT, on_signal);

    log_reader_t *r = log_reader_open(path);
    if (!r) return 1;

    if (show_header) {
        print_header(r);
        log_reader_close(r);
        return 0;
    }

    if (from > 0) {
        if (log_reader_seek_ns(r, from) != 0) {
            log_reader_close(r);
            return 1;
        }
    }

    uint64_t printed = 0;
    packet_t p;
    char line[LINE_MAX];

    while (!g_stop) {
        int rc = log_reader_next(r, &p);
        if (rc == 0) break;
        if (rc < 0) { log_reader_close(r); return 1; }

        if (to > 0 && p.timestamp_ns >= to) break;
        if (limit > 0 && printed >= limit) break;

        if (jsonl) {
            int n = packet_to_json(&p, line, sizeof(line));
            if (n > 0) {
                fputs(line, stdout);
                fputc('\n', stdout);
            }
        } else {
            print_table_line(&p);
        }
        printed++;
    }

    if (!jsonl) {
        fprintf(stderr, "\n%llu record(s) printed\n",
                (unsigned long long)printed);
    }

    log_reader_close(r);
    return 0;
}