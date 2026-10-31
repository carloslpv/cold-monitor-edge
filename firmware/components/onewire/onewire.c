#include "onewire.h"

#define ONEWIRE_SEARCH_MAX_PASSES 256

void onewire_write_byte(const onewire_bus_t *bus, uint8_t byte)
{
    // least significant bit first
    for (int i = 0; i < 8; i++) {
        bus->write_bit(bus->ctx, (byte >> i) & 1);
    }
}

uint8_t onewire_read_byte(const onewire_bus_t *bus)
{
    uint8_t byte = 0;
    for (int i = 0; i < 8; i++) {
        if (bus->read_bit(bus->ctx)) {
            byte |= (uint8_t)(1u << i);
        }
    }
    return byte;
}

void onewire_write(const onewire_bus_t *bus, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        onewire_write_byte(bus, data[i]);
    }
}

void onewire_read(const onewire_bus_t *bus, uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        data[i] = onewire_read_byte(bus);
    }
}

uint8_t onewire_crc8(const uint8_t *data, size_t len)
{
    // bitwise on purpose: 9 bytes per reading do not justify a 256-byte table in flash
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];
        for (int bit = 0; bit < 8; bit++) {
            uint8_t mix = (crc ^ byte) & 1;
            crc >>= 1;
            if (mix) {
                crc ^= 0x8C;
            }
            byte >>= 1;
        }
    }
    return crc;
}

static void rom_to_bytes(onewire_rom_t rom, uint8_t bytes[8])
{
    for (int i = 0; i < 8; i++) {
        bytes[i] = (uint8_t)(rom >> (8 * i));
    }
}

bool onewire_rom_valid(onewire_rom_t rom)
{
    // a bus held low reads back as all zeros, which has a valid CRC of 0
    if (rom == 0) {
        return false;
    }
    uint8_t bytes[8];
    rom_to_bytes(rom, bytes);
    return onewire_crc8(bytes, 8) == 0;
}

uint8_t onewire_rom_family(onewire_rom_t rom) { return (uint8_t)rom; }

void onewire_rom_to_hex(onewire_rom_t rom, char out[17])
{
    static const char digits[] = "0123456789abcdef";
    uint8_t bytes[8];
    rom_to_bytes(rom, bytes);
    for (int i = 0; i < 8; i++) {
        out[2 * i] = digits[bytes[i] >> 4];
        out[2 * i + 1] = digits[bytes[i] & 0x0F];
    }
    out[16] = '\0';
}

bool onewire_select(const onewire_bus_t *bus, onewire_rom_t rom)
{
    if (!onewire_reset(bus)) {
        return false;
    }
    uint8_t bytes[8];
    rom_to_bytes(rom, bytes);
    onewire_write_byte(bus, ONEWIRE_CMD_MATCH_ROM);
    onewire_write(bus, bytes, sizeof bytes);
    return true;
}

bool onewire_select_all(const onewire_bus_t *bus)
{
    if (!onewire_reset(bus)) {
        return false;
    }
    onewire_write_byte(bus, ONEWIRE_CMD_SKIP_ROM);
    return true;
}

void onewire_search_start(onewire_search_t *search)
{
    search->rom = 0;
    search->last_discrepancy = 0;
    search->last_device = false;
}

/*
 * Every device sends its next ROM bit and then the complement of it; the bus is wired-AND, so the master reads:
 *   0 1  all remaining devices have a 0 here
 *   1 0  all remaining devices have a 1 here
 *   0 0  discrepancy: devices disagree, the master picks a branch
 *   1 1  nobody answered
 * The master then writes the chosen bit and the devices that do not match it go quiet until the next reset. Taking
 * the 0 branch first and remembering the deepest 0 taken (last_discrepancy) walks the whole tree, one device per
 * pass.
 */
onewire_search_result_t onewire_search_next(const onewire_bus_t *bus, onewire_search_t *search)
{
    if (search->last_device) {
        return ONEWIRE_SEARCH_DONE;
    }
    if (!onewire_reset(bus)) {
        onewire_search_start(search);
        return ONEWIRE_SEARCH_NO_DEVICE;
    }
    onewire_write_byte(bus, ONEWIRE_CMD_SEARCH_ROM);

    onewire_rom_t rom = 0;
    int last_zero = 0;
    for (int position = 1; position <= 64; position++) {
        bool id_bit = bus->read_bit(bus->ctx);
        bool complement = bus->read_bit(bus->ctx);
        bool direction;
        if (id_bit && complement) {
            onewire_search_start(search);
            return ONEWIRE_SEARCH_ERROR;
        }
        if (id_bit != complement) {
            direction = id_bit;
        } else {
            if (position < search->last_discrepancy) {
                // still on the path of the previous pass
                direction = (search->rom >> (position - 1)) & 1;
            } else {
                // at the last branch point take the 1 this time, past it take the 0 first
                direction = (position == search->last_discrepancy);
            }
            if (!direction) {
                last_zero = position;
            }
        }
        if (direction) {
            rom |= (onewire_rom_t)1 << (position - 1);
        }
        bus->write_bit(bus->ctx, direction);
    }

    if (!onewire_rom_valid(rom)) {
        onewire_search_start(search);
        return ONEWIRE_SEARCH_ERROR;
    }
    search->rom = rom;
    search->last_discrepancy = last_zero;
    search->last_device = (last_zero == 0);
    return ONEWIRE_SEARCH_FOUND;
}

int onewire_search_all(const onewire_bus_t *bus, uint8_t family, onewire_rom_t *roms, int max)
{
    onewire_search_t search;
    onewire_search_start(&search);
    int found = 0;
    // a correct search ends on its own; the pass limit only guards against a bus that keeps lying
    for (int pass = 0; found < max && pass < ONEWIRE_SEARCH_MAX_PASSES; pass++) {
        switch (onewire_search_next(bus, &search)) {
        case ONEWIRE_SEARCH_FOUND:
            if (family == 0 || onewire_rom_family(search.rom) == family) {
                roms[found++] = search.rom;
            }
            break;
        case ONEWIRE_SEARCH_DONE:
        case ONEWIRE_SEARCH_NO_DEVICE:
            return found;
        case ONEWIRE_SEARCH_ERROR:
            return -1;
        }
    }
    return found;
}
