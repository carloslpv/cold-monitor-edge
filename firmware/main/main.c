#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_link.h"
#include "nvs_flash.h"
#include "outbox.h"
#include "payload.h"
#include "probes.h"
#include "wall_clock.h"
#include "wifi.h"

static const char *TAG = "main";

static outbox_entry_t outbox_storage[CONFIG_CM_OUTBOX_CAPACITY];
static outbox_t outbox;
static SemaphoreHandle_t outbox_lock;
static volatile uint32_t interval_s = CONFIG_CM_READ_INTERVAL_S;
static char device_id[32];

static void set_interval(uint32_t seconds)
{
    interval_s = seconds;
}

static void init_device_id(void)
{
    if (strlen(CONFIG_CM_DEVICE_ID) > 0) {
        strlcpy(device_id, CONFIG_CM_DEVICE_ID, sizeof(device_id));
        return;
    }
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    snprintf(device_id, sizeof(device_id), "cm-%02x%02x%02x", mac[3], mac[4], mac[5]);
}

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}


/**
 * Reads every probe on a fixed period and stores the readings in the outbox. It never touches the network, so a
 * dead access point or broker does not delay the measurements.
 */
static void measure_task(void *arg)
{
    uint32_t seq = 0;
    probe_reading_t probes[PROBES_MAX];
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        size_t n = probes_read(probes, PROBES_MAX);
        int64_t timestamp = clock_now();
        int64_t uptime_s = esp_timer_get_time() / 1000000;

        xSemaphoreTake(outbox_lock, portMAX_DELAY);
        for (size_t i = 0; i < n; i++) {
            reading_t reading = {
                .seq = ++seq,
                .timestamp = timestamp,
                .uptime_s = uptime_s,
                .probe = probes[i].address,
                .centi_celsius = payload_centi_celsius(probes[i].celsius),
            };
            if (!reading_outbox_push(&outbox, &reading)) {
                ESP_LOGW(TAG, "outbox full, oldest reading dropped (%" PRIu32 " so far)", outbox.dropped);
            }
        }
        size_t pending = reading_outbox_pending(&outbox);
        xSemaphoreGive(outbox_lock);

        ESP_LOGI(TAG, "%u reading(s) taken, %u waiting for delivery", (unsigned)n, (unsigned)pending);
        mqtt_link_notify();
        xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(interval_s * 1000));
    }
}

void app_main(void)
{
    init_nvs();
    init_device_id();
    // a new random id per boot lets the bridge tell "seq 1 again after a reboot" from a duplicate of seq 1
    uint32_t boot_id = esp_random();
    ESP_LOGI(TAG, "device %s, boot %" PRIu32, device_id, boot_id);

    outbox_lock = xSemaphoreCreateMutex();
    reading_outbox_init(&outbox, outbox_storage, CONFIG_CM_OUTBOX_CAPACITY);
    ESP_ERROR_CHECK(probes_init(CONFIG_CM_ONEWIRE_GPIO));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_start(CONFIG_CM_WIFI_SSID, CONFIG_CM_WIFI_PASSWORD);

    // measure from the start, even before Wi-Fi: those readings wait in the outbox, and the ones taken before NTP
    // sets the clock are dated from the uptime when they are sent
    xTaskCreate(measure_task, "measure", 4096, NULL, 6, NULL);

    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_CM_NTP_SERVER);
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp_config));

    const mqtt_link_config_t link = {
        .broker_uri = CONFIG_CM_MQTT_BROKER_URI,
        // the broker ACL lets each user write only under coldmonitor/<user name>/, so the device id is the default
        .username = strlen(CONFIG_CM_MQTT_USERNAME) > 0 ? CONFIG_CM_MQTT_USERNAME : device_id,
        .password = CONFIG_CM_MQTT_PASSWORD,
        .device_id = device_id,
        .boot_id = boot_id,
        .outbox = &outbox,
        .lock = outbox_lock,
        .max_inflight = CONFIG_CM_MAX_INFLIGHT,
        .on_interval = set_interval,
    };
    mqtt_link_start(&link);
}
