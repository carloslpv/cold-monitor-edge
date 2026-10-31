#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "onewire.h"

#ifdef __cplusplus
extern "C" {
#endif

/* DS18B20 digital thermometer on a 1-Wire bus (Maxim/Analog Devices datasheet 19-7487). */

#define DS18B20_FAMILY 0x28

#define DS18B20_CMD_CONVERT_T 0x44
#define DS18B20_CMD_WRITE_SCRATCHPAD 0x4E
#define DS18B20_CMD_READ_SCRATCHPAD 0xBE

#define DS18B20_SCRATCHPAD_SIZE 9

/** Raw value the sensor holds after power-up until the first conversion: exactly 85.0 °C. */
#define DS18B20_POWER_ON_RAW 0x0550

typedef enum {
    DS18B20_OK,
    DS18B20_NOT_PRESENT,    /**< No presence pulse: unplugged sensor or broken cable. */
    DS18B20_CRC_ERROR,      /**< Scratchpad failed its CRC, or read back as all zeros (line held low). */
    DS18B20_POWER_ON_VALUE, /**< 85.0 °C reset value: the sensor lost power or was read before converting. */
} ds18b20_status_t;

/** Maximum conversion time for a resolution of 9 to 12 bits (93.75 to 750 ms), rounded up. */
uint32_t ds18b20_conversion_time_ms(int resolution_bits);

/** Starts a conversion on every sensor of the bus at once (Skip ROM + Convert T). */
bool ds18b20_start_conversion_all(const onewire_bus_t *bus);

/** Sets the resolution (9 to 12 bits), keeping the alarm registers as they are. */
ds18b20_status_t ds18b20_set_resolution(const onewire_bus_t *bus, onewire_rom_t rom, int resolution_bits);

/** Reads the last conversion of one sensor, in hundredths of a degree Celsius. */
ds18b20_status_t ds18b20_read(const onewire_bus_t *bus, onewire_rom_t rom, int32_t *centi_celsius);

/** Checks a scratchpad and converts its temperature to hundredths of a degree Celsius. */
ds18b20_status_t ds18b20_decode(const uint8_t scratchpad[DS18B20_SCRATCHPAD_SIZE], int32_t *centi_celsius);

#ifdef __cplusplus
}
#endif
