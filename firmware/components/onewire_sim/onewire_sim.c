#include "onewire_sim.h"

#include <string.h>

#include "ds18b20.h"

static bool rom_bit(onewire_rom_t rom, int position) { return (rom >> position) & 1; }

static bool sim_reset(void *ctx)
{
    onewire_sim_t *sim = ctx;
    if (sim->shorted || sim->count == 0) {
        sim->state = SIM_IDLE;
        return false;
    }
    for (int i = 0; i < sim->count; i++) {
        sim->devices[i].selected = true;
    }
    sim->state = SIM_ROM_COMMAND;
    sim->bits = 0;
    sim->byte = 0;
    return true;
}

static void deselect_all(onewire_sim_t *sim)
{
    for (int i = 0; i < sim->count; i++) {
        sim->devices[i].selected = false;
    }
}

static void on_rom_command(onewire_sim_t *sim, uint8_t command)
{
    sim->position = 0;
    sim->phase = 0;
    switch (command) {
    case ONEWIRE_CMD_SEARCH_ROM:
        sim->state = SIM_SEARCH;
        if (sim->vanish_on_search) {
            deselect_all(sim);
        }
        break;
    case ONEWIRE_CMD_READ_ROM:
        sim->state = SIM_READ_ROM;
        break;
    case ONEWIRE_CMD_MATCH_ROM:
        sim->state = SIM_MATCH_ROM;
        break;
    case ONEWIRE_CMD_SKIP_ROM:
        sim->state = SIM_FUNCTION_COMMAND;
        break;
    default:
        sim->state = SIM_IDLE;
    }
}

static void on_function_command(onewire_sim_t *sim, uint8_t command)
{
    sim->position = 0;
    sim->bytes = 0;
    switch (command) {
    case DS18B20_CMD_CONVERT_T:
        for (int i = 0; i < sim->count; i++) {
            onewire_sim_device_t *device = &sim->devices[i];
            if (device->selected) {
                device->conversions++;
                if (device->on_convert != NULL) {
                    device->on_convert(device, device->on_convert_arg);
                }
            }
        }
        sim->state = SIM_IDLE;
        break;
    case DS18B20_CMD_READ_SCRATCHPAD:
        sim->state = SIM_READ_SCRATCHPAD;
        break;
    case DS18B20_CMD_WRITE_SCRATCHPAD:
        sim->state = SIM_WRITE_SCRATCHPAD;
        break;
    default:
        sim->state = SIM_IDLE;
    }
}

static void on_scratchpad_byte(onewire_sim_t *sim, uint8_t byte)
{
    // Write Scratchpad sets TH, TL and the configuration register, in that order
    for (int i = 0; i < sim->count; i++) {
        if (sim->devices[i].selected) {
            sim->devices[i].scratchpad[2 + sim->bytes] = byte;
            onewire_sim_seal(sim->devices[i].scratchpad);
        }
    }
    if (++sim->bytes == 3) {
        sim->state = SIM_IDLE;
    }
}

static void sim_write_bit(void *ctx, bool bit)
{
    onewire_sim_t *sim = ctx;
    switch (sim->state) {
    case SIM_SEARCH:
        if (sim->phase != 2) {
            sim->state = SIM_IDLE;  // protocol violation: the master wrote before reading both bits
            return;
        }
        for (int i = 0; i < sim->count; i++) {
            if (rom_bit(sim->devices[i].rom, sim->position) != bit) {
                sim->devices[i].selected = false;
            }
        }
        sim->phase = 0;
        if (++sim->position == 64) {
            sim->state = SIM_IDLE;
        }
        return;
    case SIM_MATCH_ROM:
        for (int i = 0; i < sim->count; i++) {
            if (rom_bit(sim->devices[i].rom, sim->position) != bit) {
                sim->devices[i].selected = false;
            }
        }
        if (++sim->position == 64) {
            sim->state = SIM_FUNCTION_COMMAND;
            sim->bits = 0;
            sim->byte = 0;
        }
        return;
    case SIM_ROM_COMMAND:
    case SIM_FUNCTION_COMMAND:
    case SIM_WRITE_SCRATCHPAD:
        sim->byte |= (uint8_t)(bit << sim->bits);
        if (++sim->bits < 8) {
            return;
        }
        uint8_t byte = sim->byte;
        sim->bits = 0;
        sim->byte = 0;
        if (sim->state == SIM_ROM_COMMAND) {
            on_rom_command(sim, byte);
        } else if (sim->state == SIM_FUNCTION_COMMAND) {
            on_function_command(sim, byte);
        } else {
            on_scratchpad_byte(sim, byte);
        }
        return;
    default:
        return;
    }
}

/* Wired-AND: the line reads 1 unless some selected device pulls it low. */
static bool sim_read_bit(void *ctx)
{
    onewire_sim_t *sim = ctx;
    bool line = true;
    switch (sim->state) {
    case SIM_SEARCH:
        if (sim->phase == 2) {
            sim->state = SIM_IDLE;
            return true;
        }
        for (int i = 0; i < sim->count; i++) {
            if (sim->devices[i].selected) {
                bool bit = rom_bit(sim->devices[i].rom, sim->position);
                line = line && (sim->phase == 0 ? bit : !bit);
            }
        }
        sim->phase++;
        return line;
    case SIM_READ_ROM:
        for (int i = 0; i < sim->count; i++) {
            if (sim->devices[i].selected) {
                line = line && rom_bit(sim->devices[i].rom, sim->position);
            }
        }
        if (++sim->position == 64) {
            sim->state = SIM_IDLE;
        }
        return line;
    case SIM_READ_SCRATCHPAD:
        for (int i = 0; i < sim->count; i++) {
            if (sim->devices[i].selected) {
                const uint8_t *pad = sim->devices[i].scratchpad;
                line = line && ((pad[sim->position / 8] >> (sim->position % 8)) & 1);
            }
        }
        // past the 9th byte a DS18B20 sends ones
        if (sim->position < 72) {
            sim->position++;
        }
        if (sim->position == 72) {
            sim->state = SIM_IDLE;
        }
        return line;
    default:
        return true;
    }
}

void onewire_sim_init(onewire_sim_t *sim, onewire_bus_t *bus)
{
    memset(sim, 0, sizeof *sim);
    bus->reset = sim_reset;
    bus->write_bit = sim_write_bit;
    bus->read_bit = sim_read_bit;
    bus->ctx = sim;
}

onewire_rom_t onewire_sim_rom(uint8_t family, uint64_t serial)
{
    uint8_t bytes[8];
    bytes[0] = family;
    for (int i = 0; i < 6; i++) {
        bytes[1 + i] = (uint8_t)(serial >> (8 * i));
    }
    bytes[7] = onewire_crc8(bytes, 7);
    onewire_rom_t rom = 0;
    for (int i = 0; i < 8; i++) {
        rom |= (onewire_rom_t)bytes[i] << (8 * i);
    }
    return rom;
}

void onewire_sim_seal(uint8_t scratchpad[9]) { scratchpad[8] = onewire_crc8(scratchpad, 8); }

void onewire_sim_set_raw(onewire_sim_device_t *device, int16_t raw)
{
    device->scratchpad[0] = (uint8_t)raw;
    device->scratchpad[1] = (uint8_t)((uint16_t)raw >> 8);
    onewire_sim_seal(device->scratchpad);
}

onewire_sim_device_t *onewire_sim_add_ds18b20(onewire_sim_t *sim, uint64_t serial, int16_t raw)
{
    onewire_sim_device_t *device = &sim->devices[sim->count++];
    memset(device, 0, sizeof *device);
    device->rom = onewire_sim_rom(DS18B20_FAMILY, serial);
    // power-on contents of the datasheet, except for the temperature
    const uint8_t pad[9] = {0, 0, 0x4B, 0x46, 0x7F, 0xFF, 0x0C, 0x10, 0};
    memcpy(device->scratchpad, pad, sizeof pad);
    onewire_sim_set_raw(device, raw);
    return device;
}
