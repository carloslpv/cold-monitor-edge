#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_CONFIG_MIN_INTERVAL_S 5
#define DEVICE_CONFIG_MAX_INTERVAL_S 3600

/**
 * Remote settings the device accepts on its retained config topic, for example {"interval_s":30}.
 */
typedef struct {
    uint32_t interval_s;
} device_config_t;

/**
 * Reads the settings from a config message (not NUL terminated, straight from the MQTT event). Unknown keys are
 * ignored. Returns false and leaves `config` untouched when "interval_s" is missing, not an integer or outside
 * DEVICE_CONFIG_MIN_INTERVAL_S..DEVICE_CONFIG_MAX_INTERVAL_S, so a bad message never stops the measurements.
 */
bool device_config_parse(const char *json, size_t len, device_config_t *config);

#ifdef __cplusplus
}
#endif
