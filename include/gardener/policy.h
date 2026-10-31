#ifndef GARDENER_POLICY_H
#define GARDENER_POLICY_H

#include "gardener/event.h"
#include <stdint.h>
#include <stdbool.h>

/* ---------------------------------------------------------------------
 * Policy Engine.
 *
 * Consumes events from the detectors and triggers actions (notify,
 * webhook, log) based on user-defined rules.
 *
 * E0 defines the data structures and constants. The actual matching
 * logic (E1) and action execution (E2) come later.
 * ------------------------------------------------------------------- */

#define POLICY_MAX_RULES        32
#define POLICY_MAX_ACTIONS      8
#define POLICY_NAME_MAX         32

typedef enum {
    ACTION_NOTIFY  = 0,   /* print to stderr / terminal */
    ACTION_LOG     = 1,   /* append to a log file */
    ACTION_WEBHOOK = 2,   /* execute a shell command (e.g. curl) */
} policy_action_type_t;

typedef struct {
    policy_action_type_t type;
    /* Used for ACTION_LOG (path) or ACTION_WEBHOOK (url/command).
     * Empty string for ACTION_NOTIFY. */
    char target[128];
} policy_action_t;

typedef struct {
    char name[POLICY_NAME_MAX];
    
    /* Match criteria. If a field is 0 / UNKNOWN, it is treated as a
     * wildcard. */
    detector_t match_detector;
    severity_t match_severity;
    event_state_t match_state;
    
    /* Debounce: minimum seconds between triggers for this rule. */
    uint32_t cooldown_sec;
    
    /* Actions to execute when the rule matches. */
    uint8_t action_count;
    policy_action_t actions[POLICY_MAX_ACTIONS];

    /* Runtime state (not set by config). */
    uint64_t last_fired_ns;
} policy_rule_t;

typedef struct {
    uint8_t rule_count;
    policy_rule_t rules[POLICY_MAX_RULES];
} policy_engine_t;

/* ---- compile-time invariants -------------------------------------- */

/* Ensure the structs are cache-friendly and not accidentally huge. */
_Static_assert(sizeof(policy_engine_t) < 8192*8, "policy_engine_t is too large for static allocation");

/* ---- lifecycle ---------------------------------------------------- */

/* Initialize an empty policy engine. */
int policy_init(policy_engine_t *pe);

/* Load rules from a config file. (Implementation deferred to E1). */
int policy_load_toml(policy_engine_t *pe, const char *path);

/* Check if an event matches any rule, and return the rules that fired.
 * (Implementation deferred to E1). */
// int policy_evaluate(policy_engine_t *pe, const event_t *e, ...);

/* Evaluate an event against the loaded rules.
 *
 * If an event matches a rule and the rule's cooldown has expired,
 * it is considered "fired". The function updates the rule's
 * last_fired_ns to enforce the cooldown.
 *
 * Returns the number of rules that fired, and fills the `fired_rules`
 * array (up to `max_fired`) with pointers to the matched rules. The
 * caller uses these pointers to execute the actions.
 *
 * `now_ns` should be the event's timestamp (or plat_wall_ns() if you
 * want cooldowns based on real time).
 */
int policy_evaluate(policy_engine_t *pe,
                    const event_t *e,
                    uint64_t now_ns,
                    const policy_rule_t **fired_rules,
                    size_t max_fired);
#endif /* GARDENER_POLICY_H */