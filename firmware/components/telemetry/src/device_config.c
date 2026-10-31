#include "device_config.h"

#include <ctype.h>
#include <string.h>

#define INTERVAL_KEY "\"interval_s\""

static const char *find(const char *haystack, size_t len, const char *needle)
{
    size_t needle_len = strlen(needle);
    for (size_t i = 0; i + needle_len <= len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            return haystack + i;
        }
    }
    return NULL;
}

bool device_config_parse(const char *json, size_t len, device_config_t *config)
{
    const char *end = json + len;
    const char *p = find(json, len, INTERVAL_KEY);
    if (p == NULL) {
        return false;
    }
    p += strlen(INTERVAL_KEY);
    while (p < end && isspace((unsigned char)*p)) {
        p++;
    }
    if (p == end || *p != ':') {
        return false;
    }
    p++;
    while (p < end && isspace((unsigned char)*p)) {
        p++;
    }

    uint32_t value = 0;
    size_t digits = 0;
    while (p < end && isdigit((unsigned char)*p)) {
        if (value > DEVICE_CONFIG_MAX_INTERVAL_S) {
            return false; // stop before the number can overflow
        }
        value = value * 10 + (uint32_t)(*p - '0');
        digits++;
        p++;
    }
    // reject "30.5", "30s" and the like: after the digits only a separator may follow
    if (digits == 0 || (p < end && *p != ',' && *p != '}' && !isspace((unsigned char)*p))) {
        return false;
    }
    if (value < DEVICE_CONFIG_MIN_INTERVAL_S || value > DEVICE_CONFIG_MAX_INTERVAL_S) {
        return false;
    }
    config->interval_s = value;
    return true;
}
