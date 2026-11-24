/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Positive BAR bounds policy shared by DriverKit and CPU-only fault tests.
 * The caller holds its provider state lock through validation AND typed I/O.
 * No descriptor, mapping, PCI register access or platform ABI belongs here.
 */
#ifndef EXTGPU_MMIO_BOUNDS_CACHE_HPP
#define EXTGPU_MMIO_BOUNDS_CACHE_HPP
#include <stdint.h>

namespace extgpu_driver {
enum class MMIOBARQuery { found, absent, error };
enum class MMIOBoundsResult {
    success, bad_argument, unavailable, stale_generation, not_found,
    unsupported, metadata_error
};

struct MMIOBARBounds {
    uint64_t size;
    uint8_t memoryIndex;
    uint8_t type;
    bool valid;
};

/* POD: the DriverKit ivars allocator zero-initializes this object. Entries are
 * indexed by PCI BAR slot for storage only; lookup ALWAYS uses memoryIndex.
 */
struct MMIOBoundsCache {
    uintptr_t provider;
    uint64_t generation;
    MMIOBARBounds bars[6];

    void clear()
    {
        provider = 0;
        generation = 0;
        for (auto &bar : bars) bar = {};
    }
};

struct MMIOBoundsContext {
    uintptr_t provider;
    uint64_t generation;
    uint64_t expectedGeneration;
    bool stopping;
    bool owned;
};

// A positive memory-enable check is reusable briefly by the same owner.
// The caller must invalidate it before configuration or register operations
// that can change device state, and still validate ownership and bounds.
struct MMIOMemoryEnableCheck {
    static constexpr uint64_t maxAgeNs = 150000;
    uint64_t checkedAtNs;

    bool valid_at(uint64_t now) const
    {
        return checkedAtNs && now >= checkedAtNs && now - checkedAtNs < maxAgeNs;
    }

    bool record(uint64_t started, uint16_t command)
    {
        const bool enabled = command != UINT16_MAX && (command & 2);
        checkedAtNs = enabled ? started : 0;
        return enabled;
    }
};

inline bool valid_mmio_request(uint64_t index, uint64_t offset, uint64_t width)
{
    return index <= UINT8_MAX && (width == 1 || width == 2 || width == 4 || width == 8) &&
        !(offset & (width - 1)) && offset <= UINT64_MAX - width;
}

inline bool valid_bar_metadata(const MMIOBARBounds &bar, unsigned slot)
{
    if (bar.size == 0 || (bar.size & (bar.size - 1))) return false;
    switch (bar.type) {
    case 0x01: // PCI I/O BAR, never an MMIO entry.
        return bar.size >= 4 && bar.size <= (UINT64_C(1) << 32);
    case 0x00: // 32-bit memory, with/without prefetchability.
    case 0x08:
        return bar.size >= 16 && bar.size <= (UINT64_C(1) << 32);
    case 0x04: // A 64-bit BAR consumes the next slot.
    case 0x0c:
        return bar.size >= 16 && slot < 5;
    default:
        return false;
    }
}

inline MMIOBoundsResult mmio_bounds_result(const MMIOBARBounds &bar,
                                           uint64_t offset, uint64_t width)
{
    if (bar.type & 1) return MMIOBoundsResult::unsupported;
    return bar.size >= width && offset <= bar.size - width
        ? MMIOBoundsResult::success : MMIOBoundsResult::bad_argument;
}

inline const MMIOBARBounds *find_mmio_bar(const MMIOBoundsCache &cache, uint64_t index)
{
    for (const auto &bar : cache.bars) {
        if (bar.valid && bar.memoryIndex == index) return &bar;
    }
    return nullptr;
}

inline bool prefetchable_mmio_bar(const MMIOBARBounds *bar)
{
    return bar && (bar->type == 0x08 || bar->type == 0x0c);
}

/* Check the complete span after the normal locked bounds lookup has
 * established a cache for this provider and ownership generation. */
inline bool prefetchable_bar_contains_units(const MMIOBoundsCache &cache, uint64_t index,
                                             uint64_t offset, uint64_t width, uint64_t count)
{
    if (!count || !valid_mmio_request(index, offset, width)) return false;
    const auto *bar = find_mmio_bar(cache, index);
    return prefetchable_mmio_bar(bar) && offset <= bar->size && count <= (bar->size - offset) / width;
}

inline bool prefetchable_bar_contains_words(const MMIOBoundsCache &cache, uint64_t index,
                                             uint64_t offset, uint64_t count)
{
    return prefetchable_bar_contains_units(cache, index, offset, 8, count);
}

/* Operations::query_bar returns Found only for acknowledged metadata, Absent
 * only for an explicit missing BAR, and Error for transport/other failures.
 * A failed/ambiguous scan publishes nothing, including earlier positive rows.
 * No absence or error is cached. A miss must retry a fresh complete scan.
 */
template <typename Operations>
MMIOBoundsResult validate_mmio_bounds(MMIOBoundsCache &cache,
                                      const MMIOBoundsContext &context,
                                      uint64_t index, uint64_t offset, uint64_t width,
                                      Operations &operations)
{
    if (!valid_mmio_request(index, offset, width)) return MMIOBoundsResult::bad_argument;
    if (!context.provider || !context.generation || context.stopping || !context.owned)
        return MMIOBoundsResult::unavailable;
    if (context.expectedGeneration != context.generation)
        return MMIOBoundsResult::stale_generation;

    if (cache.provider != context.provider || cache.generation != context.generation)
        cache.clear();
    if (const auto *bar = find_mmio_bar(cache, index)) return mmio_bounds_result(*bar, offset, width);

    // A new scan supersedes the previous snapshot. Never retain old positives
    // after observing contradictory or uncertain metadata on a cache miss.
    cache.clear();
    MMIOBARBounds scanned[6] = {};
    bool upper = false;
    const MMIOBARBounds *matched = nullptr;
    for (unsigned slot = 0; slot < 6; ++slot) {
        MMIOBARBounds &bar = scanned[slot];
        const MMIOBARQuery result = operations.query_bar(static_cast<uint8_t>(slot), bar);
        bar.valid = false; // Output from a failed query is never evidence.
        if (result == MMIOBARQuery::error) return MMIOBoundsResult::metadata_error;
        if (result == MMIOBARQuery::absent) {
            bar = {};
            upper = false;
            continue;
        }
        if (result != MMIOBARQuery::found || upper || !valid_bar_metadata(bar, slot))
            return MMIOBoundsResult::metadata_error;
        for (unsigned earlier = 0; earlier < slot; ++earlier) {
            if (scanned[earlier].valid && scanned[earlier].memoryIndex == bar.memoryIndex)
                return MMIOBoundsResult::metadata_error;
        }
        bar.valid = true;
        upper = (bar.type & 0x06) == 0x04;
        if (bar.memoryIndex == index) matched = &bar;
    }
    if (!matched) return MMIOBoundsResult::not_found;
    const MMIOBoundsResult result = mmio_bounds_result(*matched, offset, width);
    if (result != MMIOBoundsResult::success) return result;

    cache.provider = context.provider;
    cache.generation = context.generation;
    for (unsigned slot = 0; slot < 6; ++slot) {
        // An I/O BAR can participate in ambiguity checks but cannot be cached
        // as a valid MemoryReadXX/MemoryWriteXX target.
        if (scanned[slot].valid && !(scanned[slot].type & 1)) cache.bars[slot] = scanned[slot];
    }
    return MMIOBoundsResult::success;
}
}
#endif
