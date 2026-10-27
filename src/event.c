#include "gardener/event.h"

#include <string.h>

/* ---------------------------------------------------------------------
 * Event helpers, D0: naming and timestamp accessors.
 *
 * event_to_json is declared in the header but not implemented here.
 * It lands in D1 with the first detector that needs to write events.
 * ------------------------------------------------------------------- */

const char *detector_name(detector_t d) {
    switch (d) {
        case DETECTOR_UNKNOWN:     return "unknown";
        case DETECTOR_RANGE:       return "range";
        case DETECTOR_ZSCORE:      return "zscore";
        case DETECTOR_FLATLINE:    return "flatline";
        case DETECTOR_ROC:         return "roc";
        case DETECTOR_CORRELATION: return "correlation";
        case DETECTOR_MISMATCH:    return "mismatch";
        default:                   return "?";
    }
}

const char *severity_name(severity_t s) {
    switch (s) {
        case SEVERITY_INFO:     return "info";
        case SEVERITY_WARN:     return "warn";
        case SEVERITY_CRITICAL: return "critical";
        default:                return "?";
    }
}

const char *event_state_name(event_state_t st) {
    switch (st) {
        case EVENT_STATE_STARTED: return "started";
        case EVENT_STATE_CLEARED: return "cleared";
        default:                  return "?";
    }
}

void event_set_timestamp(event_t *e, uint64_t ts_ns) {
    if (!e) return;
    e->timestamp_ns_lo = (uint32_t)(ts_ns & 0xFFFFFFFFu);
    e->timestamp_ns_hi = (uint32_t)(ts_ns >> 32);
}

void event_set_source_ts(event_t *e, uint64_t ts_ns) {
    if (!e) return;
    e->source_ts_ns_lo = (uint32_t)(ts_ns & 0xFFFFFFFFu);
    e->source_ts_ns_hi = (uint32_t)(ts_ns >> 32);
}

uint64_t event_get_timestamp(const event_t *e) {
    if (!e) return 0;
    return ((uint64_t)e->timestamp_ns_hi << 32) | (uint64_t)e->timestamp_ns_lo;
}

uint64_t event_get_source_ts(const event_t *e) {
    if (!e) return 0;
    return ((uint64_t)e->source_ts_ns_hi << 32) | (uint64_t)e->source_ts_ns_lo;
}