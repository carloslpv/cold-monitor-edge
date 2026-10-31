/*
 * Host tests of the 1-Wire protocol code and the DS18B20 driver, run against the simulated bus in onewire_sim, so
 * the ROM search, CRC checks and temperature decoding are checked without hardware.
 *   cmake -S firmware/components/onewire/test -B build/onewire-test && cmake --build build/onewire-test
 *   ./build/onewire-test/onewire_test
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ds18b20.h"
#include "onewire.h"
#include "onewire_sim.h"

static int failures;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            failures++;                                                                \
        }                                                                              \
    } while (0)

static onewire_sim_t sim;
static onewire_bus_t bus;

static bool contains(const onewire_rom_t *roms, int count, onewire_rom_t rom)
{
    for (int i = 0; i < count; i++) {
        if (roms[i] == rom) {
            return true;
        }
    }
    return false;
}

static void test_crc8_matches_the_example_of_maxim_application_note_27(void)
{
    // family 0x02, serial 0x000001B81C, CRC 0xA2
    const uint8_t rom[] = {0x02, 0x1C, 0xB8, 0x01, 0x00, 0x00, 0x00};
    CHECK(onewire_crc8(rom, sizeof rom) == 0xA2);
}

static void test_crc8_of_a_block_followed_by_its_crc_is_zero(void)
{
    const uint8_t rom[] = {0x02, 0x1C, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xA2};
    CHECK(onewire_crc8(rom, sizeof rom) == 0x00);
}

static void test_rom_valid_rejects_a_bad_crc_and_the_all_zero_code(void)
{
    onewire_rom_t rom = onewire_sim_rom(0x28, 0x1234);
    CHECK(onewire_rom_valid(rom));
    CHECK(!onewire_rom_valid(rom ^ ((onewire_rom_t)1 << 20)));
    CHECK(!onewire_rom_valid(0));
}

static void test_rom_hex_starts_with_the_family_code(void)
{
    char hex[17];
    onewire_rom_to_hex(0xA200000001B81C02ULL, hex);
    CHECK(strcmp("021cb801000000a2", hex) == 0);
}

static void test_search_finds_a_single_device(void)
{
    onewire_sim_device_t *device = onewire_sim_add_ds18b20(&sim, 0xAABBCCDDEEFFULL, 0);
    onewire_rom_t roms[4];

    CHECK(onewire_search_all(&bus, 0, roms, 4) == 1);
    CHECK(roms[0] == device->rom);
}

static void test_search_finds_every_device_on_a_shared_bus(void)
{
    // serials that share long prefixes force branches deep into the ROM code
    const uint64_t serials[] = {0x000000000001, 0x000000000002, 0x000000000003, 0x800000000000,
                                0x800000000001, 0x7FFFFFFFFFFF, 0x123456789ABC, 0xFFFFFFFFFFFF};
    const int count = (int)(sizeof serials / sizeof serials[0]);
    for (int i = 0; i < count; i++) {
        onewire_sim_add_ds18b20(&sim, serials[i], 0);
    }
    onewire_rom_t roms[ONEWIRE_SIM_MAX_DEVICES];

    CHECK(onewire_search_all(&bus, 0, roms, ONEWIRE_SIM_MAX_DEVICES) == count);
    for (int i = 0; i < count; i++) {
        CHECK(contains(roms, count, sim.devices[i].rom));
    }
}

static void test_search_stops_at_the_capacity_given(void)
{
    for (int i = 0; i < 5; i++) {
        onewire_sim_add_ds18b20(&sim, (uint64_t)i + 1, 0);
    }
    onewire_rom_t roms[3];

    CHECK(onewire_search_all(&bus, 0, roms, 3) == 3);
}

static void test_search_filters_by_family(void)
{
    onewire_sim_add_ds18b20(&sim, 1, 0);
    sim.devices[sim.count].rom = onewire_sim_rom(0x10, 2);  // DS18S20, another family
    sim.count++;
    onewire_sim_add_ds18b20(&sim, 3, 0);
    onewire_rom_t roms[4];

    CHECK(onewire_search_all(&bus, 0x28, roms, 4) == 2);
    CHECK(onewire_rom_family(roms[0]) == 0x28);
    CHECK(onewire_rom_family(roms[1]) == 0x28);
}

static void test_search_on_an_empty_bus_finds_nothing(void)
{
    onewire_rom_t roms[4];
    onewire_search_t search;
    onewire_search_start(&search);

    CHECK(onewire_search_next(&bus, &search) == ONEWIRE_SEARCH_NO_DEVICE);
    CHECK(onewire_search_all(&bus, 0, roms, 4) == 0);
}

static void test_search_on_a_shorted_bus_finds_nothing(void)
{
    onewire_sim_add_ds18b20(&sim, 1, 0);
    sim.shorted = true;
    onewire_rom_t roms[4];

    CHECK(onewire_search_all(&bus, 0, roms, 4) == 0);
}

static void test_search_reports_devices_that_stop_answering(void)
{
    onewire_sim_add_ds18b20(&sim, 1, 0);
    sim.vanish_on_search = true;
    onewire_rom_t roms[4];
    onewire_search_t search;
    onewire_search_start(&search);

    CHECK(onewire_search_next(&bus, &search) == ONEWIRE_SEARCH_ERROR);
    CHECK(onewire_search_all(&bus, 0, roms, 4) == -1);
}

static void test_search_can_be_restarted_after_it_is_done(void)
{
    onewire_sim_add_ds18b20(&sim, 1, 0);
    onewire_sim_add_ds18b20(&sim, 2, 0);
    onewire_search_t search;
    onewire_search_start(&search);

    CHECK(onewire_search_next(&bus, &search) == ONEWIRE_SEARCH_FOUND);
    CHECK(onewire_search_next(&bus, &search) == ONEWIRE_SEARCH_FOUND);
    CHECK(onewire_search_next(&bus, &search) == ONEWIRE_SEARCH_DONE);

    onewire_search_start(&search);
    CHECK(onewire_search_next(&bus, &search) == ONEWIRE_SEARCH_FOUND);
}

static void test_read_rom_returns_the_code_of_a_lone_device(void)
{
    onewire_sim_device_t *device = onewire_sim_add_ds18b20(&sim, 0x0102030405ULL, 0);

    CHECK(onewire_reset(&bus));
    onewire_write_byte(&bus, ONEWIRE_CMD_READ_ROM);
    uint8_t bytes[8];
    onewire_read(&bus, bytes, sizeof bytes);

    onewire_rom_t rom = 0;
    for (int i = 0; i < 8; i++) {
        rom |= (onewire_rom_t)bytes[i] << (8 * i);
    }
    CHECK(rom == device->rom);
}

static void test_select_fails_on_an_empty_bus(void)
{
    CHECK(!onewire_select(&bus, onewire_sim_rom(0x28, 1)));
    CHECK(!onewire_select_all(&bus));
}

static void scratchpad(uint8_t pad[9], uint16_t raw, uint8_t config)
{
    const uint8_t bytes[9] = {(uint8_t)raw, (uint8_t)(raw >> 8), 0x4B, 0x46, config, 0xFF, 0x0C, 0x10, 0};
    memcpy(pad, bytes, sizeof bytes);
    onewire_sim_seal(pad);
}

static int32_t decode(uint16_t raw, uint8_t config)
{
    uint8_t pad[9];
    scratchpad(pad, raw, config);
    int32_t centi = INT32_MIN;
    CHECK(ds18b20_decode(pad, &centi) == DS18B20_OK);
    return centi;
}

static void test_decode_matches_the_datasheet_table(void)
{
    // temperature/data relationship table of the datasheet, 12-bit
    CHECK(decode(0x07D0, 0x7F) == 12500); // +125
    CHECK(decode(0x0191, 0x7F) == 2506);  // +25.0625
    CHECK(decode(0x00A2, 0x7F) == 1013);  // +10.125
    CHECK(decode(0x0008, 0x7F) == 50);    // +0.5
    CHECK(decode(0x0000, 0x7F) == 0);     // 0
    CHECK(decode(0xFFF8, 0x7F) == -50);   // -0.5
    CHECK(decode(0xFF5E, 0x7F) == -1013); // -10.125
    CHECK(decode(0xFE6F, 0x7F) == -2506); // -25.0625
    CHECK(decode(0xFC90, 0x7F) == -5500); // -55
}

static void test_decode_rounds_sixteenths_to_the_nearest_hundredth(void)
{
    CHECK(decode(0x0001, 0x7F) == 6);   // 0.0625
    CHECK(decode(0x0002, 0x7F) == 13);  // 0.125
    CHECK(decode(0xFFFF, 0x7F) == -6);  // -0.0625
    CHECK(decode(0xFFFE, 0x7F) == -13); // -0.125
}

static void test_decode_ignores_the_undefined_bits_below_12_bits(void)
{
    CHECK(decode(0x0197, 0x1F) == 2500); // 9-bit: 3 undefined bits
    CHECK(decode(0x0193, 0x3F) == 2500); // 10-bit: 2
    CHECK(decode(0x0199, 0x5F) == 2550); // 11-bit: 1
}

static void test_decode_flags_the_power_on_value(void)
{
    uint8_t pad[9];
    scratchpad(pad, DS18B20_POWER_ON_RAW, 0x7F);
    int32_t centi;

    CHECK(ds18b20_decode(pad, &centi) == DS18B20_POWER_ON_VALUE);
}

static void test_decode_rejects_a_corrupted_scratchpad(void)
{
    uint8_t pad[9];
    scratchpad(pad, 0x0191, 0x7F);
    pad[0] ^= 0x04;
    int32_t centi;

    CHECK(ds18b20_decode(pad, &centi) == DS18B20_CRC_ERROR);
}

static void test_decode_rejects_a_line_held_low(void)
{
    const uint8_t zeros[9] = {0};
    int32_t centi;

    CHECK(ds18b20_decode(zeros, &centi) == DS18B20_CRC_ERROR);
}

static void test_conversion_time_halves_with_each_bit_less(void)
{
    CHECK(ds18b20_conversion_time_ms(12) == 751);
    CHECK(ds18b20_conversion_time_ms(11) == 376);
    CHECK(ds18b20_conversion_time_ms(10) == 188);
    CHECK(ds18b20_conversion_time_ms(9) == 94);
    CHECK(ds18b20_conversion_time_ms(16) == 751);
}

static void test_read_addresses_each_sensor_on_a_shared_bus(void)
{
    onewire_sim_device_t *fridge = onewire_sim_add_ds18b20(&sim, 1, 0x0050);  // 5 °C
    onewire_sim_device_t *freezer = onewire_sim_add_ds18b20(&sim, 2, -0x0190); // -25 °C
    int32_t centi;

    CHECK(ds18b20_read(&bus, fridge->rom, &centi) == DS18B20_OK);
    CHECK(centi == 500);
    CHECK(ds18b20_read(&bus, freezer->rom, &centi) == DS18B20_OK);
    CHECK(centi == -2500);
}

static void test_a_sensor_that_is_not_on_the_bus_reads_as_an_error(void)
{
    onewire_sim_add_ds18b20(&sim, 1, 0x0050);
    int32_t centi;

    // nobody answers the Match ROM, so the scratchpad reads as all ones
    CHECK(ds18b20_read(&bus, onewire_sim_rom(DS18B20_FAMILY, 99), &centi) == DS18B20_CRC_ERROR);
}

static void test_read_on_an_empty_bus_reports_not_present(void)
{
    int32_t centi;

    CHECK(ds18b20_read(&bus, onewire_sim_rom(DS18B20_FAMILY, 1), &centi) == DS18B20_NOT_PRESENT);
}

static void test_start_conversion_reaches_every_sensor(void)
{
    onewire_sim_add_ds18b20(&sim, 1, 0);
    onewire_sim_add_ds18b20(&sim, 2, 0);

    CHECK(ds18b20_start_conversion_all(&bus));
    CHECK(sim.devices[0].conversions == 1);
    CHECK(sim.devices[1].conversions == 1);
}

static void test_set_resolution_keeps_the_alarm_registers(void)
{
    onewire_sim_device_t *target = onewire_sim_add_ds18b20(&sim, 1, 0);
    onewire_sim_device_t *other = onewire_sim_add_ds18b20(&sim, 2, 0);
    target->scratchpad[2] = 0x19;
    target->scratchpad[3] = 0x02;
    onewire_sim_seal(target->scratchpad);

    CHECK(ds18b20_set_resolution(&bus, target->rom, 10) == DS18B20_OK);
    CHECK(target->scratchpad[2] == 0x19);
    CHECK(target->scratchpad[3] == 0x02);
    CHECK(target->scratchpad[4] == 0x3F);
    CHECK(other->scratchpad[4] == 0x7F);
}

int main(void)
{
    void (*const tests[])(void) = {
        test_crc8_matches_the_example_of_maxim_application_note_27,
        test_crc8_of_a_block_followed_by_its_crc_is_zero,
        test_rom_valid_rejects_a_bad_crc_and_the_all_zero_code,
        test_rom_hex_starts_with_the_family_code,
        test_search_finds_a_single_device,
        test_search_finds_every_device_on_a_shared_bus,
        test_search_stops_at_the_capacity_given,
        test_search_filters_by_family,
        test_search_on_an_empty_bus_finds_nothing,
        test_search_on_a_shorted_bus_finds_nothing,
        test_search_reports_devices_that_stop_answering,
        test_search_can_be_restarted_after_it_is_done,
        test_read_rom_returns_the_code_of_a_lone_device,
        test_select_fails_on_an_empty_bus,
        test_decode_matches_the_datasheet_table,
        test_decode_rounds_sixteenths_to_the_nearest_hundredth,
        test_decode_ignores_the_undefined_bits_below_12_bits,
        test_decode_flags_the_power_on_value,
        test_decode_rejects_a_corrupted_scratchpad,
        test_decode_rejects_a_line_held_low,
        test_conversion_time_halves_with_each_bit_less,
        test_read_addresses_each_sensor_on_a_shared_bus,
        test_a_sensor_that_is_not_on_the_bus_reads_as_an_error,
        test_read_on_an_empty_bus_reports_not_present,
        test_start_conversion_reaches_every_sensor,
        test_set_resolution_keeps_the_alarm_registers,
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        // every test starts from an empty bus
        onewire_sim_init(&sim, &bus);
        tests[i]();
    }
    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("%zu tests passed\n", sizeof tests / sizeof tests[0]);
    return 0;
}
