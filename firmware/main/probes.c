#include "probes.h"

#include <inttypes.h>

#include "ds18b20.h"
#include "esp_log.h"
#include "onewire_bus.h"

static const char *TAG = "probes";

static onewire_bus_handle_t bus;
static ds18b20_device_handle_t devices[PROBES_MAX];
static size_t device_count;

esp_err_t probes_init(int gpio)
{
    onewire_bus_config_t bus_config = {
        .bus_gpio_num = gpio,
        .flags = { .en_pull_up = true }, // the 4.7 kΩ external pull-up is still recommended for long cables
    };
    onewire_bus_rmt_config_t rmt_config = {
        .max_rx_bytes = 10, // 1 byte ROM command + 8 bytes ROM code + 1 byte device command
    };
    return onewire_new_bus_rmt(&bus_config, &rmt_config, &bus);
}

static void search(void)
{
    onewire_device_iter_handle_t iter = NULL;
    if (onewire_new_device_iter(bus, &iter) != ESP_OK) {
        return;
    }
    onewire_device_t device;
    while (device_count < PROBES_MAX && onewire_device_iter_get_next(iter, &device) == ESP_OK) {
        ds18b20_config_t config = { 0 };
        if (ds18b20_new_device_from_enumeration(&device, &config, &devices[device_count]) == ESP_OK) {
            ESP_LOGI(TAG, "found DS18B20 %016" PRIx64, device.address);
            device_count++;
        } else {
            ESP_LOGW(TAG, "ignoring unknown 1-Wire device %016" PRIx64, device.address);
        }
    }
    onewire_del_device_iter(iter);
}

size_t probes_read(probe_reading_t *out, size_t max)
{
    if (device_count == 0) {
        search();
        if (device_count == 0) {
            ESP_LOGE(TAG, "no DS18B20 on the bus");
            return 0;
        }
    }
    esp_err_t err = ds18b20_trigger_temperature_conversion_for_all(bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "conversion failed: %s", esp_err_to_name(err));
        return 0;
    }
    size_t n = 0;
    for (size_t i = 0; i < device_count && n < max; i++) {
        onewire_device_address_t address;
        ds18b20_get_device_address(devices[i], &address);
        float celsius;
        err = ds18b20_get_temperature(devices[i], &celsius);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "probe %016" PRIx64 " did not answer: %s", address, esp_err_to_name(err));
            continue;
        }
        out[n].address = address;
        out[n].celsius = celsius;
        n++;
    }
    return n;
}
