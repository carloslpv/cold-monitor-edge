#pragma once

#include <stddef.h>
#include <stdint.h>

#include "outbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Encodes a reading as the JSON the bridge expects, for example
 * {"device":"cm-a1b2c3","boot":3735928559,"seq":42,"probe":"28ff4a1c0b160351","t":4.25,"ts":1761868800}.
 * "ts" is left out when the clock was not set. Returns the length written, or -1 when the buffer is too small.
 */
int payload_encode_reading(char *buf, size_t size, const char *device_id, uint32_t boot_id, const reading_t *reading);

/**
 * Formats a 1-Wire ROM code as 16 lowercase hex digits. The buffer needs at least 17 bytes.
 */
void payload_format_probe(char *buf, uint64_t probe);

/**
 * Unix time of a reading. Readings taken before NTP set the clock have no timestamp; once the clock is valid they
 * are dated backwards from the current time using the uptime, so a reading that waited hours in the outbox still
 * carries the moment it was taken. Returns 0 when neither the reading nor `now` has a valid time.
 */
int64_t payload_reading_time(const reading_t *reading, int64_t now, int64_t uptime_now_s);

/**
 * Converts a temperature in degrees to hundredths of a degree, rounding half away from zero.
 */
int32_t payload_centi_celsius(float celsius);

#ifdef __cplusplus
}
#endif
