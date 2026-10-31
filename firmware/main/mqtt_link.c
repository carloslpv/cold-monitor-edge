#include "mqtt_link.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "device_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "payload.h"
#include "wall_clock.h"

#define TOPIC_SIZE 96
#define PAYLOAD_SIZE 160
#define IDLE_WAKEUP_MS 5000

static const char *TAG = "mqtt";

static mqtt_link_config_t cfg;
static esp_mqtt_client_handle_t client;
static TaskHandle_t publisher;
static volatile bool connected;

static char readings_topic[TOPIC_SIZE];
static char status_topic[TOPIC_SIZE];
static char config_topic[TOPIC_SIZE];

void mqtt_link_notify(void)
{
    if (publisher != NULL) {
        xTaskNotifyGive(publisher);
    }
}

static void on_config(const char *data, int len)
{
    device_config_t config;
    if (!device_config_parse(data, (size_t)len, &config)) {
        ESP_LOGW(TAG, "ignoring invalid config: %.*s", len, data);
        return;
    }
    ESP_LOGI(TAG, "interval set to %" PRIu32 " s", config.interval_s);
    cfg.on_interval(config.interval_s);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t event = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected to the broker");
        connected = true;
        esp_mqtt_client_publish(client, status_topic, "online", 0, 1, 1);
        esp_mqtt_client_subscribe(client, config_topic, 1);
        mqtt_link_notify();
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected from the broker");
        connected = false;
        // readings published without a PUBACK are sent again after reconnecting
        xSemaphoreTake(cfg.lock, portMAX_DELAY);
        reading_outbox_requeue_inflight(cfg.outbox);
        xSemaphoreGive(cfg.lock);
        break;
    case MQTT_EVENT_PUBLISHED:
        xSemaphoreTake(cfg.lock, portMAX_DELAY);
        reading_outbox_ack(cfg.outbox, event->msg_id);
        xSemaphoreGive(cfg.lock);
        mqtt_link_notify();
        break;
    case MQTT_EVENT_DATA:
        if (event->topic_len == (int)strlen(config_topic)
            && memcmp(event->topic, config_topic, event->topic_len) == 0) {
            on_config(event->data, event->data_len);
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "mqtt error, type %d", event->error_handle->error_type);
        break;
    default:
        break;
    }
}

/**
 * Publishes readings from the outbox, at most `max_inflight` at a time, while connected. The PUBACK handler above
 * removes them; this task only ever sends.
 */
static void publish_task(void *arg)
{
    char payload[PAYLOAD_SIZE];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(IDLE_WAKEUP_MS));
        while (connected) {
            xSemaphoreTake(cfg.lock, portMAX_DELAY);
            outbox_entry_t *entry = reading_outbox_inflight(cfg.outbox) < cfg.max_inflight
                                    ? reading_outbox_next_unsent(cfg.outbox) : NULL;
            int len = -1;
            if (entry != NULL) {
                reading_t reading = entry->reading;
                reading.timestamp = payload_reading_time(&reading, clock_now(), esp_timer_get_time() / 1000000);
                len = payload_encode_reading(payload, sizeof(payload), cfg.device_id, cfg.boot_id, &reading);
            }
            uint32_t seq = entry != NULL ? entry->reading.seq : 0;
            xSemaphoreGive(cfg.lock);
            if (entry == NULL || len < 0) {
                break;
            }

            // the lock is not held here: the mqtt task takes it in the PUBACK handler, and holding both locks at
            // once is how deadlocks start. Enqueue only stores the message; the mqtt task sends it.
            int msg_id = esp_mqtt_client_enqueue(client, readings_topic, payload, len, 1, 0, true);
            if (msg_id <= 0) {
                break;
            }
            xSemaphoreTake(cfg.lock, portMAX_DELAY);
            // a new reading may have overwritten the slot meanwhile (outbox full); mark only the one just sent
            if (entry->msg_id == OUTBOX_UNSENT && entry->reading.seq == seq) {
                reading_outbox_mark_sent(entry, msg_id);
            }
            xSemaphoreGive(cfg.lock);
        }
    }
}

void mqtt_link_start(const mqtt_link_config_t *config)
{
    cfg = *config;
    snprintf(readings_topic, sizeof(readings_topic), "coldmonitor/%s/readings", cfg.device_id);
    snprintf(status_topic, sizeof(status_topic), "coldmonitor/%s/status", cfg.device_id);
    snprintf(config_topic, sizeof(config_topic), "coldmonitor/%s/config", cfg.device_id);

    const esp_mqtt_client_config_t mqtt_config = {
        .broker.address.uri = cfg.broker_uri,
        .credentials = {
            .username = cfg.username,
            .client_id = cfg.device_id,
            .authentication.password = cfg.password,
        },
        .session = {
            .keepalive = 30,
            .last_will = {
                .topic = status_topic,
                .msg = "offline",
                .qos = 1,
                .retain = 1,
            },
        },
        .network.reconnect_timeout_ms = 5000,
    };
    client = esp_mqtt_client_init(&mqtt_config);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(client));
    xTaskCreate(publish_task, "publish", 4096, NULL, 5, &publisher);
}
