#include "ds18b20.h"

#define CONFIG_RESOLUTION_SHIFT 5

static int clamp_resolution(int bits)
{
    if (bits < 9) {
        return 9;
    }
    if (bits > 12) {
        return 12;
    }
    return bits;
}

uint32_t ds18b20_conversion_time_ms(int resolution_bits)
{
    // 750 ms at 12 bits, halved for each bit less
    return (750u >> (12 - clamp_resolution(resolution_bits))) + 1;
}

bool ds18b20_start_conversion_all(const onewire_bus_t *bus)
{
    if (!onewire_select_all(bus)) {
        return false;
    }
    onewire_write_byte(bus, DS18B20_CMD_CONVERT_T);
    return true;
}

static ds18b20_status_t read_scratchpad(const onewire_bus_t *bus, onewire_rom_t rom,
                                        uint8_t scratchpad[DS18B20_SCRATCHPAD_SIZE])
{
    if (!onewire_select(bus, rom)) {
        return DS18B20_NOT_PRESENT;
    }
    onewire_write_byte(bus, DS18B20_CMD_READ_SCRATCHPAD);
    onewire_read(bus, scratchpad, DS18B20_SCRATCHPAD_SIZE);
    return DS18B20_OK;
}

static bool scratchpad_valid(const uint8_t scratchpad[DS18B20_SCRATCHPAD_SIZE])
{
    // a line held low reads as nine zeros, whose CRC is also zero
    bool all_zero = true;
    for (int i = 0; i < DS18B20_SCRATCHPAD_SIZE; i++) {
        all_zero = all_zero && scratchpad[i] == 0;
    }
    return !all_zero && onewire_crc8(scratchpad, DS18B20_SCRATCHPAD_SIZE) == 0;
}

ds18b20_status_t ds18b20_set_resolution(const onewire_bus_t *bus, onewire_rom_t rom, int resolution_bits)
{
    uint8_t scratchpad[DS18B20_SCRATCHPAD_SIZE];
    ds18b20_status_t status = read_scratchpad(bus, rom, scratchpad);
    if (status != DS18B20_OK) {
        return status;
    }
    if (!scratchpad_valid(scratchpad)) {
        return DS18B20_CRC_ERROR;
    }
    uint8_t config = (uint8_t)(((clamp_resolution(resolution_bits) - 9) << CONFIG_RESOLUTION_SHIFT) | 0x1F);
    if (!onewire_select(bus, rom)) {
        return DS18B20_NOT_PRESENT;
    }
    const uint8_t data[] = {DS18B20_CMD_WRITE_SCRATCHPAD, scratchpad[2], scratchpad[3], config};
    onewire_write(bus, data, sizeof data);
    return DS18B20_OK;
}

ds18b20_status_t ds18b20_read(const onewire_bus_t *bus, onewire_rom_t rom, int32_t *centi_celsius)
{
    uint8_t scratchpad[DS18B20_SCRATCHPAD_SIZE];
    ds18b20_status_t status = read_scratchpad(bus, rom, scratchpad);
    if (status != DS18B20_OK) {
        return status;
    }
    return ds18b20_decode(scratchpad, centi_celsius);
}

ds18b20_status_t ds18b20_decode(const uint8_t scratchpad[DS18B20_SCRATCHPAD_SIZE], int32_t *centi_celsius)
{
    if (!scratchpad_valid(scratchpad)) {
        return DS18B20_CRC_ERROR;
    }
    int16_t raw = (int16_t)(scratchpad[0] | (scratchpad[1] << 8));
    if (raw == DS18B20_POWER_ON_RAW) {
        return DS18B20_POWER_ON_VALUE;
    }

    // below 12 bits the lowest bits are undefined: 1 at 11 bits, 2 at 10, 3 at 9
    int resolution = 9 + ((scratchpad[4] >> CONFIG_RESOLUTION_SHIFT) & 0x03);
    int undefined_bits = 12 - resolution;
    raw = (int16_t)(raw & ~((1 << undefined_bits) - 1));

    // raw is in 1/16 °C: hundredths = raw * 100 / 16 = raw * 25 / 4, rounded half away from zero
    int32_t scaled = (int32_t)raw * 25;
    *centi_celsius = (scaled + (scaled >= 0 ? 2 : -2)) / 4;
    return DS18B20_OK;
}
