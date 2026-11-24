/* Actual SDK state/lifetime logic with a private fake IOKit transport.
 * These tests never access a physical device and do not qualify DriverKit. */
#include "extgpu_test.h"
#include <extgpu/extgpu_driver_abi.h>
#include <extgpu/private_mmio_ring.h>
#include <IOKit/IOReturn.h>
#include <IOKit/IOTypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); } } while (0)

typedef struct fake_driver {
    unsigned calls[EXTGPU_SELECTOR_MMIO_RING_KICK + 1], maps, unmaps, closes;
    uint64_t generation, capabilities, bar_bytes, mapped_bytes;
    uint64_t dma_iova, dma_bytes;
    uint64_t dma_budget;
    uint32_t bar_type, last_map_options;
    bool wrong_major, wrong_fingerprint, short_query, claimed;
    bool busy_release, fail_unmap, fail_retire, bad_coverage, registered, fail_irq_mask;
    bool fail_map, failed_map_has_address;
    bool dma_requested, drop_budget_reply;
    bool drop_ring_reply;
    uint64_t ring_idle_us;
    uint64_t ring_flags;
    struct openpci_mmio_ring ring;
    unsigned char page[16384];
} fake_driver;

static fake_driver fresh(void)
{
    fake_driver f = {0};
    f.generation = 101;
    f.capabilities = UINT64_C(0xfff);
    f.bar_bytes = 4096;
    f.mapped_bytes = 4096;
    f.dma_budget = EXTGPU_DEFAULT_DMA_BUDGET_BYTES;
    return f;
}

static int32_t emit(uint64_t *out, uint32_t *capacity, const uint64_t *values, uint32_t n)
{
    if (!capacity || *capacity < n || (n && !out)) return kIOReturnNoSpace;
    if (n) memcpy(out, values, n * sizeof(*values));
    *capacity = n;
    return kIOReturnSuccess;
}

static int32_t scalar(void *opaque, uint32_t selector, const uint64_t *in,
                      uint32_t n, uint64_t *out, uint32_t *capacity)
{
    fake_driver *f = opaque;
    if (selector >= sizeof(f->calls) / sizeof(f->calls[0])) return kIOReturnUnsupported;
    ++f->calls[selector];
    switch (selector) {
    case EXTGPU_SELECTOR_QUERY_PROTOCOL: {
        CHECK(n == 3 && in[0] == EXTGPU_PROTOCOL_MAJOR && in[2] == EXTGPU_PROTOCOL_FINGERPRINT);
        uint64_t values[8] = {f->wrong_major ? 999 : EXTGPU_PROTOCOL_MAJOR,
            EXTGPU_PROTOCOL_MINOR, f->generation, f->capabilities,
            EXTGPU_MAX_DMA_REGIONS, f->dma_budget,
            EXTGPU_MAX_RETURNED_SEGMENTS, f->wrong_fingerprint ? 0 : EXTGPU_PROTOCOL_FINGERPRINT};
        return emit(out, capacity, values, f->short_query ? 7 : 8);
    }
    case EXTGPU_SELECTOR_CLAIM:
        CHECK(n == 0);
        if (f->claimed) return kIOReturnExclusiveAccess;
        f->claimed = true;
        return emit(out, capacity, NULL, 0);
    case EXTGPU_SELECTOR_RELEASE_OWNER:
        CHECK(n == 1 && in[0] == f->generation);
        if (f->busy_release || f->registered) return kIOReturnBusy;
        f->claimed = false;
        f->dma_budget = EXTGPU_DEFAULT_DMA_BUDGET_BYTES;
        f->dma_requested = false;
        return emit(out, capacity, NULL, 0);
    case EXTGPU_SELECTOR_CONFIG_READ: {
        CHECK(n == 2 && f->claimed);
        uint64_t value = UINT64_C(0x687f1002);
        return emit(out, capacity, &value, 1);
    }
    case EXTGPU_SELECTOR_CONFIG_WRITE:
        CHECK(n == 3 && f->claimed);
        return emit(out, capacity, NULL, 0);
    case EXTGPU_SELECTOR_GET_BAR_INFO: {
        CHECK(n == 1 && in[0] < 6 && f->claimed);
        uint64_t values[3] = {2, f->bar_bytes, f->bar_type};
        return emit(out, capacity, values, 3);
    }
    case EXTGPU_SELECTOR_REGISTER_DMA_REGION: {
        CHECK(n == 3 && f->claimed);
        f->dma_requested = true;
        if (in[2] > f->dma_budget) return kIOReturnNoSpace;
        if (f->registered) return kIOReturnStillOpen;
        f->registered = true;
        f->dma_iova = in[0];
        f->dma_bytes = in[2];
        uint64_t values[3] = {f->bad_coverage ? 2 : 1, UINT64_C(0x100000), in[2]};
        return emit(out, capacity, values, 3);
    }
    case EXTGPU_SELECTOR_UNREGISTER_DMA_REGION:
        CHECK(n == 1 && in[0] == f->dma_iova);
        if (f->fail_retire) return kIOReturnBusy;
        f->registered = false;
        return emit(out, capacity, NULL, 0);
    case EXTGPU_SELECTOR_RESET_DEVICE:
        CHECK(n == 0 && f->claimed && !f->registered);
        ++f->generation;
        return emit(out, capacity, NULL, 0);
    case EXTGPU_SELECTOR_SET_DMA_ADDRESS_BITS:
        CHECK(n == 1 && in[0] >= 32 && in[0] <= 64);
        return emit(out, capacity, NULL, 0);
    case EXTGPU_SELECTOR_SET_DMA_BUDGET:
        CHECK(n == EXTGPU_DMA_BUDGET_WORDS && f->claimed && in[0] == f->generation && in[1]);
        if (f->dma_requested) return kIOReturnStillOpen;
        f->dma_budget = in[1];
        if (f->drop_budget_reply) return kIOReturnError;
        return emit(out, capacity, in, EXTGPU_DMA_BUDGET_WORDS);
    case EXTGPU_SELECTOR_MMIO_RING_KICK: {
        CHECK(n == (f->ring_flags ? 4 : f->ring_idle_us ? 3 : 2) && f->claimed && in[0] == f->generation);
        if (f->ring_idle_us) CHECK(in[2] == f->ring_idle_us);
        if (f->ring_flags) CHECK(in[3] == f->ring_flags);
        CHECK(in[1] == f->ring.published && f->ring.active == OPENPCI_RING_ACTIVE);
        CHECK(in[1] > f->ring.completed && in[1] - f->ring.completed <= EXTGPU_BAR_WORDS_MAX);
        if (f->drop_ring_reply) return kIOReturnError;
        for (uint64_t sequence = f->ring.completed; sequence < in[1]; ++sequence) {
            struct openpci_mmio_ring_slot *slot = openpci_mmio_ring_slot_at(&f->ring, sequence);
            if (slot->operation == OPENPCI_RING_WRITE_GROUPED) {
                const uint64_t word = sequence - f->ring.completed;
                CHECK(in[1] - f->ring.completed == 2 && slot->index == 2 && slot->width == 4);
                CHECK(slot->offset == 32 + 4 * word && slot->input == 0x91 + word);
                continue;
            }
            CHECK(slot->index == 2 && slot->width == 8 && slot->offset % 8 == 0);
            if (slot->operation == OPENPCI_RING_READ_WORDS) slot->output = slot->offset;
            else if (slot->operation == OPENPCI_RING_WRITE_WORDS) {
                const uint64_t word = sequence - f->ring.completed;
                CHECK(in[1] - f->ring.completed == 8 && slot->offset == 0x80 + 8 * word);
                CHECK(slot->input == 0x9876);
            }
            else CHECK(slot->operation == OPENPCI_RING_WRITE && slot->input == 0x1234);
        }
        f->ring.completed = in[1];
        CHECK(openpci_mmio_ring_disarm(&f->ring, in[1]));
        return emit(out, capacity, NULL, 0);
    }
    case EXTGPU_SELECTOR_SETUP_INTERRUPTS: {
        CHECK(n == 0 && f->claimed);
        uint64_t count = 8;
        return emit(out, capacity, &count, 1);
    }
    case EXTGPU_SELECTOR_RETIRE_INTERRUPTS:
        CHECK(n == 0 && f->claimed);
        return emit(out, capacity, NULL, 0);
    case EXTGPU_SELECTOR_SET_IRQ_MASK:
        CHECK(n == 4 && f->claimed);
        if (f->fail_irq_mask) return kIOReturnBusy;
        return emit(out, capacity, NULL, 0);
    case EXTGPU_SELECTOR_CHECK_INTERRUPT: {
        CHECK(n == 0 && f->claimed);
        uint64_t pending[4] = {0};
        return emit(out, capacity, pending, 4);
    }
    default:
        return kIOReturnUnsupported;
    }
}

static int32_t map(void *opaque, uint32_t type, uint64_t *address, uint64_t *size, uint32_t options)
{
    fake_driver *f = opaque;
    if (type == EXTGPU_MEMORY_MMIO_RING) {
        CHECK(f->claimed);
        f->ring.generation = f->generation;
        *address = (uintptr_t)&f->ring;
        *size = sizeof(f->ring);
        ++f->maps;
        return kIOReturnSuccess;
    }
    CHECK(type >= EXTGPU_MEMORY_BAR0 && type <= EXTGPU_MEMORY_BAR5 && f->claimed);
    ++f->maps;
    f->last_map_options = options;
    if (f->fail_map && !f->failed_map_has_address) return kIOReturnUnsupported;
    *address = (uintptr_t)f->page;
    *size = f->mapped_bytes;
    return f->fail_map ? kIOReturnError : kIOReturnSuccess;
}

static int32_t unmap(void *opaque, uint32_t type, uint64_t address)
{
    fake_driver *f = opaque;
    if (type == EXTGPU_MEMORY_MMIO_RING) {
        CHECK(address == (uintptr_t)&f->ring);
        ++f->unmaps;
        return kIOReturnSuccess;
    }
    CHECK(type >= EXTGPU_MEMORY_BAR0 && type <= EXTGPU_MEMORY_BAR5);
    CHECK(address == (uintptr_t)f->page);
    ++f->unmaps;
    return f->fail_unmap ? kIOReturnBusy : kIOReturnSuccess;
}

static int32_t close_connection(void *opaque)
{
    fake_driver *f = opaque;
    CHECK(!f->claimed && !f->registered);
    ++f->closes;
    return kIOReturnSuccess;
}

static const extgpu_test_transport transport = {scalar, map, unmap, close_connection};

static void open_device(fake_driver *f, extgpu_session **session, extgpu_device **device)
{
    CHECK(extgpu_test_open(&transport, f, session, device) == EXTGPU_OK);
    CHECK(f->calls[EXTGPU_SELECTOR_QUERY_PROTOCOL] == 1);
    CHECK(f->calls[EXTGPU_SELECTOR_CLAIM] == 0);
}

static void finish(extgpu_session **session, extgpu_device **device)
{
    CHECK(extgpu_device_release(*device) == EXTGPU_OK);
    CHECK(extgpu_device_close(device) == EXTGPU_OK && !*device);
    CHECK(extgpu_session_destroy(session) == EXTGPU_OK && !*session);
}

static void protocol_rejection(void)
{
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        fake_driver f = fresh();
        f.wrong_major = scenario == 0;
        f.wrong_fingerprint = scenario == 1;
        f.short_query = scenario == 2;
        extgpu_session *s = NULL;
        extgpu_device *d = NULL;
        CHECK(extgpu_test_open(&transport, &f, &s, &d) == EXTGPU_E_PROTOCOL);
        CHECK(!s && !d && !f.calls[EXTGPU_SELECTOR_CLAIM]);
    }
}

static void owner_and_bounds(void)
{
    fake_driver f = fresh();
    extgpu_session *s = NULL;
    extgpu_device *d = NULL;
    open_device(&f, &s, &d);
    uint32_t value = 0;
    CHECK(extgpu_config_read(d, 0, 4, &value) == EXTGPU_E_NOT_CLAIMED);
    CHECK(f.calls[EXTGPU_SELECTOR_CONFIG_READ] == 0);
    CHECK(extgpu_device_claim(d) == EXTGPU_OK);
    CHECK(extgpu_device_claim(d) == EXTGPU_E_BUSY && f.calls[EXTGPU_SELECTOR_CLAIM] == 1);
    CHECK(extgpu_config_read(d, 4096, 4, &value) == EXTGPU_E_ARGUMENT);
    CHECK(extgpu_config_write(d, 4, 1, 0x100) == EXTGPU_E_ARGUMENT);
    CHECK(f.calls[EXTGPU_SELECTOR_CONFIG_WRITE] == 0);
    CHECK(extgpu_config_read(d, 0, 4, &value) == EXTGPU_OK && value == 0x687f1002);
    f.busy_release = true;
    CHECK(extgpu_device_release(d) == EXTGPU_E_BUSY);
    CHECK(extgpu_device_close(&d) == EXTGPU_E_BUSY && d != NULL);
    CHECK(extgpu_session_destroy(&s) == EXTGPU_E_BUSY && s != NULL);
    f.busy_release = false;
    finish(&s, &d);
    CHECK(f.closes == 1 && f.calls[EXTGPU_SELECTOR_TERMINATE] == 0);
}

static void mapping_retention(void)
{
    fake_driver f = fresh();
    extgpu_session *s = NULL;
    extgpu_device *d = NULL;
    extgpu_mapping *m = NULL;
    open_device(&f, &s, &d);
    CHECK(extgpu_device_claim(d) == EXTGPU_OK);
    CHECK(extgpu_bar_map(d, 5, &m) == EXTGPU_OK && m != NULL);
    CHECK(extgpu_mapping_address(m) == f.page && extgpu_mapping_size(m) == 4096);
    CHECK(extgpu_device_release(d) == EXTGPU_E_BUSY);
    CHECK(f.calls[EXTGPU_SELECTOR_RELEASE_OWNER] == 0);
    CHECK(extgpu_device_reset(d) == EXTGPU_E_BUSY);
    f.fail_unmap = true;
    CHECK(extgpu_mapping_unmap(&m) == EXTGPU_E_BUSY && m != NULL);
    CHECK(extgpu_device_close(&d) == EXTGPU_E_BUSY);
    f.fail_unmap = false;
    CHECK(extgpu_mapping_unmap(&m) == EXTGPU_OK && m == NULL);
    finish(&s, &d);

    f = fresh();
    f.mapped_bytes = 16;
    open_device(&f, &s, &d);
    CHECK(extgpu_device_claim(d) == EXTGPU_OK);
    CHECK(extgpu_bar_map(d, 5, &m) == EXTGPU_E_UNSAFE && m != NULL);
    CHECK(extgpu_device_release(d) == EXTGPU_E_BUSY);
    CHECK(extgpu_mapping_unmap(&m) == EXTGPU_OK && m == NULL);
    finish(&s, &d);
}

static void mapping_cache_policy(void)
{
    for (uint32_t type = 0; type <= 8; type += 8) {
        for (extgpu_bar_cache_policy policy = EXTGPU_BAR_CACHE_AUTOMATIC;
             policy <= EXTGPU_BAR_CACHE_WRITE_COMBINE; ++policy) {
            fake_driver f = fresh();
            f.bar_type = type;
            extgpu_session *s = NULL;
            extgpu_device *d = NULL;
            extgpu_mapping *m = NULL;
            open_device(&f, &s, &d);
            CHECK(extgpu_bar_map_with_cache_policy(d, 0, policy, &m) == EXTGPU_E_NOT_CLAIMED);
            CHECK(!m && !f.maps && !f.calls[EXTGPU_SELECTOR_GET_BAR_INFO]);
            CHECK(extgpu_device_claim(d) == EXTGPU_OK);
            CHECK(extgpu_bar_map_with_cache_policy(d, 0, UINT32_MAX, &m) == EXTGPU_E_ARGUMENT);
            CHECK(!m && !f.maps && !f.calls[EXTGPU_SELECTOR_GET_BAR_INFO]);
            if (!type && policy == EXTGPU_BAR_CACHE_WRITE_COMBINE) {
                CHECK(extgpu_bar_map_with_cache_policy(d, 0, policy, &m) == EXTGPU_E_UNSUPPORTED);
                CHECK(!m && !f.maps);
            } else {
                CHECK(extgpu_bar_map_with_cache_policy(d, 0, policy, &m) == EXTGPU_OK && m);
                uint32_t expected = kIOMapAnywhere;
                if (policy == EXTGPU_BAR_CACHE_INHIBIT) expected |= kIOMapInhibitCache;
                else if (policy == EXTGPU_BAR_CACHE_WRITE_COMBINE ||
                         (policy == EXTGPU_BAR_CACHE_AUTOMATIC && type)) expected |= kIOMapWriteCombineCache;
                CHECK(f.maps == 1 && f.last_map_options == expected);
                extgpu_mapping *other = NULL;
                CHECK(extgpu_bar_map_with_cache_policy(d, 0, policy, &other) == EXTGPU_E_BUSY);
                CHECK(!other && f.maps == 1);
                CHECK(extgpu_mapping_unmap(&m) == EXTGPU_OK && !m);
                /* Equivalent native bits are allowed, not a policy-name lock. */
                extgpu_bar_cache_policy equivalent = (expected & kIOMapWriteCombineCache) ?
                    EXTGPU_BAR_CACHE_WRITE_COMBINE : policy;
                CHECK(extgpu_bar_map_with_cache_policy(d, 0, equivalent, &m) == EXTGPU_OK);
                CHECK(f.maps == 2 && f.last_map_options == expected);
                CHECK(extgpu_mapping_unmap(&m) == EXTGPU_OK);
                /* No cache-mode changes after unmap, including after reset. */
                extgpu_bar_cache_policy changed = policy == EXTGPU_BAR_CACHE_INHIBIT ?
                    EXTGPU_BAR_CACHE_DEFAULT : EXTGPU_BAR_CACHE_INHIBIT;
                CHECK(extgpu_bar_map_with_cache_policy(d, 0, changed, &m) == EXTGPU_E_BUSY);
                CHECK(!m && f.maps == 2);
                CHECK(extgpu_device_reset(d) == EXTGPU_OK);
                CHECK(extgpu_bar_map_with_cache_policy(d, 0, changed, &m) == EXTGPU_E_BUSY);
                CHECK(!m && f.maps == 2);
            }
            finish(&s, &d);
        }
    }
    for (unsigned retained = 0; retained < 2; ++retained) {
        fake_driver f = fresh();
        f.fail_map = true;
        f.failed_map_has_address = retained;
        extgpu_session *s = NULL;
        extgpu_device *d = NULL;
        extgpu_mapping *m = NULL;
        open_device(&f, &s, &d);
        CHECK(extgpu_device_claim(d) == EXTGPU_OK);
        CHECK(extgpu_bar_map_with_cache_policy(d, 0, EXTGPU_BAR_CACHE_INHIBIT, &m) ==
              (retained ? EXTGPU_E_UNSAFE : EXTGPU_E_UNSUPPORTED));
        CHECK(f.maps == 1 && f.last_map_options == (kIOMapAnywhere | kIOMapInhibitCache));
        if (retained) {
            CHECK(m && extgpu_device_release(d) == EXTGPU_E_BUSY);
            f.fail_unmap = true;
            CHECK(extgpu_mapping_unmap(&m) == EXTGPU_E_BUSY && m);
            f.fail_unmap = false;
            CHECK(extgpu_mapping_unmap(&m) == EXTGPU_OK && !m);
        } else CHECK(!m);
        CHECK(extgpu_bar_map(d, 0, &m) == EXTGPU_E_BUSY && !m && f.maps == 1);
        f.fail_map = false;
        CHECK(extgpu_bar_map_with_cache_policy(d, 0, EXTGPU_BAR_CACHE_INHIBIT, &m) == EXTGPU_OK);
        CHECK(f.maps == 2 && extgpu_mapping_unmap(&m) == EXTGPU_OK);
        /* A different BAR may request independent native flags. */
        CHECK(extgpu_bar_map_with_cache_policy(d, 5, EXTGPU_BAR_CACHE_DEFAULT, &m) == EXTGPU_OK);
        CHECK(f.maps == 3 && f.last_map_options == kIOMapAnywhere);
        CHECK(extgpu_mapping_unmap(&m) == EXTGPU_OK);
        finish(&s, &d);
    }
    fake_driver f = fresh();
    f.bar_type = 1; /* I/O port BARs never enter the cache-policy/export path. */
    extgpu_session *s = NULL;
    extgpu_device *d = NULL;
    extgpu_mapping *m = NULL;
    open_device(&f, &s, &d);
    CHECK(extgpu_device_claim(d) == EXTGPU_OK);
    for (extgpu_bar_cache_policy policy = 0; policy <= EXTGPU_BAR_CACHE_WRITE_COMBINE; ++policy)
        CHECK(extgpu_bar_map_with_cache_policy(d, 0, policy, &m) == EXTGPU_E_UNSUPPORTED && !m);
    CHECK(!f.maps);
    finish(&s, &d);
}

static void dma_lifetime(void)
{
    const uint64_t size = (uint64_t)sysconf(_SC_PAGESIZE);
    void *backing = NULL;
    CHECK(size && posix_memalign(&backing, (size_t)size, (size_t)size * 2) == 0);
    fake_driver f = fresh();
    extgpu_session *s = NULL;
    extgpu_device *d = NULL;
    extgpu_buffer *buffer = NULL, *duplicate = NULL;
    extgpu_dma_coverage coverage;
    open_device(&f, &s, &d);
    CHECK(extgpu_device_claim(d) == EXTGPU_OK);
    CHECK(extgpu_dma_register(d, size * 64, backing, size * 2, &buffer, &coverage) == EXTGPU_OK);
    CHECK(buffer && coverage.complete && f.registered);
    CHECK(extgpu_dma_register(d, size * 64, backing, size, &duplicate, &coverage) != EXTGPU_OK);
    CHECK(duplicate == NULL && f.calls[EXTGPU_SELECTOR_REGISTER_DMA_REGION] == 1);
    CHECK(extgpu_dma_register(d, size * 65, backing, size, &duplicate, &coverage) != EXTGPU_OK);
    CHECK(duplicate == NULL && f.calls[EXTGPU_SELECTOR_REGISTER_DMA_REGION] == 1);
    CHECK(f.calls[EXTGPU_SELECTOR_UNREGISTER_DMA_REGION] == 0 && f.registered);
    f.fail_retire = true;
    CHECK(extgpu_buffer_unregister(&buffer) == EXTGPU_E_UNSAFE && buffer != NULL);
    CHECK(extgpu_device_release(d) == EXTGPU_E_BUSY && f.registered);
    f.fail_retire = false;
    CHECK(extgpu_buffer_unregister(&buffer) == EXTGPU_OK && buffer == NULL && !f.registered);
    finish(&s, &d);

    for (unsigned retain = 0; retain < 2; ++retain) {
        f = fresh();
        f.bad_coverage = true;
        f.fail_retire = retain != 0;
        open_device(&f, &s, &d);
        CHECK(extgpu_device_claim(d) == EXTGPU_OK);
        extgpu_status st = extgpu_dma_register(d, size * 64, backing, size, &buffer, &coverage);
        if (retain) {
            CHECK(st == EXTGPU_E_UNSAFE && buffer != NULL && f.registered);
            f.fail_retire = false;
            CHECK(extgpu_buffer_unregister(&buffer) == EXTGPU_OK && buffer == NULL);
        } else {
            CHECK(st == EXTGPU_E_COVERAGE && buffer == NULL && !f.registered);
        }
        finish(&s, &d);
    }
    free(backing);
}

static void unexpected_irq(extgpu_irq *irq, void *context)
{
    (void)irq;
    (void)context;
    CHECK(!"No physical interrupt or successful async connection exists in this test");
}

static void irq_partial_setup_cleanup(void)
{
    fake_driver f = fresh();
    extgpu_session *s = NULL;
    extgpu_device *d = NULL;
    extgpu_irq *irq = NULL;
    uint32_t vectors = 0;
    open_device(&f, &s, &d);
    CHECK(extgpu_device_claim(d) == EXTGPU_OK);
    /* Scalar SetupInterrupts is injected, but there is no IOKit connection.
     * Real asynchronous IOKit arming therefore fails. The owned notification
     * port and dispatch cancellation/drain are real Darwin resources. */
    CHECK(extgpu_irq_create(d, unexpected_irq, NULL, &irq, &vectors) == EXTGPU_E_UNSAFE);
    CHECK(irq != NULL && vectors == 8);
    CHECK(extgpu_device_release(d) == EXTGPU_E_BUSY);
    f.fail_irq_mask = true;
    CHECK(extgpu_irq_close(&irq) == EXTGPU_E_UNSAFE && irq != NULL);
    f.fail_irq_mask = false;
    CHECK(extgpu_irq_close(&irq) == EXTGPU_OK && irq == NULL);
    finish(&s, &d);
}

static void reset_generation(void)
{
    fake_driver f = fresh();
    extgpu_session *s = NULL;
    extgpu_device *d = NULL;
    extgpu_protocol_info before, after;
    open_device(&f, &s, &d);
    CHECK(extgpu_device_claim(d) == EXTGPU_OK);
    CHECK(extgpu_device_protocol(d, &before) == EXTGPU_OK);
    unsigned queries_before_reset = f.calls[EXTGPU_SELECTOR_QUERY_PROTOCOL];
    CHECK(extgpu_device_reset(d) == EXTGPU_OK);
    CHECK(extgpu_device_protocol(d, &after) == EXTGPU_OK && after.generation != before.generation);
    CHECK(f.calls[EXTGPU_SELECTOR_QUERY_PROTOCOL] > queries_before_reset);
    CHECK(extgpu_device_set_dma_address_bits(d, 48) == EXTGPU_OK);
    finish(&s, &d);
}

static void dma_budget_negotiation(void)
{
    const uint64_t large = UINT64_C(768) * 1024 * 1024;
    const uint64_t requested = UINT64_C(2) * 1024 * 1024 * 1024;
    fake_driver f = fresh();
    extgpu_session *s = NULL;
    extgpu_device *d = NULL;
    open_device(&f, &s, &d);
    CHECK(extgpu_device_claim(d) == EXTGPU_OK);
    CHECK(extgpu_device_set_dma_budget(d, requested) == EXTGPU_E_UNSUPPORTED);
    CHECK(f.calls[EXTGPU_SELECTOR_SET_DMA_BUDGET] == 0);
    finish(&s, &d);

    f = fresh();
    f.capabilities |= EXTGPU_CAP_CONFIGURABLE_DMA_BUDGET;
    open_device(&f, &s, &d);
    CHECK(extgpu_device_set_dma_budget(d, requested) == EXTGPU_E_NOT_CLAIMED);
    CHECK(extgpu_device_claim(d) == EXTGPU_OK);
    CHECK(extgpu_device_set_dma_budget(d, 0) == EXTGPU_E_ARGUMENT);
    void *memory = mmap(NULL, large, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(memory != MAP_FAILED);
    extgpu_buffer *buffer = NULL;
    extgpu_dma_coverage coverage;
    CHECK(extgpu_dma_register(d, 0, memory, large, &buffer, &coverage) == EXTGPU_E_CAPACITY);
    f.drop_budget_reply = true;
    CHECK(extgpu_device_set_dma_budget(d, requested) == EXTGPU_E_UNSAFE);
    CHECK(extgpu_dma_register(d, 0, memory, large, &buffer, &coverage) == EXTGPU_E_UNSAFE);
    CHECK(f.calls[EXTGPU_SELECTOR_REGISTER_DMA_REGION] == 0 && buffer == NULL);
    extgpu_protocol_info protocol;
    CHECK(extgpu_device_protocol(d, &protocol) == EXTGPU_E_UNSAFE);
    f.drop_budget_reply = false;
    CHECK(extgpu_device_set_dma_budget(d, requested) == EXTGPU_OK);
    CHECK(extgpu_device_protocol(d, &protocol) == EXTGPU_OK && protocol.maximum_dma_bytes == requested);
    CHECK(extgpu_dma_register(d, 0, memory, large, &buffer, &coverage) == EXTGPU_OK);
    CHECK(coverage.covered_bytes == large && extgpu_buffer_size(buffer) == large);
    CHECK(extgpu_device_set_dma_budget(d, large) == EXTGPU_E_BUSY);
    CHECK(extgpu_buffer_unregister(&buffer) == EXTGPU_OK && buffer == NULL);
    CHECK(munmap(memory, large) == 0);
    CHECK(extgpu_device_set_dma_budget(d, large) == EXTGPU_E_BUSY);
    CHECK(extgpu_device_release(d) == EXTGPU_OK);
    CHECK(extgpu_device_claim(d) == EXTGPU_OK);
    CHECK(extgpu_device_protocol(d, &protocol) == EXTGPU_OK &&
          protocol.maximum_dma_bytes == EXTGPU_DEFAULT_DMA_BUDGET_BYTES);
    finish(&s, &d);
}

static void mmio_ring_lifetime(bool extended, uint64_t flags)
{
    fake_driver f = fresh();
    f.capabilities |= EXTGPU_CAP_MMIO_RING | EXTGPU_CAP_BAR_WORDS;
    extgpu_session *s = NULL;
    extgpu_device *d = NULL;
    if (extended) {
        f.capabilities |= EXTGPU_CAP_MMIO_RING_POLL | EXTGPU_CAP_MMIO_RING_GROUPED_STORES;
        f.ring_idle_us = 192;
        CHECK(setenv("OPENPCI_MMIO_RING_POLL_US", "192", 1) == 0);
    } else {
        CHECK(setenv("OPENPCI_MMIO_RING", "1", 1) == 0);
    }
    if (flags) {
        f.capabilities |= EXTGPU_CAP_MMIO_RING_ADAPTIVE;
        f.ring_flags = flags;
    }
    if (flags & OPENPCI_MMIO_RING_ADAPTIVE_FLAG)
        CHECK(setenv("OPENPCI_MMIO_RING_ADAPTIVE", "1", 1) == 0);
    if (flags & OPENPCI_MMIO_RING_IRQ_ARM_FLAG) {
        f.capabilities |= EXTGPU_CAP_MMIO_RING_IRQ_ARM | EXTGPU_CAP_IRQ_SHARED_NOTIFICATION;
        CHECK(setenv("OPENPCI_MMIO_RING_IRQ_ARM", "1", 1) == 0);
    }
    if (flags & OPENPCI_MMIO_RING_EVENT_FLAG) {
        f.capabilities |= EXTGPU_CAP_MMIO_RING_EVENT | EXTGPU_CAP_MMIO_RING_POLL;
        CHECK(setenv("OPENPCI_MMIO_RING_EVENT", "1", 1) == 0);
    }
    open_device(&f, &s, &d);
    CHECK(extgpu_device_claim(d) == EXTGPU_OK);
    for (unsigned i = 0; i < OPENPCI_MMIO_RING_SLOTS - 1; ++i)
        CHECK(extgpu_mmio_write(d, 2, 0, 8, 0x1234) == EXTGPU_OK);
    uint64_t outputs[3] = {0};
    CHECK(extgpu_bar_words(d, 2, 8, EXTGPU_BAR_WORDS_READ, 3, outputs) == EXTGPU_OK);
    CHECK(outputs[0] == 8 && outputs[1] == 16 && outputs[2] == 24);
    CHECK(f.maps == 1 && f.ring.completed == OPENPCI_MMIO_RING_SLOTS + 2);
    const uint64_t pair[2] = {0x91, 0x92};
    CHECK(extgpu_mmio_write_grouped(d, 2, 33, 4, 2, pair) == EXTGPU_E_ARGUMENT);
    CHECK(extgpu_mmio_write_grouped(d, 2, 32, 4, 2, pair) ==
          (extended ? EXTGPU_OK : EXTGPU_E_UNSUPPORTED));
    CHECK(f.ring.completed == OPENPCI_MMIO_RING_SLOTS + (extended ? 4 : 2));
    uint64_t fill = 0x9876;
    CHECK(extgpu_bar_words(d, 2, 0x80, EXTGPU_BAR_WORDS_FILL, 8, &fill) == EXTGPU_OK);
    CHECK(f.ring.completed == OPENPCI_MMIO_RING_SLOTS + (extended ? 12 : 10));
    f.drop_ring_reply = true;
    CHECK(extgpu_mmio_write(d, 2, 0, 8, 0x1234) == EXTGPU_E_UNSAFE);
    unsigned kicks = f.calls[EXTGPU_SELECTOR_MMIO_RING_KICK];
    CHECK(extgpu_mmio_write(d, 2, 0, 8, 0x1234) == EXTGPU_E_UNSAFE);
    CHECK(extgpu_bar_words(d, 2, 8, EXTGPU_BAR_WORDS_FILL, 3, outputs) == EXTGPU_E_UNSAFE);
    CHECK(f.calls[EXTGPU_SELECTOR_MMIO_RING_KICK] == kicks);
    CHECK(f.calls[EXTGPU_SELECTOR_MMIO_WRITE] == 0 && f.calls[EXTGPU_SELECTOR_BAR_WORDS] == 0);
    finish(&s, &d);
    CHECK(f.unmaps == 1);
    CHECK(unsetenv("OPENPCI_MMIO_RING") == 0);
    CHECK(unsetenv("OPENPCI_MMIO_RING_POLL_US") == 0);
    CHECK(unsetenv("OPENPCI_MMIO_RING_ADAPTIVE") == 0);
    CHECK(unsetenv("OPENPCI_MMIO_RING_IRQ_ARM") == 0);
    CHECK(unsetenv("OPENPCI_MMIO_RING_EVENT") == 0);
}

int main(void)
{
    protocol_rejection();
    owner_and_bounds();
    mapping_retention();
    mapping_cache_policy();
    dma_lifetime();
    dma_budget_negotiation();
    irq_partial_setup_cleanup();
    reset_generation();
    mmio_ring_lifetime(false, 0);
    mmio_ring_lifetime(true, 0);
    mmio_ring_lifetime(true, OPENPCI_MMIO_RING_ADAPTIVE_FLAG);
    mmio_ring_lifetime(true, OPENPCI_MMIO_RING_ADAPTIVE_FLAG | OPENPCI_MMIO_RING_IRQ_ARM_FLAG);
    mmio_ring_lifetime(false, OPENPCI_MMIO_RING_EVENT_FLAG);
    puts("PASS real SDK protocol, ownership, bounds, retained resources and Darwin IRQ cancellation; no hardware");
    return 0;
}
