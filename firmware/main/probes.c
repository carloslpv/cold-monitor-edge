#include "probes.h"

#include <inttypes.h>
#include <stdbool.h>

#include "ds18b20.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "onewire_gpio.h"
#include "sdkconfig.h"

static const char *TAG = "probes";

static onewire_gpio_t gpio_bus;
static onewire_bus_t bus;
static onewire_rom_t roms[PROBES_MAX];
static size_t probe_count;
static bool search_needed = true;

static const char *status_text(ds18b20_status_t status)
{
    switch (status) {
    case DS18B20_OK:
        return "ok";
    case DS18B20_NOT_PRESENT:
        return "no presence pulse";
    case DS18B20_CRC_ERROR:
        return "CRC error";
    case DS18B20_POWER_ON_VALUE:
        return "power-on value (85 C), the probe lost power";
    }
    return "unknown";
}

esp_err_t probes_init(int gpio)
{
    return onewire_gpio_init(&gpio_bus, (gpio_num_t)gpio, &bus);
}

static void search(void)
{
    int found = onewire_search_all(&bus, DS18B20_FAMILY, roms, PROBES_MAX);
    if (found < 0) {
        ESP_LOGW(TAG, "bus error during the search, trying again on the next reading");
        found = 0;
    }
    for (int i = 0; i < found; i++) {
        ds18b20_status_t status = ds18b20_set_resolution(&bus, roms[i], CONFIG_CM_DS18B20_RESOLUTION);
        ESP_LOGI(TAG, "found DS18B20 %016" PRIx64 " (%s)", roms[i], status_text(status));
    }
    probe_count = (size_t)found;
    search_needed = false;
}

size_t probes_read(probe_reading_t *out, size_t max)
{
    if (search_needed || probe_count == 0) {
        search();
        if (probe_count == 0) {
            ESP_LOGE(TAG, "no DS18B20 on the bus: check the wiring and the 4.7 kOhm pull-up");
            return 0;
        }
    }
    if (!ds18b20_start_conversion_all(&bus)) {
        ESP_LOGE(TAG, "no presence pulse on the bus");
        search_needed = true;
        return 0;
    }
    vTaskDelay(pdMS_TO_TICKS(ds18b20_conversion_time_ms(CONFIG_CM_DS18B20_RESOLUTION)) + 1);

    size_t n = 0;
    for (size_t i = 0; i < probe_count && n < max; i++) {
        int32_t centi;
        ds18b20_status_t status = ds18b20_read(&bus, roms[i], &centi);
        if (status != DS18B20_OK) {
            ESP_LOGW(TAG, "probe %016" PRIx64 ": %s", roms[i], status_text(status));
            // usually a probe that was unplugged or swapped: look at the bus again next time
            search_needed = true;
            continue;
        }
        out[n].address = roms[i];
        out[n].centi_celsius = centi;
        n++;
    }
    return n;
}
