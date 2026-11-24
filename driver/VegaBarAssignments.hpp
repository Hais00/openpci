#pragma once
#include <stddef.h>
#include <stdint.h>

namespace extgpu_driver {

struct VegaBarAssignments {
    struct Range { uint32_t address, size; };
    Range ranges[4];
    static constexpr unsigned registers[4] = {0x10, 0x18, 0x24, 0x30};
    static constexpr uint32_t sizes[4] = {0x10000000, 0x200000, 0x80000, 0x20000};

    static uint32_t word(const uint8_t *p)
    {
        return p[0] | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    }

    // IOPCIConfigurator serializes native Apple Silicon cells as tag, base
    // low/high, size low/high. Only the tested board's memory BARs and ROM
    // are accepted. Include the ROM in overlap checks, never program it.
    bool parse(const uint8_t *bytes, size_t length, uint32_t bdf)
    {
        if (!bytes || length != 4 * 20 || (bdf & ~UINT32_C(0x00ffff00))) return false;
        VegaBarAssignments candidate{};
        unsigned seen = 0;
        for (unsigned row = 0; row < 4; ++row) {
            const auto *p = bytes + row * 20;
            uint32_t tag = word(p), address = word(p + 4), size = word(p + 12);
            unsigned region = 0;
            while (region < 4 && registers[region] != (tag & 255)) ++region;
            if (region == 4 || (seen & (1u << region))) return false;
            const uint32_t flags = region < 2 ? 0xc2000000u : 0x82000000u;
            if (tag != (flags | bdf | registers[region]) || word(p + 8) || word(p + 16) ||
                size != sizes[region] || !address || address % size || address > UINT32_MAX - (size - 1u))
                return false;
            for (unsigned i = 0; i < 4; ++i) {
                const auto &old = candidate.ranges[i];
                if (old.size && uint64_t(address) < uint64_t(old.address) + old.size &&
                    uint64_t(old.address) < uint64_t(address) + size)
                    return false;
            }
            seen |= 1u << region;
            candidate.ranges[region] = {address, size};
        }
        *this = candidate;
        return true;
    }
};

}
