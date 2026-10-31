#include "gardener/policy.h"
#include "gardener/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---------------------------------------------------------------------
 * Policy engine: config loading and rule matching.
 * ------------------------------------------------------------------- */

int policy_init(policy_engine_t *pe) {
    if (!pe) return -1;
    memset(pe, 0, sizeof(*pe));
    return 0;
}

/* Helper: parse "10s" or "5000ms" into seconds. */
static uint32_t parse_cooldown(const char *s) {
    if (!s) return 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s) return 0;
    if (*end == 's') return (uint32_t)v;
    if (!strncmp(end, "ms", 2)) return (uint32_t)(v / 1000);
    return (uint32_t)v; // default to seconds
}

/* Helper: parse enum strings. */
static detector_t parse_detector(const char *s) {
    if (!strcmp(s, "range"))       return DETECTOR_RANGE;
    if (!strcmp(s, "zscore"))      return DETECTOR_ZSCORE;
    if (!strcmp(s, "flatline"))    return DETECTOR_FLATLINE;
    if (!strcmp(s, "roc"))         return DETECTOR_ROC;
    if (!strcmp(s, "correlation")) return DETECTOR_CORRELATION;
    if (!strcmp(s, "mismatch"))    return DETECTOR_MISMATCH;
    return DETECTOR_UNKNOWN;
}

static severity_t parse_severity(const char *s) {
    if (!strcmp(s, "info"))     return SEVERITY_INFO;
    if (!strcmp(s, "warn"))     return SEVERITY_WARN;
    if (!strcmp(s, "critical")) return SEVERITY_CRITICAL;
    return (severity_t)255; // wildcard
}

static event_state_t parse_state(const char *s) {
    if (!strcmp(s, "started")) return EVENT_STATE_STARTED;
    if (!strcmp(s, "cleared")) return EVENT_STATE_CLEARED;
    return (event_state_t)255; // wildcard
}

static policy_action_type_t parse_action_type(const char *s) {
    if (!strcmp(s, "notify"))  return ACTION_NOTIFY;
    if (!strcmp(s, "log"))     return ACTION_LOG;
    if (!strcmp(s, "webhook")) return ACTION_WEBHOOK;
    return (policy_action_type_t)255;
}

/* Very small TOML subset parser for [[rule]] and [[rule.action]] blocks. */
/* Very small TOML subset parser for [[rule]] and [[rule.action]] blocks. */
int policy_load_toml(policy_engine_t *pe, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        plat_log(LOG_ERROR, "policy: cannot open %s", path);
        return -1;
    }

    policy_init(pe);
    char line[512];
    policy_rule_t *cur_rule = NULL;
    policy_action_t *cur_action = NULL;
    int in_action = 0;

    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == '\n' || *p == 0) continue;

        if (!strncmp(p, "[[rule.action]]", 15)) {
            if (!cur_rule || cur_rule->action_count >= POLICY_MAX_ACTIONS) continue;
            cur_action = &cur_rule->actions[cur_rule->action_count++];
            memset(cur_action, 0, sizeof(*cur_action));
            in_action = 1;
            continue;
        }

        if (!strncmp(p, "[[rule]]", 8)) {
            if (pe->rule_count >= POLICY_MAX_RULES) {
                plat_log(LOG_WARN, "policy: too many rules (max %d)", POLICY_MAX_RULES);
                break;
            }
            cur_rule = &pe->rules[pe->rule_count++];
            memset(cur_rule, 0, sizeof(*cur_rule));
            cur_rule->match_detector = (detector_t)255;
            cur_rule->match_severity = (severity_t)255;
            cur_rule->match_state = (event_state_t)255;
            cur_action = NULL;
            in_action = 0;
            continue;
        }

        /* key = value */
        char key[64], val[128];
        if (sscanf(p, "%63[^= \t] = \"%127[^\"]\"", key, val) != 2 &&
            sscanf(p, "%63[^= \t] = %127[^\n#]", key, val) != 2) {
            continue;
        }

        // Trim trailing whitespace from val
        size_t vl = strlen(val);
        while (vl > 0 && isspace((unsigned char)val[vl-1])) val[--vl] = 0;

        if (in_action && cur_action) {
            if (!strcmp(key, "type"))   cur_action->type = parse_action_type(val);
            if (!strcmp(key, "target")) {
                /* Explicitly cap the copy to prevent GCC truncation warnings */
                snprintf(cur_action->target, sizeof(cur_action->target), "%.*s",
                         (int)sizeof(cur_action->target) - 1, val);
            }
        } else if (cur_rule) {
            if (!strcmp(key, "name")) {
                snprintf(cur_rule->name, sizeof(cur_rule->name), "%.*s",
                         (int)sizeof(cur_rule->name) - 1, val);
            }
            if (!strcmp(key, "detector")) cur_rule->match_detector = parse_detector(val);
            if (!strcmp(key, "severity")) cur_rule->match_severity = parse_severity(val);
            if (!strcmp(key, "state"))    cur_rule->match_state = parse_state(val);
            if (!strcmp(key, "cooldown")) cur_rule->cooldown_sec = parse_cooldown(val);
        }
    }

    fclose(f);
    plat_log(LOG_INFO, "policy: loaded %d rules from %s", pe->rule_count, path);
    return 0;
}

/* Returns true if the rule matches the event. */
static bool rule_matches(const policy_rule_t *rule, const event_t *e) {
    if (rule->match_detector != (detector_t)255 && rule->match_detector != e->detector) return false;
    if (rule->match_severity != (severity_t)255 && rule->match_severity != e->severity) return false;
    if (rule->match_state != (event_state_t)255 && rule->match_state != e->state) return false;
    return true;
}

int policy_evaluate(policy_engine_t *pe,
                    const event_t *e,
                    uint64_t now_ns,
                    const policy_rule_t **fired_rules,
                    size_t max_fired) {
    if (!pe || !e) return -1;

    int fired = 0;

    for (uint8_t i = 0; i < pe->rule_count; i++) {
        policy_rule_t *rule = &pe->rules[i];

        if (!rule_matches(rule, e)) continue;

        /* Enforce cooldown. */
        if (rule->last_fired_ns > 0) {
            uint64_t cooldown_ns = (uint64_t)rule->cooldown_sec * 1000000000ull;
            if (now_ns < rule->last_fired_ns + cooldown_ns) {
                continue; // Still in cooldown
            }
        }

        /* Fire the rule. */
        rule->last_fired_ns = now_ns;
        
        if ((size_t)fired < max_fired) {
            fired_rules[fired] = rule;
        }
        fired++;
    }

    return fired;
}