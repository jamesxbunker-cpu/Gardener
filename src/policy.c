#include "gardener/policy.h"
#include <string.h>

int policy_init(policy_engine_t *pe) {
    if (!pe) return -1;
    memset(pe, 0, sizeof(*pe));
    return 0;
}