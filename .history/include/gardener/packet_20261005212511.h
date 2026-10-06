#ifndef GARDENER_PACKET_H
#define GARDENER_PACKET_H

#include "gardener/channel.h"
#include <stdint.h>

/* ---------------------------------------------------------------------
 * Packet — one tick's worth of samples from one source.
 *
 * The wire format arrives in A6 (binary) and A4 (JSONL feed). For now
 * this header defines only the in-memory struct that ingest_feed()
 * consumes, so A3 can be tested with hand-built packets and no parser.
 *
 * Wire layout, once implemented:
 *   u32 magic "GRDN"
 *   u16 version
 *   u16 source_id
 *   u64 timestamp_ns
 *   u16 channel_count
 *   u16 flags
 *   [ channel_count * packed_sample (18 bytes each) ]
 *   u16 crc16
 * ------------------------------------------------------------------- */

#define PACKET_MAGIC   0x4752444Eu   /* "GRDN" */
#define PACKET_VERSION 1

/* Maximum channels in one packet. Smaller than REGISTRY_MAX_CHANNELS
 * because a single tick never carries the whole registry. Keeps packet_t
 * small enough to live on the stack on an MCU. */
#define PACKET_MAX_CH 64

/* Per-packet flags. Carried on the wire and preserved through ingest. */
#define PACKET_FLAG_SIMULATED 0x0001

typedef struct {
    uint64_t timestamp_ns;                  /* packet timestamp */
    uint16_t source_id;                     /* which system sent it */
    uint16_t flags;                         /* PACKET_FLAG_* */
    uint16_t channel_count;                 /* number of valid samples */
    uint16_t _pad;
    sample_t samples[PACKET_MAX_CH];        /* in-memory, natural-aligned */
} packet_t;

/* ---------------------------------------------------------------------
 * JSONL encode/decode.
 *
 * One packet per line. The parser is deliberately small: it handles
 * exactly the shapes the dev loop and simulated sources produce, and
 * nothing else. It is NOT a general JSON parser. When the dashboard
 * link (phase F) needs real JSON, we swap in a proper parser behind
 * these two functions and keep callers unchanged.
 *
 * Accepted shape (canonical):
 *   {"t":<uint64>,"src":<uint16>,"ch":[
 *       {"id":<uint16>,"type":"f32","role":<uint8>,"v":<number|bool>},
 *       ...
 *   ]}
 *
 * Accepted shape (shorthand, for hand-written input):
 *   {"t":<uint64>,"v":{"<id>":<number|bool>, ...}}
 *   id defaults to role=0, type inferred from value.
 *
 * Unknown keys are ignored. Missing optional fields take defaults.
 * ------------------------------------------------------------------- */

/* Encode a packet as one JSON line, no trailing newline. Returns the
 * number of bytes that would be written (may exceed cap), or -1 on
 * error. Callers should treat the return value as "needed", and re-
 * allocate if they got -1 back or if it exceeds cap. */
int packet_to_json(const packet_t *p, char *buf, size_t cap);

/* Parse one JSONL line into a packet. Trailing newline optional. Returns
 * 0 on success, -1 on parse failure. On success, out->channel_count is
 * the number of samples decoded. */
int packet_from_json(const char *line, packet_t *out);
#endif /* GARDENER_PACKET_H */