#pragma once

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "outbox.h"

typedef struct {
    const char *broker_uri;
    const char *username;
    const char *password;
    const char *device_id;
    uint32_t boot_id;
    outbox_t *outbox;          /*!< Shared with the measuring task */
    SemaphoreHandle_t lock;    /*!< Guards `outbox` */
    uint32_t max_inflight;
    void (*on_interval)(uint32_t interval_s); /*!< Called when a valid config message arrives */
} mqtt_link_config_t;

/**
 * Connects to the broker and starts the publishing task, which drains the outbox whenever it is connected.
 *
 * Topics, with <id> the device id:
 *   coldmonitor/<id>/readings  one JSON reading per message, QoS 1
 *   coldmonitor/<id>/status    "online" or "offline", retained; "offline" is the last will, so the broker
 *                              publishes it by itself when the device vanishes without saying goodbye
 *   coldmonitor/<id>/config    retained settings for the device, for example {"interval_s":30}
 */
void mqtt_link_start(const mqtt_link_config_t *config);

/**
 * Wakes the publishing task after new readings were pushed to the outbox.
 */
void mqtt_link_notify(void);
