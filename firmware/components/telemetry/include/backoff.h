#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Delay before reconnection attempt number `attempt` (0 for the first retry): exponential from `base_ms`, capped at
 * `max_ms`, with jitter so that many devices that lost the same access point do not reconnect in lockstep. `random`
 * is any 32-bit random value (esp_random() on the device, a fixed value in tests). The result lies between half and
 * all of the exponential delay.
 */
uint32_t backoff_delay_ms(uint32_t attempt, uint32_t base_ms, uint32_t max_ms, uint32_t random);

#ifdef __cplusplus
}
#endif
