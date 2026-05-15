#ifndef SNES_CPU_WRAM_SHADOW_HPP
#define SNES_CPU_WRAM_SHADOW_HPP

#include <stdint.h>

static const uint32_t WRAM_SIZE       = 128 * 1024;
static const uint32_t SHADOW_SENTINEL = 0xFFFFFFFF;

// Convert any 24-bit SNES address to a canonical WRAM byte offset (0..131071).
// Returns -1 if the address is not in WRAM.
inline int32_t wramOffset(uint32_t addr24) {
    uint8_t  bank   = (addr24 >> 16) & 0xFF;
    uint16_t offset = addr24 & 0xFFFF;

    if (bank == 0x7E || bank == 0x7F)
        return (bank - 0x7E) * 0x10000 + offset;

    // Low WRAM mirror: $00-$3F and $80-$BF, addresses $0000-$1FFF
    if (offset < 0x2000 && (bank < 0x40 || (bank >= 0x80 && bank < 0xC0)))
        return offset;

    return -1;
}

#endif
