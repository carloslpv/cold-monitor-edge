#include "payload.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

void payload_format_probe(char *buf, uint64_t probe)
{
    snprintf(buf, 17, "%016" PRIx64, probe);
}

int32_t payload_centi_celsius(float celsius)
{
    return (int32_t)lroundf(celsius * 100.0f);
}

int64_t payload_reading_time(const reading_t *reading, int64_t now, int64_t uptime_now_s)
{
    if (reading->timestamp > 0) {
        return reading->timestamp;
    }
    if (now <= 0) {
        return 0;
    }
    return now - (uptime_now_s - reading->uptime_s);
}

int payload_encode_reading(char *buf, size_t size, const char *device_id, uint32_t boot_id, const reading_t *reading)
{
    char probe[17];
    payload_format_probe(probe, reading->probe);

    // integer formatting keeps the payload exact: 4.25 is "4.25", never "4.2499999"
    int32_t centi = reading->centi_celsius;
    const char *sign = centi < 0 ? "-" : "";
    uint32_t magnitude = (uint32_t)labs((long)centi);

    int written = snprintf(buf, size,
                           "{\"device\":\"%s\",\"boot\":%" PRIu32 ",\"seq\":%" PRIu32 ",\"probe\":\"%s\","
                           "\"t\":%s%" PRIu32 ".%02" PRIu32,
                           device_id, boot_id, reading->seq, probe, sign, magnitude / 100, magnitude % 100);
    if (written < 0 || (size_t)written >= size) {
        return -1;
    }

    int tail = reading->timestamp > 0
               ? snprintf(buf + written, size - written, ",\"ts\":%" PRId64 "}", reading->timestamp)
               : snprintf(buf + written, size - written, "}");
    if (tail < 0 || (size_t)tail >= size - written) {
        return -1;
    }
    return written + tail;
}
