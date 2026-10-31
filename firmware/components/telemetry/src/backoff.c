#include "backoff.h"

uint32_t backoff_delay_ms(uint32_t attempt, uint32_t base_ms, uint32_t max_ms, uint32_t random)
{
    uint32_t delay = base_ms;
    for (uint32_t i = 0; i < attempt && delay < max_ms; i++) {
        delay = delay > max_ms / 2 ? max_ms : delay * 2;
    }
    if (delay > max_ms) {
        delay = max_ms;
    }
    uint32_t half = delay / 2;
    return half + random % (delay - half + 1);
}
