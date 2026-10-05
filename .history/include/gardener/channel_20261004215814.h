#ifndef GARDENER_CHANNEL_H
#define GARDENER_CHANNEL_H

#include <stdint.h>
#include <stdbool.h>

/* Wire + storage type of a channel's value. Kept small so sample_t packs
 * into 24 bytes in memory and 18 bytes on the wire (see packet.h). */
typedef enum {
    CH_F32  = 0,
    CH_I32  = 1,
    CH_U16  = 2,
    CH_BOOL = 3,
    CH_ENUM = 4,
} ch_type_t;

/* Role of a channel within the system. Determines which detectors apply
 * and what a value means. A PID loop produces SETPOINT, CTRL_OUT, SENSOR,
 * and ACTUATOR channels; a supervisor treats them differently. */
typedef enum {
    ROLE_UNKNOWN  = 0,
    ROLE_SENSOR   = 1,   /* physical measurement */
    ROLE_SETPOINT = 2,   /* desired value for a loop */
    ROLE_CTRL_OUT = 3,   /* controller output before the actuator */
    ROLE_ACTUATOR = 4,   /* actual commanded actuator value */
    ROLE_STATUS   = 5,   /* bits, modes, fault codes */
    ROLE_COUNTER  = 6,   /* totals, runtime hours */
} ch_role_t;

/* Per-sample flags. Carried on the wire and preserved through ingest. */
#define CH_FLAG_VALID     0x01
#define CH_FLAG_STALE     0x02
#define CH_FLAG_SIMULATED 0x04

/* Static description of a channel, owned by the registry. Strings are
 * borrowed from the registry's arena and live as long as the registry. */
typedef struct {
    uint16_t    id;
    const char *name;
    const char *unit;
    ch_type_t   type;
    ch_role_t   role;
    bool        controllable;   /* may gardener write to this channel? */
    bool        is_regime;      /* regime selector for baseline segmentation */
    bool        has_range;
    float       min_valid;
    float       max_valid;
} channel_desc_t;

/* A single sample. Value is a union so all types fit in 4 bytes; detectors
 * read via sample_as_f32() regardless of the stored type.
 *
 * Layout: 2+1+1+1+1 (header) + 4 (value) + 8 (timestamp) = 18 bytes of
 * payload. In memory the struct is padded to 24 for natural alignment.
 * On the wire we pack to 18 (see packet.h). */
typedef struct {
    uint16_t channel_id;
    uint8_t  type;         /* ch_type_t */
    uint8_t  role;         /* ch_role_t */
    uint8_t  flags;        /* CH_FLAG_* */
    uint8_t  _pad;         /* keep next field aligned */
    union {
        float    f;
        int32_t  i;
        uint16_t u;
        uint8_t  b;
    } v;
    uint64_t timestamp_ns;
} sample_t;

/* Read any sample as a float. Bool yields 0.0 or 1.0, enum is treated as
 * i32. Detectors only ever see floats. */
static inline float sample_as_f32(const sample_t *s) {
    switch (s->type) {
        case CH_F32:  return s->v.f;
        case CH_I32:  return (float)s->v.i;
        case CH_U16:  return (float)s->v.u;
        case CH_BOOL: return s->v.b ? 1.0f : 0.0f;
        case CH_ENUM: return (float)s->v.i;
        default:      return 0.0f;
    }
}

/* Human-readable names for logs and the dump tool. */
static inline const char *ch_role_name(ch_role_t r) {
    switch (r) {
        case ROLE_SENSOR:   return "sensor";
        case ROLE_SETPOINT: return "setpoint";
        case ROLE_CTRL_OUT: return "ctrl_out";
        case ROLE_ACTUATOR: return "actuator";
        case ROLE_STATUS:   return "status";
        case ROLE_COUNTER:  return "counter";
        default:            return "unknown";
    }
}

static inline const char *ch_type_name(ch_type_t t) {
    switch (t) {
        case CH_F32:  return "f32";
        case CH_I32:  return "i32";
        case CH_U16:  return "u16";
        case CH_BOOL: return "bool";
        case CH_ENUM: return "enum";
        default:      return "?";
    }
}

#endif