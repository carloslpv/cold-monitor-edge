#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 1-Wire bus protocol: ROM commands, byte transfers, CRC-8 and the ROM search.
 *
 * Everything here is plain C on top of three bit-level operations, so the protocol logic runs and is tested on a
 * PC against a simulated bus. The ESP32 implementation of those operations is in onewire_gpio.h.
 */

#define ONEWIRE_CMD_SEARCH_ROM 0xF0
#define ONEWIRE_CMD_READ_ROM 0x33
#define ONEWIRE_CMD_MATCH_ROM 0x55
#define ONEWIRE_CMD_SKIP_ROM 0xCC

/** A 64-bit ROM code: family code in the low byte, then the 48-bit serial number, then the CRC in the high byte. */
typedef uint64_t onewire_rom_t;

typedef struct {
    /** Reset pulse; returns true when at least one device answered with a presence pulse. */
    bool (*reset)(void *ctx);
    void (*write_bit)(void *ctx, bool bit);
    bool (*read_bit)(void *ctx);
    void *ctx;
} onewire_bus_t;

static inline bool onewire_reset(const onewire_bus_t *bus) { return bus->reset(bus->ctx); }

void onewire_write_byte(const onewire_bus_t *bus, uint8_t byte);
uint8_t onewire_read_byte(const onewire_bus_t *bus);
void onewire_write(const onewire_bus_t *bus, const uint8_t *data, size_t len);
void onewire_read(const onewire_bus_t *bus, uint8_t *data, size_t len);

/** Dallas/Maxim CRC-8 (polynomial x^8 + x^5 + x^4 + 1, reflected). A block followed by its CRC yields 0. */
uint8_t onewire_crc8(const uint8_t *data, size_t len);

/** True when the ROM code's CRC byte matches its first 7 bytes and the code is not all zeros. */
bool onewire_rom_valid(onewire_rom_t rom);

uint8_t onewire_rom_family(onewire_rom_t rom);

/** Writes the ROM code as 16 lowercase hex digits, family code first (the order printed on datasheets). */
void onewire_rom_to_hex(onewire_rom_t rom, char out[17]);

/** Reset followed by Match ROM, which addresses a single device. Returns false when nothing is on the bus. */
bool onewire_select(const onewire_bus_t *bus, onewire_rom_t rom);

/** Reset followed by Skip ROM, which addresses every device at once. Returns false when nothing is on the bus. */
bool onewire_select_all(const onewire_bus_t *bus);

/** State of a ROM search (Maxim application note 187). Reuse it across calls to enumerate every device. */
typedef struct {
    onewire_rom_t rom;
    int last_discrepancy;
    bool last_device;
} onewire_search_t;

typedef enum {
    ONEWIRE_SEARCH_FOUND,     /**< search->rom holds the next device. */
    ONEWIRE_SEARCH_DONE,      /**< Every device has been returned. */
    ONEWIRE_SEARCH_NO_DEVICE, /**< No presence pulse. */
    ONEWIRE_SEARCH_ERROR,     /**< Bus noise: both a 1 and a 0 were missing, or the ROM failed its CRC. */
} onewire_search_result_t;

void onewire_search_start(onewire_search_t *search);
onewire_search_result_t onewire_search_next(const onewire_bus_t *bus, onewire_search_t *search);

/**
 * Finds up to max devices and stores their ROM codes in roms, optionally only those of one family (0 for all).
 * Returns the number found, or -1 when the search hit a bus error.
 */
int onewire_search_all(const onewire_bus_t *bus, uint8_t family, onewire_rom_t *roms, int max);

#ifdef __cplusplus
}
#endif
