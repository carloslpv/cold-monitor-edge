#include "onewire_gpio.h"

#include "esp_rom_sys.h"

/*
 * Standard-speed timings in microseconds (Maxim application note 126). A slot is short and the slave samples it at a
 * fixed point, so the time-critical part of each slot runs with interrupts off on this core. The longest such window
 * is about 70 us; the Wi-Fi stack runs on the other core and is not affected.
 */
#define T_A 6   // write 1 / read: low time
#define T_B 64  // write 1: rest of the slot
#define T_C 60  // write 0: low time
#define T_D 10  // write 0: recovery
#define T_E 9   // read: wait before sampling
#define T_F 55  // read: rest of the slot
#define T_H 480 // reset: low time
#define T_I 70  // reset: wait before sampling the presence pulse
#define T_J 410 // reset: rest of the presence window

static inline void line_low(const onewire_gpio_t *gpio) { gpio_set_level(gpio->pin, 0); }

static inline void line_release(const onewire_gpio_t *gpio) { gpio_set_level(gpio->pin, 1); }

static inline bool line_read(const onewire_gpio_t *gpio) { return gpio_get_level(gpio->pin); }

static bool gpio_reset(void *ctx)
{
    onewire_gpio_t *gpio = ctx;

    // a line that stays low when released is shorted or has no pull-up; nothing can answer on it
    line_release(gpio);
    esp_rom_delay_us(5);
    if (!line_read(gpio)) {
        return false;
    }

    // the reset pulse only has a minimum length, so it does not need interrupts off
    line_low(gpio);
    esp_rom_delay_us(T_H);

    portENTER_CRITICAL(&gpio->lock);
    line_release(gpio);
    esp_rom_delay_us(T_I);
    bool presence = !line_read(gpio);
    portEXIT_CRITICAL(&gpio->lock);

    esp_rom_delay_us(T_J);
    return presence;
}

static void gpio_write_bit(void *ctx, bool bit)
{
    onewire_gpio_t *gpio = ctx;
    portENTER_CRITICAL(&gpio->lock);
    line_low(gpio);
    esp_rom_delay_us(bit ? T_A : T_C);
    line_release(gpio);
    portEXIT_CRITICAL(&gpio->lock);
    esp_rom_delay_us(bit ? T_B : T_D);
}

static bool gpio_read_bit(void *ctx)
{
    onewire_gpio_t *gpio = ctx;
    portENTER_CRITICAL(&gpio->lock);
    line_low(gpio);
    esp_rom_delay_us(T_A);
    line_release(gpio);
    esp_rom_delay_us(T_E);
    bool bit = line_read(gpio);
    portEXIT_CRITICAL(&gpio->lock);
    esp_rom_delay_us(T_F);
    return bit;
}

esp_err_t onewire_gpio_init(onewire_gpio_t *gpio, gpio_num_t pin, onewire_bus_t *bus)
{
    gpio->pin = pin;
    portMUX_INITIALIZE(&gpio->lock);

    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        return err;
    }
    line_release(gpio);

    bus->reset = gpio_reset;
    bus->write_bit = gpio_write_bit;
    bus->read_bit = gpio_read_bit;
    bus->ctx = gpio;
    return ESP_OK;
}
