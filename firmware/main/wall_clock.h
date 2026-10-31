#pragma once

#include <stdint.h>
#include <time.h>

#define CLOCK_VALID_AFTER 1700000000 // any time before Nov 2023 means NTP has not set the clock yet

/**
 * Current Unix time in seconds, or 0 while the clock has not been set.
 */
static inline int64_t clock_now(void)
{
    time_t now = time(NULL);
    return now > CLOCK_VALID_AFTER ? (int64_t)now : 0;
}
