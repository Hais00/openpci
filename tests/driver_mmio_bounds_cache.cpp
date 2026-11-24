#include "MMIOBoundsCache.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); std::exit(1); } } while (0)

using namespace extgpu_driver;

struct Metadata {
    MMIOBARQuery replies[6] = {MMIOBARQuery::found, MMIOBARQuery::absent,
        MMIOBARQuery::found, MMIOBARQuery::absent, MMIOBARQuery::found, MMIOBARQuery::found};
    MMIOBARBounds bars[6] = {
        {UINT64_C(1) << 20, 7, 0x0c, false}, {},
        {UINT64_C(1) << 16, 2, 0x04, false}, {},
        {256, 3, 0x01, false}, {4096, 1, 0x00, false}};
    unsigned queries = 0;
    unsigned typedReads = 0;
    unsigned typedWrites = 0;

    MMIOBARQuery query_bar(uint8_t slot, MMIOBARBounds &out)
    {
        ++queries;
        out = bars[slot];
        // Even absent/error output is dirty; the policy must ignore it.
        out.valid = true;
        return replies[slot];
    }
};

struct Fixture {
    MMIOBoundsCache cache{};
    MMIOBoundsContext context{0x1000, 41, 41, false, true};
    Metadata metadata{};

    MMIOBoundsResult access(uint64_t index = 7, uint64_t offset = 0,
                            uint64_t width = 4, bool write = false)
    {
        const auto result = validate_mmio_bounds(cache, context, index, offset, width, metadata);
        if (result == MMIOBoundsResult::success) {
            if (write) ++metadata.typedWrites;
            else ++metadata.typedReads;
        }
        return result;
    }

    void no_hardware(uint64_t index, uint64_t offset, uint64_t width,
                      MMIOBoundsResult expected)
    {
        const unsigned queries = metadata.queries;
        const unsigned reads = metadata.typedReads;
        const unsigned writes = metadata.typedWrites;
        CHECK(access(index, offset, width) == expected);
        CHECK(access(index, offset, width, true) == expected);
        CHECK(metadata.queries == queries && metadata.typedReads == reads && metadata.typedWrites == writes);
    }
};

static void positive_cache_uses_memory_index_not_bar_slot()
{
    Fixture f;
    CHECK(f.access() == MMIOBoundsResult::success);
    CHECK(f.metadata.queries == 6);
    CHECK(f.cache.provider == f.context.provider && f.cache.generation == f.context.generation);
    const uint64_t last = f.cache.bars[0].size - 8;
    CHECK(prefetchable_bar_contains_words(f.cache, 7, last, 1));
    CHECK(prefetchable_bar_contains_units(f.cache, 7, last, 4, 2));
    CHECK(!prefetchable_bar_contains_units(f.cache, 7, last + 4, 4, 2));
    CHECK(!prefetchable_bar_contains_units(f.cache, 7, last, 3, 1));
    CHECK(!prefetchable_bar_contains_words(f.cache, 7, last, 2));
    CHECK(!prefetchable_bar_contains_words(f.cache, 7, last + 1, 1));
    CHECK(!prefetchable_bar_contains_words(f.cache, 2, 0, 1));
    CHECK(!prefetchable_bar_contains_words(f.cache, 7, 0, UINT64_MAX));
    CHECK(!prefetchable_bar_contains_words(f.cache, 3, 0, 1)); // I/O.
    Fixture ampere;
    ampere.metadata.bars[0].type = 0x00;
    ampere.metadata.replies[1] = MMIOBARQuery::found;
    ampere.metadata.bars[1] = {UINT64_C(1) << 28, 9, 0x0c, false};
    ampere.metadata.replies[2] = MMIOBARQuery::absent;
    CHECK(ampere.access(9) == MMIOBoundsResult::success);
    CHECK(prefetchable_bar_contains_words(ampere.cache, 9, 8, 2));
    CHECK(!prefetchable_bar_contains_words(ampere.cache, 9, (UINT64_C(1) << 28) - 8, 2));
    CHECK(!prefetchable_bar_contains_words(ampere.cache, 7, 8, 2)); // MMIO registers.
    CHECK(!prefetchable_bar_contains_words(ampere.cache, 1, 8, 2)); // BAR slot is not memory index.
    // All successful memory BAR rows in the complete snapshot are reusable.
    for (unsigned count = 0; count < 10000; ++count) {
        CHECK(f.access(7, 16, 8, true) == MMIOBoundsResult::success);
        CHECK(f.access(2, 4, 4) == MMIOBoundsResult::success);
        CHECK(f.access(1, 4095, 1) == MMIOBoundsResult::success); // Memory index 1 is BAR5.
    }
    CHECK(f.metadata.queries == 6 && f.metadata.typedWrites == 10000);
    CHECK(f.access(0) == MMIOBoundsResult::not_found); // BAR0 is not memory index 0.
    CHECK(f.metadata.queries == 12);
    CHECK(f.access(0) == MMIOBoundsResult::not_found); // No negative cache.
    CHECK(f.metadata.queries == 18);
}

static void invalid_requests_never_query_metadata_or_touch_payload()
{
    for (bool warm : {false, true}) {
        Fixture f;
        if (warm) CHECK(f.access() == MMIOBoundsResult::success);
        for (uint64_t width : {UINT64_C(0), UINT64_C(3), UINT64_C(16), UINT64_MAX})
            f.no_hardware(7, 0, width, MMIOBoundsResult::bad_argument);
        f.no_hardware(256, 0, 4, MMIOBoundsResult::bad_argument);
        f.no_hardware(UINT64_MAX, 0, 4, MMIOBoundsResult::bad_argument);
        f.no_hardware(7, 1, 2, MMIOBoundsResult::bad_argument);
        f.no_hardware(7, 4, 8, MMIOBoundsResult::bad_argument);
        f.no_hardware(7, UINT64_MAX, 1, MMIOBoundsResult::bad_argument);
        f.no_hardware(7, UINT64_MAX - 7, 8, MMIOBoundsResult::bad_argument);
        f.context.stopping = true;
        f.no_hardware(7, 0, 4, MMIOBoundsResult::unavailable);
        f.context.stopping = false;
        f.context.owned = false;
        f.no_hardware(7, 0, 4, MMIOBoundsResult::unavailable);
        f.context.owned = true;
        ++f.context.generation;
        f.no_hardware(7, 0, 4, MMIOBoundsResult::stale_generation);
        f.context.generation = f.context.expectedGeneration;
        f.context.provider = 0;
        f.no_hardware(7, 0, 4, MMIOBoundsResult::unavailable);
    }
}

static void bounds_and_widths_preserved_on_hits()
{
    Fixture f;
    CHECK(f.access(1, 4095, 1) == MMIOBoundsResult::success);
    for (uint64_t width : {UINT64_C(1), UINT64_C(2), UINT64_C(4), UINT64_C(8)}) {
        CHECK(f.access(1, 4096 - width, width) == MMIOBoundsResult::success);
        CHECK(f.access(1, 4096 - width, width, true) == MMIOBoundsResult::success);
        f.no_hardware(1, 4096, width, MMIOBoundsResult::bad_argument);
    }
    CHECK(f.metadata.queries == 6);
}

static void invalidation_covers_failed_attempt_and_new_lifetimes()
{
    Fixture f;
    CHECK(f.access() == MMIOBoundsResult::success);
    // The production invalidation occurs before Reset/Open/Close/Stop, even
    // when an operation fails and leaves the public generation unchanged.
    f.cache.clear();
    CHECK(f.access() == MMIOBoundsResult::success && f.metadata.queries == 12);
    ++f.context.generation;
    f.no_hardware(7, 0, 4, MMIOBoundsResult::stale_generation);
    f.context.expectedGeneration = f.context.generation;
    CHECK(f.access() == MMIOBoundsResult::success && f.metadata.queries == 18);
    f.context.provider = 0x2000; // No cache sharing between provider instances.
    f.metadata.bars[0].size = 4096;
    CHECK(f.access(7, 4096) == MMIOBoundsResult::bad_argument);
    CHECK(f.metadata.queries == 24 && f.cache.provider == 0);
    CHECK(f.access() == MMIOBoundsResult::success && f.metadata.queries == 30);
    CHECK(f.cache.provider == 0x2000);
}

static void failed_scans_never_publish_positive_or_negative_entries()
{
    for (unsigned slot = 0; slot < 6; ++slot) {
        Fixture f;
        f.metadata.replies[slot] = MMIOBARQuery::error;
        CHECK(f.access() == MMIOBoundsResult::metadata_error);
        CHECK(f.metadata.typedReads == 0 && f.metadata.typedWrites == 0);
        CHECK(f.cache.provider == 0);
        const unsigned queries = f.metadata.queries;
        f.metadata = Metadata{};
        CHECK(f.access() == MMIOBoundsResult::success && f.metadata.queries == 6);
        CHECK(queries == slot + 1);
    }
    Fixture f;
    CHECK(f.access() == MMIOBoundsResult::success);
    f.metadata.replies[0] = MMIOBARQuery::error;
    CHECK(f.access(99) == MMIOBoundsResult::metadata_error);
    CHECK(f.cache.provider == 0); // Failed rescan discards the former snapshot.
    CHECK(f.access() == MMIOBoundsResult::metadata_error);
}

static void malformed_and_ambiguous_metadata_never_reaches_typed_io()
{
    for (uint8_t type : {uint8_t(0x02), uint8_t(0x03), uint8_t(0x06), uint8_t(0x10), uint8_t(0xff)}) {
        Fixture f;
        f.metadata.bars[5].type = type;
        CHECK(f.access() == MMIOBoundsResult::metadata_error && f.metadata.typedReads == 0);
        CHECK(f.cache.provider == 0);
    }
    for (uint64_t size : {UINT64_C(0), UINT64_C(3), UINT64_C(8), UINT64_MAX}) {
        Fixture f;
        f.metadata.bars[5].size = size;
        CHECK(f.access() == MMIOBoundsResult::metadata_error && f.metadata.typedReads == 0);
    }
    Fixture duplicate;
    duplicate.metadata.bars[5].memoryIndex = 7;
    CHECK(duplicate.access() == MMIOBoundsResult::metadata_error);
    CHECK(duplicate.cache.provider == 0 && duplicate.metadata.typedReads == 0);
    Fixture upper;
    upper.metadata.replies[1] = MMIOBARQuery::found;
    upper.metadata.bars[1] = {4096, 9, 0x00, false};
    CHECK(upper.access() == MMIOBoundsResult::metadata_error);
    CHECK(upper.cache.provider == 0 && upper.metadata.typedReads == 0);
    Fixture final64;
    final64.metadata.bars[5].type = 0x04;
    CHECK(final64.access() == MMIOBoundsResult::metadata_error);
    Fixture tooLarge32;
    tooLarge32.metadata.bars[5].size = UINT64_C(1) << 33;
    CHECK(tooLarge32.access() == MMIOBoundsResult::metadata_error);
    Fixture io;
    CHECK(io.access(3) == MMIOBoundsResult::unsupported);
    CHECK(io.cache.provider == 0 && io.metadata.typedReads == 0);
    CHECK(io.access(3) == MMIOBoundsResult::unsupported && io.metadata.queries == 12);
}

static void all_supported_memory_types_and_large_64bit_bounds()
{
    for (uint8_t type : {uint8_t(0), uint8_t(4), uint8_t(8), uint8_t(12)}) {
        Fixture f;
        f.metadata.bars[0].type = type;
        f.metadata.bars[0].size = type & 4 ? UINT64_C(1) << 63 : UINT64_C(1) << 32;
        const auto size = f.metadata.bars[0].size;
        CHECK(f.access(7, size - 8, 8) == MMIOBoundsResult::success);
        f.no_hardware(7, size, 8, MMIOBoundsResult::bad_argument);
        CHECK(f.metadata.queries == 6);
    }
}

static void memory_enable_reuse_requires_a_recent_positive_check()
{
    MMIOMemoryEnableCheck check{};
    constexpr uint64_t start = 1000;
    CHECK(!check.valid_at(start));
    CHECK(check.record(start, 0x0006));
    CHECK(!check.valid_at(start - 1));
    CHECK(check.valid_at(start));
    CHECK(check.valid_at(start + MMIOMemoryEnableCheck::maxAgeNs - 1));
    CHECK(!check.valid_at(start + MMIOMemoryEnableCheck::maxAgeNs));
    CHECK(!check.record(start + 10, UINT16_MAX));
    CHECK(!check.valid_at(start + 10));
    CHECK(!check.record(start + 10, 0x0004));
    CHECK(!check.valid_at(start + 10));
    CHECK(check.record(start + 10, 0x0002));
    check = {};
    CHECK(!check.valid_at(start + 10));

    Fixture f;
    CHECK(f.access(7) == MMIOBoundsResult::success);
    CHECK(prefetchable_mmio_bar(find_mmio_bar(f.cache, 7)));
    CHECK(!prefetchable_mmio_bar(find_mmio_bar(f.cache, 1)));
    CHECK(!prefetchable_mmio_bar(find_mmio_bar(f.cache, 8)));
}

int main()
{
    positive_cache_uses_memory_index_not_bar_slot();
    invalid_requests_never_query_metadata_or_touch_payload();
    bounds_and_widths_preserved_on_hits();
    invalidation_covers_failed_attempt_and_new_lifetimes();
    failed_scans_never_publish_positive_or_negative_entries();
    malformed_and_ambiguous_metadata_never_reaches_typed_io();
    all_supported_memory_types_and_large_64bit_bounds();
    memory_enable_reuse_requires_a_recent_positive_check();
    std::puts("PASS actual shared MMIO bounds policy: positive generation/provider cache, no repeated metadata queries, typed access gates and fail-closed metadata; CPU only");
    return 0;
}
