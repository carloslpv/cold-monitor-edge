#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "onewire.h"

/*
 * Simulated 1-Wire bus with DS18B20 sensors. It answers bit by bit the way real devices do, including the wired-AND
 * of several devices talking at once, so the ROM search and the sensor driver run against it unchanged. The host
 * tests use it, and it lets the firmware run without probes or a real GPIO, in an emulator for example.
 */

#define ONEWIRE_SIM_MAX_DEVICES 16

typedef struct onewire_sim_device onewire_sim_device_t;

struct onewire_sim_device {
    onewire_rom_t rom;
    uint8_t scratchpad[9];
    bool selected;
    int conversions;
    /** Called on each Convert T, to update the temperature in the scratchpad (optional). */
    void (*on_convert)(onewire_sim_device_t *device, void *arg);
    void *on_convert_arg;
};

typedef enum {
    SIM_IDLE,
    SIM_ROM_COMMAND,
    SIM_SEARCH,
    SIM_READ_ROM,
    SIM_MATCH_ROM,
    SIM_FUNCTION_COMMAND,
    SIM_READ_SCRATCHPAD,
    SIM_WRITE_SCRATCHPAD,
} onewire_sim_state_t;

typedef struct {
    onewire_sim_device_t devices[ONEWIRE_SIM_MAX_DEVICES];
    int count;
    bool shorted;           /* line held low: no reset succeeds */
    bool vanish_on_search;  /* every device stops answering right after a Search ROM command */

    onewire_sim_state_t state;
    uint8_t byte;
    int bits;
    int position;
    int phase;
    int bytes;
} onewire_sim_t;

void onewire_sim_init(onewire_sim_t *sim, onewire_bus_t *bus);

/** Builds a valid ROM code (CRC included) from a family code and a 48-bit serial number. */
onewire_rom_t onewire_sim_rom(uint8_t family, uint64_t serial);

/** Adds a DS18B20 holding raw (1/16 °C) at 12-bit resolution; returns it so tests can tweak its scratchpad. */
onewire_sim_device_t *onewire_sim_add_ds18b20(onewire_sim_t *sim, uint64_t serial, int16_t raw);

/** Sets the temperature a sensor reports, in 1/16 °C, and updates the CRC. */
void onewire_sim_set_raw(onewire_sim_device_t *device, int16_t raw);

/** Recomputes the scratchpad CRC after its bytes changed. */
void onewire_sim_seal(uint8_t scratchpad[9]);
