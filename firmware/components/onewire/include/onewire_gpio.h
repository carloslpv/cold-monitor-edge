#pragma once

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "onewire.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 1-Wire master on a single GPIO, bit-banged with the standard-speed timings of Maxim application note 126.
 *
 * The pin is open-drain: the master only pulls the line low and lets the pull-up bring it back. The internal pull-up
 * (about 45 kOhm) is turned on, but a 4.7 kOhm resistor to 3.3 V is needed for reliable edges on a real cable.
 */
typedef struct {
    gpio_num_t pin;
    portMUX_TYPE lock;
} onewire_gpio_t;

/** Configures the pin and fills bus with operations that drive it. gpio must outlive bus. */
esp_err_t onewire_gpio_init(onewire_gpio_t *gpio, gpio_num_t pin, onewire_bus_t *bus);

#ifdef __cplusplus
}
#endif
