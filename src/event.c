#include "gardener/event.h"

#include <stdio.h>
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

int event_to_json(const event_t *e, const char *msg,
                  char *buf, size_t cap) {
    if (!e || !buf || cap == 0) return -1;

    /* Truncate msg to EVENT_MSG_MAX - 1 chars if longer. */
    char msg_buf[EVENT_MSG_MAX];
    if (msg) {
        strncpy(msg_buf, msg, sizeof(msg_buf) - 1);
        msg_buf[sizeof(msg_buf) - 1] = 0;
    } else {
        msg_buf[0] = 0;
    }

    /* Escape quotes and backslashes in msg for JSON. Cheap manual
     * escape: doubled. Full escape (control chars, unicode) is
     * deferred until we have a real need. */
    char escaped[EVENT_MSG_MAX * 2];
    size_t ep = 0;
    for (size_t i = 0; msg_buf[i] && ep < sizeof(escaped) - 2; i++) {
        char c = msg_buf[i];
        if (c == '"' || c == '\\') {
            escaped[ep++] = '\\';
        }
        escaped[ep++] = c;
    }
    escaped[ep] = 0;

    uint64_t ts = event_get_timestamp(e);
    uint64_t src_ts = event_get_source_ts(e);

    /* Build the JSON line. All numeric fields printed without quotes;
     * strings quoted; channel/regime printed as numbers, with
     * 0xFFFF (EVENT_CHANNEL_NONE / EVENT_REGIME_NONE) rendered as
     * null. */
    char ch_buf[16], ch2_buf[16], reg_buf[16];
    if (e->channel_id == EVENT_CHANNEL_NONE)
        snprintf(ch_buf, sizeof(ch_buf), "null");
    else
        snprintf(ch_buf, sizeof(ch_buf), "%u", (unsigned)e->channel_id);
    if (e->channel_id2 == EVENT_CHANNEL_NONE)
        snprintf(ch2_buf, sizeof(ch2_buf), "null");
    else
        snprintf(ch2_buf, sizeof(ch2_buf), "%u", (unsigned)e->channel_id2);
    if (e->regime == EVENT_REGIME_NONE)
        snprintf(reg_buf, sizeof(reg_buf), "null");
    else
        snprintf(reg_buf, sizeof(reg_buf), "%u", (unsigned)e->regime);

    int n = snprintf(buf, cap,
        "{\"t\":%llu,\"src_t\":%llu,"
        "\"ch\":%s,\"ch2\":%s,\"regime\":%s,"
        "\"detector\":\"%s\",\"severity\":\"%s\",\"state\":\"%s\","
        "\"observed\":%g,\"mean\":%g,\"stddev\":%g,\"threshold\":%g,"
        "\"msg\":\"%s\"}",
        (unsigned long long)ts,
        (unsigned long long)src_ts,
        ch_buf, ch2_buf, reg_buf,
        detector_name((detector_t)e->detector),
        severity_name((severity_t)e->severity),
        event_state_name((event_state_t)e->state),
        (double)e->observed,
        (double)e->baseline_mean,
        (double)e->baseline_stddev,
        (double)e->threshold,
        escaped);
    return n;
}