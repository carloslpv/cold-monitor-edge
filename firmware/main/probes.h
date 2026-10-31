#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define PROBES_MAX 8

typedef struct {
    uint64_t address;      /*!< 1-Wire ROM code */
    int32_t centi_celsius; /*!< hundredths of a degree, straight from the sensor's fixed-point value */
} probe_reading_t;

/**
 * Sets up the 1-Wire bus on the given GPIO. The probes are searched on the first probes_read().
 */
esp_err_t probes_init(int gpio);

/**
 * Starts one conversion on every probe of the bus at once, waits for it and reads them. Searches the bus again when
 * no probe is known yet or when a probe failed to answer, so a probe plugged in or swapped after boot is picked up.
 * Returns the number of readings written to `out`; a probe that fails is skipped and logged.
 */
size_t probes_read(probe_reading_t *out, size_t max);
