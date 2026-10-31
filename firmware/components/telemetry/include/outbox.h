#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * One temperature reading taken by a probe.
 */
typedef struct {
    uint32_t seq;           /*!< Sequence number, unique per boot */
    int64_t timestamp;      /*!< Unix time in seconds, 0 when the clock was not set yet */
    int64_t uptime_s;       /*!< Seconds since boot, to date the reading later if the clock was not set yet */
    uint64_t probe;         /*!< 1-Wire ROM code of the probe */
    int32_t centi_celsius;  /*!< Temperature in hundredths of a degree, so no float reaches the payload */
} reading_t;

#define OUTBOX_UNSENT 0
#define OUTBOX_ACKED (-1)

typedef struct {
    reading_t reading;
    int msg_id; /*!< OUTBOX_UNSENT, OUTBOX_ACKED or the MQTT message id while waiting for the PUBACK */
} outbox_entry_t;

/**
 * Ring buffer of readings waiting to be delivered.
 *
 * A reading leaves the outbox only when the broker acknowledges it (PUBACK), so readings taken while Wi-Fi or the
 * broker is down are kept and sent later. When the buffer is full the oldest reading is overwritten: losing the
 * oldest data is better than stopping to measure. The outbox is not thread safe; the caller holds a lock.
 */
typedef struct {
    outbox_entry_t *entries;
    size_t capacity;
    size_t head;      /*!< Index of the oldest entry */
    size_t count;     /*!< Entries in use, acknowledged ones included until they reach the head */
    uint32_t dropped; /*!< Readings overwritten because the buffer was full */
} outbox_t;

void reading_outbox_init(outbox_t *outbox, outbox_entry_t *storage, size_t capacity);

/**
 * Adds a reading. Returns false when the buffer was full and the oldest reading was overwritten.
 */
bool reading_outbox_push(outbox_t *outbox, const reading_t *reading);

/**
 * Oldest reading not published yet, or NULL when every reading is in flight or acknowledged.
 */
outbox_entry_t *reading_outbox_next_unsent(outbox_t *outbox);

/**
 * Marks an entry returned by reading_outbox_next_unsent() as published with the given MQTT message id.
 */
void reading_outbox_mark_sent(outbox_entry_t *entry, int msg_id);

/**
 * Handles a PUBACK. Acknowledgements may arrive out of order. Returns false for an unknown message id, for example
 * one whose reading was overwritten in the meantime.
 */
bool reading_outbox_ack(outbox_t *outbox, int msg_id);

/**
 * Puts every in-flight reading back in the queue, used when the connection drops before the PUBACK arrives. The
 * readings are sent again, so the receiver must tolerate duplicates (at-least-once delivery).
 */
void reading_outbox_requeue_inflight(outbox_t *outbox);

size_t reading_outbox_inflight(const outbox_t *outbox);

/**
 * Readings still waiting for delivery (unsent and in flight).
 */
size_t reading_outbox_pending(const outbox_t *outbox);

#ifdef __cplusplus
}
#endif
