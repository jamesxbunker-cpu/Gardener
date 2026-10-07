/* tools/make_sample_log.c
 *
 * Throwaway utility: write a small sample log so gardener-log has
 * something to read. Not part of the build; compile manually.
 */
#include "gardener/log_writer.h"
#include "gardener/packet.h"
#include "gardener/platform.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <output.bin> [n_packets]\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];
    int n = (argc >= 3) ? atoi(argv[2]) : 100;

    /* Register three channels so the header snapshot has content. */
    registry_t reg;
    registry_init(&reg);

    reg.entries[0].id = 1;
    reg.entries[0].name = "coolant_temp";
    reg.entries[0].unit = "C";
    reg.entries[0].type = CH_F32;
    reg.entries[0].role = ROLE_SENSOR;
    reg.entries[0].min_valid = 0.0f;
    reg.entries[0].max_valid = 150.0f;
    reg.entries[0].has_range = true;

    reg.entries[1].id = 2;
    reg.entries[1].name = "coolant_pump";
    reg.entries[1].unit = "%";
    reg.entries[1].type = CH_F32;
    reg.entries[1].role = ROLE_ACTUATOR;
    reg.entries[1].controllable = true;
    reg.entries[1].min_valid = 0.0f;
    reg.entries[1].max_valid = 100.0f;
    reg.entries[1].has_range = true;

    reg.entries[2].id = 30;
    reg.entries[2].name = "running";
    reg.entries[2].type = CH_BOOL;
    reg.entries[2].role = ROLE_STATUS;
    reg.entries[2].is_regime = true;

    reg.count = 3;

    log_writer_t *w = log_writer_open(path, 1024 * 1024, &reg, true);
    if (!w) {
        fprintf(stderr, "log_writer_open failed\n");
        return 1;
    }

    /* Timestamps 1 s apart, values oscillating. */
    for (int i = 0; i < n; i++) {
        uint64_t ts = 1000000000ull + (uint64_t)i * 1000000000ull;

        sample_t s[3];
        memset(s, 0, sizeof(s));

        s[0].channel_id = 1;
        s[0].type = CH_F32;
        s[0].role = ROLE_SENSOR;
        s[0].flags = CH_FLAG_VALID;
        s[0].v.f = 60.0f + 10.0f * (float)((i % 10) - 5) / 5.0f;
        s[0].timestamp_ns = ts;

        s[1].channel_id = 2;
        s[1].type = CH_F32;
        s[1].role = ROLE_ACTUATOR;
        s[1].flags = CH_FLAG_VALID;
        s[1].v.f = 50.0f + (float)(i % 20);
        s[1].timestamp_ns = ts;

        s[2].channel_id = 30;
        s[2].type = CH_BOOL;
        s[2].role = ROLE_STATUS;
        s[2].flags = CH_FLAG_VALID;
        s[2].v.b = (i > 5) ? 1 : 0;
        s[2].timestamp_ns = ts;

        packet_t p;
        memset(&p, 0, sizeof(p));
        p.timestamp_ns = ts;
        p.source_id = 1;
        p.channel_count = 3;
        for (int k = 0; k < 3; k++) p.samples[k] = s[k];

        if (log_writer_append(w, &p) < 0) {
            fprintf(stderr, "append failed at %d\n", i);
            log_writer_close(w);
            return 1;
        }
    }

    log_writer_close(w);
    fprintf(stderr, "wrote %d packets to %s\n", n, path);
    return 0;
}