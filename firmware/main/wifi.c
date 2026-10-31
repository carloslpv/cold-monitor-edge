#include "wifi.h"

#include <inttypes.h>
#include <string.h>

#include "backoff.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define CONNECTED_BIT BIT0
#define BACKOFF_BASE_MS 1000
#define BACKOFF_MAX_MS 60000

static const char *TAG = "wifi";

static EventGroupHandle_t events;
static esp_timer_handle_t reconnect_timer;
static uint32_t attempt;

static void reconnect(void *arg)
{
    esp_wifi_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(events, CONNECTED_BIT);
        const wifi_event_sta_disconnected_t *event = data;
        uint32_t delay = backoff_delay_ms(attempt++, BACKOFF_BASE_MS, BACKOFF_MAX_MS, esp_random());
        ESP_LOGW(TAG, "disconnected (reason %d), retrying in %" PRIu32 " ms", event->reason, delay);
        esp_timer_stop(reconnect_timer); // no-op when it is not running
        esp_timer_start_once(reconnect_timer, (uint64_t)delay * 1000);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "connected, ip " IPSTR, IP2STR(&event->ip_info.ip));
        attempt = 0;
        xEventGroupSetBits(events, CONNECTED_BIT);
    }
}

void wifi_start(const char *ssid, const char *password)
{
    events = xEventGroupCreate();
    const esp_timer_create_args_t timer_args = { .callback = reconnect, .name = "wifi_reconnect" };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &reconnect_timer));

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    wifi_config_t config = { 0 };
    strlcpy((char *)config.sta.ssid, ssid, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, password, sizeof(config.sta.password));
    config.sta.threshold.authmode = password[0] != '\0' ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

bool wifi_wait_connected(uint32_t timeout_ms)
{
    EventBits_t bits = xEventGroupWaitBits(events, CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return (bits & CONNECTED_BIT) != 0;
}
