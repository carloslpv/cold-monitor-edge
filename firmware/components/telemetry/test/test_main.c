#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backoff.h"
#include "device_config.h"
#include "outbox.h"
#include "payload.h"

static int failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            failures++;                                                   \
        }                                                                 \
    } while (0)

static reading_t reading(uint32_t seq)
{
    reading_t r = { .seq = seq, .timestamp = 1761868800, .probe = 0x28ff4a1c0b160351ULL, .centi_celsius = 425 };
    return r;
}

static void test_outbox_keeps_readings_until_acked(void)
{
    outbox_entry_t storage[4];
    outbox_t outbox;
    reading_outbox_init(&outbox, storage, 4);

    reading_t r1 = reading(1), r2 = reading(2);
    CHECK(reading_outbox_push(&outbox, &r1));
    CHECK(reading_outbox_push(&outbox, &r2));

    outbox_entry_t *first = reading_outbox_next_unsent(&outbox);
    CHECK(first != NULL && first->reading.seq == 1);
    reading_outbox_mark_sent(first, 10);
    outbox_entry_t *second = reading_outbox_next_unsent(&outbox);
    CHECK(second != NULL && second->reading.seq == 2);
    reading_outbox_mark_sent(second, 11);
    CHECK(reading_outbox_next_unsent(&outbox) == NULL);
    CHECK(reading_outbox_inflight(&outbox) == 2);

    // PUBACKs out of order: the second one first
    CHECK(reading_outbox_ack(&outbox, 11));
    CHECK(reading_outbox_pending(&outbox) == 1);
    CHECK(reading_outbox_ack(&outbox, 10));
    CHECK(reading_outbox_pending(&outbox) == 0);
    CHECK(outbox.count == 0);
    CHECK(!reading_outbox_ack(&outbox, 10));
}

static void test_outbox_requeues_inflight_on_disconnect(void)
{
    outbox_entry_t storage[4];
    outbox_t outbox;
    reading_outbox_init(&outbox, storage, 4);
    reading_t r1 = reading(1);
    reading_outbox_push(&outbox, &r1);
    reading_outbox_mark_sent(reading_outbox_next_unsent(&outbox), 7);

    reading_outbox_requeue_inflight(&outbox);

    CHECK(reading_outbox_inflight(&outbox) == 0);
    outbox_entry_t *again = reading_outbox_next_unsent(&outbox);
    CHECK(again != NULL && again->reading.seq == 1);
    CHECK(!reading_outbox_ack(&outbox, 7)); // a late PUBACK of the old attempt no longer matches
}

static void test_outbox_overwrites_oldest_when_full(void)
{
    outbox_entry_t storage[3];
    outbox_t outbox;
    reading_outbox_init(&outbox, storage, 3);
    for (uint32_t seq = 1; seq <= 3; seq++) {
        reading_t r = reading(seq);
        CHECK(reading_outbox_push(&outbox, &r));
    }
    reading_t r4 = reading(4);
    CHECK(!reading_outbox_push(&outbox, &r4));
    CHECK(outbox.dropped == 1);
    CHECK(reading_outbox_pending(&outbox) == 3);
    CHECK(reading_outbox_next_unsent(&outbox)->reading.seq == 2);
}

static void test_outbox_overwriting_inflight_reading_counts_as_dropped(void)
{
    outbox_entry_t storage[2];
    outbox_t outbox;
    reading_outbox_init(&outbox, storage, 2);
    reading_t r1 = reading(1), r2 = reading(2), r3 = reading(3);
    reading_outbox_push(&outbox, &r1);
    reading_outbox_push(&outbox, &r2);
    reading_outbox_mark_sent(reading_outbox_next_unsent(&outbox), 1);
    reading_outbox_mark_sent(reading_outbox_next_unsent(&outbox), 2);
    CHECK(reading_outbox_ack(&outbox, 2)); // acked but still behind the head

    CHECK(!reading_outbox_push(&outbox, &r3)); // the oldest (seq 1) is still in flight, so it is lost
    CHECK(outbox.dropped == 1);
    CHECK(reading_outbox_pending(&outbox) == 1);
    CHECK(!reading_outbox_ack(&outbox, 1)); // its late PUBACK finds nothing
}

static void test_payload(void)
{
    char buf[160];
    reading_t r = reading(42);
    int len = payload_encode_reading(buf, sizeof(buf), "cm-a1b2c3", 3735928559u, &r);
    const char *expected = "{\"device\":\"cm-a1b2c3\",\"boot\":3735928559,\"seq\":42,"
                           "\"probe\":\"28ff4a1c0b160351\",\"t\":4.25,\"ts\":1761868800}";
    CHECK(len == (int)strlen(expected));
    CHECK(strcmp(buf, expected) == 0);

    r.centi_celsius = -1805;
    r.timestamp = 0;
    payload_encode_reading(buf, sizeof(buf), "cm-a1b2c3", 1, &r);
    CHECK(strstr(buf, "\"t\":-18.05}") != NULL);
    CHECK(strstr(buf, "\"ts\"") == NULL);

    r.centi_celsius = -5;
    payload_encode_reading(buf, sizeof(buf), "cm-a1b2c3", 1, &r);
    CHECK(strstr(buf, "\"t\":-0.05}") != NULL);

    CHECK(payload_encode_reading(buf, 20, "cm-a1b2c3", 1, &r) == -1);
}

static void test_reading_time(void)
{
    reading_t r = reading(1);
    CHECK(payload_reading_time(&r, 1761900000, 500) == 1761868800); // already dated

    r.timestamp = 0;
    r.uptime_s = 100;
    CHECK(payload_reading_time(&r, 0, 400) == 0);                   // clock still unknown
    CHECK(payload_reading_time(&r, 1761900000, 400) == 1761899700); // taken 300 s ago
}

static void test_centi_celsius_rounding(void)
{
    CHECK(payload_centi_celsius(4.25f) == 425);
    CHECK(payload_centi_celsius(-18.0625f) == -1806);
    CHECK(payload_centi_celsius(0.004f) == 0);
}

static void test_backoff(void)
{
    CHECK(backoff_delay_ms(0, 1000, 60000, 0) == 500);
    CHECK(backoff_delay_ms(0, 1000, 60000, 500) == 1000);
    CHECK(backoff_delay_ms(3, 1000, 60000, 0) == 4000);
    CHECK(backoff_delay_ms(10, 1000, 60000, 0) == 30000);
    CHECK(backoff_delay_ms(1000, 1000, 60000, UINT32_MAX) <= 60000);
}

static void test_device_config(void)
{
    device_config_t config = { .interval_s = 60 };
    const char *ok = "{\"interval_s\": 30, \"other\": true}";
    CHECK(device_config_parse(ok, strlen(ok), &config));
    CHECK(config.interval_s == 30);

    const char *bad[] = { "{}", "{\"interval_s\":2}", "{\"interval_s\":99999999999}", "{\"interval_s\":\"30\"}",
                          "{\"interval_s\":30.5}", "{\"interval_s\":-30}", "{\"interval_s\":" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(!device_config_parse(bad[i], strlen(bad[i]), &config));
    }
    CHECK(config.interval_s == 30);

    // MQTT payloads are not NUL terminated: only the first `len` bytes count
    const char raw[] = "{\"interval_s\":45}garbage";
    CHECK(device_config_parse(raw, 17, &config));
    CHECK(config.interval_s == 45);
}

int main(void)
{
    test_outbox_keeps_readings_until_acked();
    test_outbox_requeues_inflight_on_disconnect();
    test_outbox_overwrites_oldest_when_full();
    test_outbox_overwriting_inflight_reading_counts_as_dropped();
    test_payload();
    test_reading_time();
    test_centi_celsius_rounding();
    test_backoff();
    test_device_config();
    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("all telemetry tests passed\n");
    return EXIT_SUCCESS;
}
