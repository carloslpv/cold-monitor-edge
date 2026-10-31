#include "outbox.h"

#include <string.h>

static outbox_entry_t *at(outbox_t *outbox, size_t offset)
{
    return &outbox->entries[(outbox->head + offset) % outbox->capacity];
}

static void drop_acked_head(outbox_t *outbox)
{
    while (outbox->count > 0 && outbox->entries[outbox->head].msg_id == OUTBOX_ACKED) {
        outbox->head = (outbox->head + 1) % outbox->capacity;
        outbox->count--;
    }
}

void outbox_init(outbox_t *outbox, outbox_entry_t *storage, size_t capacity)
{
    memset(storage, 0, capacity * sizeof(*storage));
    outbox->entries = storage;
    outbox->capacity = capacity;
    outbox->head = 0;
    outbox->count = 0;
    outbox->dropped = 0;
}

bool outbox_push(outbox_t *outbox, const reading_t *reading)
{
    bool kept_all = true;
    if (outbox->count == outbox->capacity) {
        // full: the oldest entry gives its slot to the new reading
        if (outbox->entries[outbox->head].msg_id != OUTBOX_ACKED) {
            outbox->dropped++;
            kept_all = false;
        }
        outbox->head = (outbox->head + 1) % outbox->capacity;
        outbox->count--;
    }
    outbox_entry_t *slot = at(outbox, outbox->count);
    slot->reading = *reading;
    slot->msg_id = OUTBOX_UNSENT;
    outbox->count++;
    return kept_all;
}

outbox_entry_t *outbox_next_unsent(outbox_t *outbox)
{
    for (size_t i = 0; i < outbox->count; i++) {
        outbox_entry_t *entry = at(outbox, i);
        if (entry->msg_id == OUTBOX_UNSENT) {
            return entry;
        }
    }
    return NULL;
}

void outbox_mark_sent(outbox_entry_t *entry, int msg_id)
{
    entry->msg_id = msg_id;
}

bool outbox_ack(outbox_t *outbox, int msg_id)
{
    if (msg_id <= 0) {
        return false;
    }
    for (size_t i = 0; i < outbox->count; i++) {
        outbox_entry_t *entry = at(outbox, i);
        if (entry->msg_id == msg_id) {
            entry->msg_id = OUTBOX_ACKED;
            drop_acked_head(outbox);
            return true;
        }
    }
    return false;
}

void outbox_requeue_inflight(outbox_t *outbox)
{
    for (size_t i = 0; i < outbox->count; i++) {
        outbox_entry_t *entry = at(outbox, i);
        if (entry->msg_id > 0) {
            entry->msg_id = OUTBOX_UNSENT;
        }
    }
}

size_t outbox_inflight(const outbox_t *outbox)
{
    size_t inflight = 0;
    for (size_t i = 0; i < outbox->count; i++) {
        if (outbox->entries[(outbox->head + i) % outbox->capacity].msg_id > 0) {
            inflight++;
        }
    }
    return inflight;
}

size_t outbox_pending(const outbox_t *outbox)
{
    size_t pending = 0;
    for (size_t i = 0; i < outbox->count; i++) {
        if (outbox->entries[(outbox->head + i) % outbox->capacity].msg_id != OUTBOX_ACKED) {
            pending++;
        }
    }
    return pending;
}
