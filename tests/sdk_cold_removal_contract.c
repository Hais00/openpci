/* Exercise the actual SDK terminal cold-removal API with private transport and
 * registry-observation injection. No native provider lookup or hardware I/O. */
#include "extgpu_test.h"
#include <extgpu/extgpu_driver_abi.h>
#include <IOKit/IOReturn.h>
#include <IOKit/IOTypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); } } while (0)
_Static_assert(sizeof(extgpu_cold_removal_info) == 6 * sizeof(uint64_t), "new cold info layout");

static void unexpected_irq(extgpu_irq *irq, void *context)
{ (void)irq; (void)context; CHECK(!"unexpected IRQ callback"); }

typedef struct region {
    uint64_t iova, bytes, sequence;
    bool registered, tombstone;
} region;
typedef struct fake_driver {
    uint64_t generation, minor, capabilities, key[5], enrolled, private_cap[2];
    uint64_t broker_id, broker_actual_id, broker_caps, broker_minor, broker_epoch, first_escrow_input[7];
    const char *broker_class;
    unsigned calls[32], maps, unmaps, closes, retire_commits, finish_commits;
    unsigned broker_calls[32], broker_lookups, broker_opens, broker_closes, original_drops;
    bool claimed, frozen, finished, allocated, fail_map, fail_close;
    bool prepared, broker_bound, original_gone, broker_gone, bind_wrong_owner, escrow_input_seen;
    uint32_t fail_selector, malformed_selector;
    uint32_t broker_fail_selector, broker_malformed_selector, fail_before_selector;
    int32_t failure;
    int32_t broker_failure, native_retire_failure, drop_failure;
    int bad_field;
    int broker_bad_field;
    bool bad_count;
    bool broker_bad_count;
    region regions[4];
    void *allocation;
    uint64_t allocation_bytes;
} fake_driver;
typedef struct fixture {
    fake_driver fake;
    extgpu_session *session;
    extgpu_device *device;
} fixture;

static fake_driver fresh(void)
{
    fake_driver f = {0};
    f.generation = 101;
    f.minor = 3;
    f.capabilities = UINT64_C(0x3fff);
    f.broker_id = f.broker_actual_id = f.broker_epoch = 0xb001;
    f.broker_class = EXTGPU_RECOVERY_BROKER_CLASS;
    f.broker_minor = 3;
    f.broker_caps = EXTGPU_RECOVERY_BROKER_CAPABILITIES;
    f.fail_selector = f.malformed_selector = f.broker_fail_selector = f.broker_malformed_selector = f.fail_before_selector = UINT32_MAX;
    f.bad_field = f.broker_bad_field = -1;
    return f;
}
static int32_t emit(fake_driver *f, uint32_t selector, uint64_t *out,
                     uint32_t *capacity, uint64_t *values, uint32_t count)
{
    if (f->fail_selector == selector) return f->failure;
    CHECK(capacity && *capacity >= count && (!count || out));
    if (f->malformed_selector == selector) {
        if (f->bad_field >= 0) {
            CHECK((unsigned)f->bad_field < count);
            if ((selector == EXTGPU_SELECTOR_COLD_RETIRE && f->bad_field == 9) ||
                (selector == EXTGPU_SELECTOR_COLD_FINISH && f->bad_field == 6))
                values[f->bad_field] = 0;
            else values[f->bad_field] ^= 1;
        }
        if (f->bad_count) --count;
    }
    if (count) memcpy(out, values, count * sizeof(*out));
    *capacity = count;
    return kIOReturnSuccess;
}
static uint64_t registered_count(fake_driver *f)
{
    uint64_t result = 0;
    for (unsigned n = 0; n < 4; ++n) if (f->regions[n].registered) ++result;
    return result;
}
static int32_t scalar(void *opaque, uint32_t selector, const uint64_t *in,
                      uint32_t count, uint64_t *out, uint32_t *capacity)
{
    fake_driver *f = opaque;
    CHECK(selector < 32);
    ++f->calls[selector];
    if (f->original_gone) return kIOReturnNoDevice;
    if (selector == EXTGPU_SELECTOR_PREPARE_RECOVERY_ESCROW) {
        CHECK(count == 7);
        if (!f->escrow_input_seen) { memcpy(f->first_escrow_input, in, sizeof(f->first_escrow_input)); f->escrow_input_seen = true; }
        else CHECK(!memcmp(f->first_escrow_input, in, sizeof(f->first_escrow_input)));
    }
    if (f->fail_before_selector == selector) return kIOReturnError;
    switch (selector) {
    case EXTGPU_SELECTOR_QUERY_PROTOCOL: {
        CHECK(count == 3 && in[0] == 1 && in[1] == EXTGPU_PROTOCOL_MINIMUM_MINOR && in[1] == 1);
        CHECK(in[2] == EXTGPU_PROTOCOL_FINGERPRINT && !f->frozen);
        uint64_t reply[8] = {1, f->minor, f->generation, f->capabilities,
            EXTGPU_MAX_DMA_REGIONS, EXTGPU_MAX_PREPARED_DMA_BYTES, 1, EXTGPU_PROTOCOL_FINGERPRINT};
        return emit(f, selector, out, capacity, reply, 8);
    }
    case EXTGPU_SELECTOR_CLAIM:
        CHECK(count == 0 && !f->claimed && !f->frozen);
        f->claimed = true;
        return emit(f, selector, out, capacity, NULL, 0);
    case EXTGPU_SELECTOR_RELEASE_OWNER:
        CHECK(count == 1 && in[0] == f->generation && !f->frozen && registered_count(f) == 0);
        f->claimed = false;
        return emit(f, selector, out, capacity, NULL, 0);
    case EXTGPU_SELECTOR_CONFIG_READ: {
        CHECK(f->claimed && !f->frozen && count == 2);
        uint64_t reply = 0x687f1002;
        return emit(f, selector, out, capacity, &reply, 1);
    }
    case EXTGPU_SELECTOR_GET_BAR_INFO: {
        CHECK(f->claimed && !f->frozen && count == 1);
        uint64_t reply[3] = {2, 4096, 0};
        return emit(f, selector, out, capacity, reply, 3);
    }
    case EXTGPU_SELECTOR_REGISTER_DMA_REGION: {
        CHECK(count == 3 && f->claimed && !f->frozen);
        for (unsigned n = 0; n < 4; ++n) if (!f->regions[n].registered) {
            f->regions[n] = (region){.iova = in[0], .bytes = in[2], .registered = true};
            uint64_t reply[3] = {1, 0x100000, in[2]};
            return emit(f, selector, out, capacity, reply, 3);
        }
        return kIOReturnNoSpace;
    }
    case EXTGPU_SELECTOR_UNREGISTER_DMA_REGION:
        CHECK(count == 1 && !f->frozen);
        for (unsigned n = 0; n < 4; ++n) if (f->regions[n].registered && f->regions[n].iova == in[0]) {
            f->regions[n].registered = false;
            return emit(f, selector, out, capacity, NULL, 0);
        }
        return kIOReturnNotFound;
    case EXTGPU_SELECTOR_ALLOCATE_DMA_BUFFER: {
        CHECK(count == 2 && !f->frozen && !f->allocated);
        f->allocated = true;
        f->allocation_bytes = in[0];
        uint64_t reply[6] = {in[0], 0, 1, 0x100000, in[0], 0x030000};
        return emit(f, selector, out, capacity, reply, 6);
    }
    case EXTGPU_SELECTOR_FREE_DMA_BUFFER:
        CHECK(count == 0 && !f->frozen && f->allocated);
        f->allocated = false;
        return emit(f, selector, out, capacity, NULL, 0);
    case EXTGPU_SELECTOR_SETUP_INTERRUPTS: {
        CHECK(count == 0 && !f->frozen);
        uint64_t invalid_vectors = 0; // Retain local IRQ carrier without native async arm.
        return emit(f, selector, out, capacity, &invalid_vectors, 1);
    }
    case EXTGPU_SELECTOR_RETIRE_INTERRUPTS:
        CHECK(count == 0 && !f->frozen);
        return emit(f, selector, out, capacity, NULL, 0);
    case EXTGPU_SELECTOR_GET_RECOVERY_ENDPOINT: {
        CHECK(count == 1 && in[0] == f->generation && !f->frozen);
        uint64_t reply[3] = {f->broker_id, 1, 3};
        return emit(f, selector, out, capacity, reply, 3);
    }
    case EXTGPU_SELECTOR_PREPARE_RECOVERY_ESCROW: {
        CHECK(count == 7 && f->claimed && in[0] == f->generation);
        CHECK(in[1] == 5001 && in[2] == 9001 && (in[3] || in[4]) && (in[5] || in[6]));
        CHECK(memcmp(in + 3, in + 5, 2 * sizeof(uint64_t)));
        if (!f->prepared) {
            memcpy(f->key, in, sizeof(f->key)); memcpy(f->private_cap, in + 5, sizeof(f->private_cap));
            f->enrolled = registered_count(f); f->prepared = true;
        }
        CHECK(!memcmp(in, f->key, sizeof(f->key)) && !memcmp(in + 5, f->private_cap, sizeof(f->private_cap)));
        uint64_t reply[6]; memcpy(reply, f->key, sizeof(f->key)); reply[5] = f->broker_id;
        return emit(f, selector, out, capacity, reply, 6);
    }
    case EXTGPU_SELECTOR_COLD_BEGIN: {
        CHECK(count == 5 && f->claimed && f->prepared && f->broker_bound);
        CHECK(in[1] == 5001 && in[2] == 9001 && (in[3] || in[4]));
        if (!f->frozen) {
            f->frozen = true;
            memcpy(f->key, in, sizeof(f->key));
            f->enrolled = registered_count(f);
        }
        CHECK(!memcmp(in, f->key, sizeof(f->key)));
        uint64_t reply[7];
        memcpy(reply, f->key, sizeof(f->key));
        reply[5] = f->enrolled;
        reply[6] = EXTGPU_COLD_STATE_FROZEN;
        return emit(f, selector, out, capacity, reply, 7);
    }
    default:
        CHECK(!"unexpected GPU selector or ordinary RPC after terminal begin");
        return kIOReturnUnsupported;
    }
}
static int32_t broker_emit(fake_driver *f, uint32_t selector, uint64_t *out,
                            uint32_t *capacity, uint64_t *values, uint32_t count)
{
    if (f->broker_fail_selector == selector) return f->broker_failure;
    if (f->broker_malformed_selector != selector) return emit(f, selector, out, capacity, values, count);
    uint32_t previous = f->malformed_selector;
    int field = f->bad_field; bool short_reply = f->bad_count;
    f->malformed_selector = selector; f->bad_field = f->broker_bad_field; f->bad_count = f->broker_bad_count;
    int32_t status = emit(f, selector, out, capacity, values, count);
    f->malformed_selector = previous; f->bad_field = field; f->bad_count = short_reply;
    return status;
}
static int32_t broker_scalar(void *opaque, uint32_t selector, const uint64_t *in,
                             uint32_t count, uint64_t *out, uint32_t *capacity)
{
    fake_driver *f = opaque;
    CHECK(selector < 32 && f->broker_opens == 1); ++f->broker_calls[selector];
    if (f->broker_gone) return kIOReturnNoDevice;
    switch (selector) {
    case EXTGPU_SELECTOR_QUERY_PROTOCOL: {
        CHECK(count == 3 && in[0] == 1 && in[1] == 3 && in[2] == EXTGPU_PROTOCOL_FINGERPRINT);
        uint64_t reply[8] = {1, f->broker_minor, f->broker_epoch, f->broker_caps,
            EXTGPU_MAX_DMA_REGIONS, EXTGPU_MAX_PREPARED_DMA_BYTES, 1, EXTGPU_PROTOCOL_FINGERPRINT};
        return broker_emit(f, selector, out, capacity, reply, 8);
    }
    case EXTGPU_SELECTOR_BIND_RECOVERY_ESCROW: {
        CHECK(count == 7);
        if (!f->prepared) return kIOReturnNotFound;
        if (f->bind_wrong_owner) return kIOReturnNotPermitted;
        CHECK(!memcmp(in, f->key, sizeof(f->key)) && !memcmp(in + 5, f->private_cap, sizeof(f->private_cap)));
        f->broker_bound = true;
        uint64_t reply[7]; memcpy(reply, f->key, sizeof(f->key)); reply[5] = f->enrolled; reply[6] = EXTGPU_COLD_STATE_BOUND;
        return broker_emit(f, selector, out, capacity, reply, 7);
    }
    case EXTGPU_SELECTOR_COLD_BEGIN: {
        CHECK(count == 5 && f->prepared && f->broker_bound && !memcmp(in, f->key, sizeof(f->key)));
        if (!f->frozen) return kIOReturnNotReady;
        uint64_t reply[7]; memcpy(reply, f->key, sizeof(f->key)); reply[5] = f->enrolled; reply[6] = EXTGPU_COLD_STATE_FROZEN;
        return broker_emit(f, selector, out, capacity, reply, 7);
    }
    case EXTGPU_SELECTOR_COLD_RETIRE: {
        CHECK(count == 8 && f->frozen && !f->finished && in[7] == 3);
        CHECK(!memcmp(in, f->key, sizeof(f->key)));
        if (f->native_retire_failure) return f->native_retire_failure;
        for (unsigned n = 0; n < 4; ++n) if (f->regions[n].iova == in[5] && f->regions[n].bytes == in[6]) {
            region *r = &f->regions[n];
            if (!r->tombstone) {
                r->registered = false;
                r->tombstone = true;
                r->sequence = 100 + ++f->retire_commits;
            }
            uint64_t reply[10];
            memcpy(reply, f->key, sizeof(f->key));
            reply[5] = in[5]; reply[6] = in[6]; reply[7] = 0;
            reply[8] = EXTGPU_COLD_STATE_SUCCESS_TOMBSTONE; reply[9] = r->sequence;
            return broker_emit(f, selector, out, capacity, reply, 10);
        }
        return kIOReturnNotFound;
    }
    case EXTGPU_SELECTOR_COLD_FINISH: {
        CHECK(count == 6 && f->frozen && !registered_count(f) && in[5] == 3);
        CHECK(!memcmp(in, f->key, sizeof(f->key)));
        if (!f->finished) { f->finished = true; ++f->finish_commits; }
        f->claimed = false;
        uint64_t reply[7];
        memcpy(reply, f->key, sizeof(f->key));
        reply[5] = EXTGPU_COLD_STATE_CLOSED; reply[6] = 999;
        return broker_emit(f, selector, out, capacity, reply, 7);
    }
    default:
        CHECK(!"unexpected broker selector: no PCI/config/map/native fallback");
        return kIOReturnUnsupported;
    }
}
static int32_t map(void *opaque, uint32_t type, uint64_t *address, uint64_t *bytes, uint32_t options)
{
    fake_driver *f = opaque;
    (void)options;
    CHECK(!f->frozen);
    ++f->maps;
    if (f->fail_map) return kIOReturnUnsupported;
    CHECK(type == EXTGPU_MEMORY_DMA_BUFFER && f->allocated && f->allocation);
    *address = (uintptr_t)f->allocation;
    *bytes = f->allocation_bytes;
    return kIOReturnSuccess;
}
static int32_t unmap(void *opaque, uint32_t type, uint64_t address)
{
    fake_driver *f = opaque;
    CHECK(!f->frozen && type == EXTGPU_MEMORY_DMA_BUFFER && address == (uintptr_t)f->allocation);
    ++f->unmaps;
    return kIOReturnSuccess;
}
static int32_t close_connection(void *opaque)
{
    fake_driver *f = opaque;
    CHECK(!f->claimed && !registered_count(f) && !f->allocated);
    ++f->closes;
    return f->fail_close ? kIOReturnBusy : kIOReturnSuccess;
}
static const extgpu_test_transport transport = {scalar, map, unmap, close_connection};
static int32_t broker_lookup(void *opaque, uint64_t expected, uint64_t *actual, const char **class_name)
{
    fake_driver *f = opaque; ++f->broker_lookups; CHECK(expected == f->broker_id);
    *actual = f->broker_actual_id; *class_name = f->broker_class; return kIOReturnSuccess;
}
static int32_t broker_open(void *opaque)
{ fake_driver *f = opaque; CHECK(++f->broker_opens == 1); return kIOReturnSuccess; }
static int32_t broker_close(void *opaque)
{
    fake_driver *f = opaque; ++f->broker_closes;
    CHECK(!f->prepared || (f->finished && !registered_count(f)));
    return f->fail_close ? kIOReturnBusy : kIOReturnSuccess;
}
static int32_t drop_original(void *opaque)
{
    fake_driver *f = opaque; ++f->original_drops;
    CHECK(f->finished && !f->claimed && !registered_count(f));
    return f->drop_failure;
}
static const extgpu_test_transport broker_transport = {broker_scalar, NULL, NULL, broker_close};
static const extgpu_test_recovery_hooks recovery_hooks = {broker_lookup, broker_open, drop_original};

static void open_fixture(fixture *f, bool ids)
{
    CHECK(extgpu_test_open(&transport, &f->fake, &f->session, &f->device) == EXTGPU_OK);
    CHECK(extgpu_test_set_recovery_transport(f->device, &broker_transport, &recovery_hooks, &f->fake) == EXTGPU_OK);
    if (ids) CHECK(extgpu_test_set_registry_ids(f->device, 5001, 9001) == EXTGPU_OK);
    CHECK(extgpu_device_claim(f->device) == EXTGPU_OK);
}
static void close_fixture(fixture *f)
{
    CHECK(extgpu_device_close(&f->device) == EXTGPU_OK && !f->device);
    CHECK(extgpu_session_destroy(&f->session) == EXTGPU_OK && !f->session);
}
static extgpu_test_provider_snapshot absence(void)
{
    return (extgpu_test_provider_snapshot){.registry_id = 9001, .native_status = kIOReturnSuccess,
        .dictionary_created = 1, .iterator_present = 1, .iterator_valid_before = 1,
        .iterator_valid_after = 1, .entry_count = 0};
}
static void confirm(fixture *f)
{
    const extgpu_test_provider_snapshot snapshot = absence();
    CHECK(extgpu_test_set_provider_snapshot(f->device, &snapshot) == EXTGPU_OK);
    CHECK(extgpu_cold_removal_confirm(f->device, EXTGPU_COLD_PHYSICAL_POWER_OFF) == EXTGPU_OK);
}
static void finish_cold(fixture *f)
{
    confirm(f);
    CHECK(extgpu_cold_removal_finish(f->device) == EXTGPU_OK);
    CHECK(extgpu_cold_removal_finish(f->device) == EXTGPU_OK); // Local acknowledged idempotency.
    close_fixture(f);
    CHECK(f->fake.original_drops == 1 && f->fake.broker_closes == 1 && f->fake.closes == 0);
}
static void *page(void)
{
    void *result = NULL;
    const long bytes = sysconf(_SC_PAGESIZE);
    CHECK(bytes > 0 && !posix_memalign(&result, (size_t)bytes, (size_t)bytes));
    memset(result, 0x5a, (size_t)bytes);
    return result;
}
static extgpu_buffer *register_page(fixture *f, void *memory, uint64_t iova)
{
    extgpu_buffer *buffer = NULL;
    extgpu_dma_coverage coverage;
    CHECK(extgpu_dma_register(f->device, iova, memory, (uint64_t)sysconf(_SC_PAGESIZE),
                              &buffer, &coverage) == EXTGPU_OK && buffer);
    return buffer;
}
static void ordinary_operations_blocked(fixture *f, extgpu_buffer *buffer)
{
    unsigned calls[32]; memcpy(calls, f->fake.calls, sizeof(calls));
    const unsigned maps = f->fake.maps, unmaps = f->fake.unmaps;
    extgpu_protocol_info protocol;
    extgpu_pci_identity identity;
    extgpu_bar_info bar;
    extgpu_mapping *mapping = NULL;
    extgpu_buffer *extra = NULL;
    extgpu_irq *irq = NULL;
    extgpu_dma_coverage coverage;
    uint64_t word = 0;
    uint32_t value = 0;
    uint32_t vectors = 0;
    void *memory = page();
    CHECK(extgpu_device_protocol(f->device, &protocol) == EXTGPU_E_UNSAFE);
    CHECK(extgpu_device_identity(f->device, &identity) == EXTGPU_E_UNSAFE);
    CHECK(extgpu_device_claim(f->device) == EXTGPU_E_UNSAFE);
    CHECK(extgpu_device_release(f->device) == EXTGPU_E_UNSAFE);
    CHECK(extgpu_device_reset(f->device) == EXTGPU_E_UNSAFE);
    CHECK(extgpu_device_set_dma_address_bits(f->device, 32) == EXTGPU_E_UNSAFE);
    CHECK(extgpu_config_read(f->device, 0, 4, &value) == EXTGPU_E_UNSAFE);
    CHECK(extgpu_config_write(f->device, 4, 2, 0) == EXTGPU_E_UNSAFE);
    CHECK(extgpu_bar_get_info(f->device, 0, &bar) == EXTGPU_E_UNSAFE);
    CHECK(extgpu_bar_map(f->device, 0, &mapping) == EXTGPU_E_UNSAFE && !mapping);
    CHECK(extgpu_bar_map_with_cache_policy(f->device, 0, EXTGPU_BAR_CACHE_DEFAULT, &mapping) == EXTGPU_E_UNSAFE);
    CHECK(extgpu_mmio_read(f->device, 0, 0, 4, &word) == EXTGPU_E_UNSAFE);
    CHECK(extgpu_mmio_write(f->device, 0, 0, 4, 0) == EXTGPU_E_UNSAFE);
    CHECK(extgpu_dma_register(f->device, 0x800000, memory, (uint64_t)sysconf(_SC_PAGESIZE),
                              &extra, &coverage) == EXTGPU_E_UNSAFE && !extra);
    CHECK(extgpu_dma_allocate(f->device, (uint64_t)sysconf(_SC_PAGESIZE), 0,
                              &extra, &coverage) == EXTGPU_E_UNSAFE && !extra);
    CHECK(extgpu_irq_create(f->device, unexpected_irq, NULL, &irq, &vectors) == EXTGPU_E_UNSAFE && !irq && !vectors);
    if (buffer) {
        CHECK(extgpu_dma_probe(buffer, 0, &word) == EXTGPU_E_UNSAFE);
        CHECK(extgpu_buffer_address(buffer) == NULL);
    }
    CHECK(!memcmp(calls, f->fake.calls, sizeof(calls)) && maps == f->fake.maps && unmaps == f->fake.unmaps);
    CHECK(extgpu_device_close(&f->device) == EXTGPU_E_UNSAFE && f->device);
    free(memory);
}

static void old_minor_compatibility_and_capability_preflight(void)
{
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        fixture f = {.fake = fresh()};
        if (scenario == 0) f.fake.minor = 1;
        if (scenario == 1) f.fake.capabilities &= ~EXTGPU_CAP_COLD_REMOVAL;
        if (scenario == 3) f.fake.minor = 2;
        if (scenario == 4) f.fake.capabilities &= ~EXTGPU_CAP_RECOVERY_BROKER;
        open_fixture(&f, scenario != 2);
        extgpu_cold_removal_info info;
        const extgpu_status expected = scenario == 2 ? EXTGPU_E_UNSAFE : EXTGPU_E_UNSUPPORTED;
        CHECK(extgpu_cold_removal_begin(f.device, &info) == expected);
        CHECK(!f.fake.calls[EXTGPU_SELECTOR_COLD_BEGIN]);
        uint32_t identity = 0;
        CHECK(extgpu_config_read(f.device, 0, 4, &identity) == EXTGPU_OK && identity == 0x687f1002);
        CHECK(extgpu_device_release(f.device) == EXTGPU_OK);
        close_fixture(&f);
    }
}
static void begin_lost_reply_freezes_and_retries_exact_old_key(void)
{
    fixture f = {.fake = fresh()}; open_fixture(&f, true);
    void *memory = page(); extgpu_buffer *buffer = register_page(&f, memory, 0x400000);
    f.fake.fail_selector = EXTGPU_SELECTOR_COLD_BEGIN; f.fake.failure = kIOReturnError;
    extgpu_cold_removal_info info; memset(&info, 0xa5, sizeof(info));
    extgpu_cold_removal_info saved = info;
    CHECK(extgpu_cold_removal_begin(f.device, &info) == EXTGPU_E_UNSAFE);
    CHECK(!memcmp(&info, &saved, sizeof(info)) && f.fake.frozen && f.fake.enrolled == 1);
    ordinary_operations_blocked(&f, buffer);
    CHECK(extgpu_buffer_unregister(&buffer) == EXTGPU_E_UNSAFE && buffer);
    CHECK(!f.fake.calls[EXTGPU_SELECTOR_COLD_RETIRE] && !f.fake.broker_calls[EXTGPU_SELECTOR_COLD_RETIRE] &&
          !f.fake.calls[EXTGPU_SELECTOR_UNREGISTER_DMA_REGION]);
    CHECK(extgpu_cold_removal_confirm(f.device, 1) == EXTGPU_E_UNSAFE);
    CHECK(extgpu_test_set_registry_ids(f.device, 5002, 9002) == EXTGPU_E_UNSAFE);
    const unsigned queries = f.fake.calls[EXTGPU_SELECTOR_QUERY_PROTOCOL];
    ++f.fake.generation; // Never requery/rebind to a newly visible epoch.
    f.fake.fail_selector = UINT32_MAX;
    CHECK(extgpu_cold_removal_begin(f.device, &info) == EXTGPU_OK);
    CHECK(info.generation == 101 && info.driver_registry_id == 5001 && info.pci_registry_id == 9001);
    CHECK(info.enrolled_regions == 1 && info.nonce[0] == f.fake.key[3] && info.nonce[1] == f.fake.key[4]);
    CHECK(f.fake.calls[EXTGPU_SELECTOR_QUERY_PROTOCOL] == queries);
    confirm(&f);
    CHECK(extgpu_cold_removal_finish(f.device) == EXTGPU_E_BUSY);
    CHECK(extgpu_buffer_unregister(&buffer) == EXTGPU_OK && !buffer);
    free(memory);
    finish_cold(&f);
}
static void begin_malformed_replies_retain_terminal_session(void)
{
    for (int field = -1; field < 7; ++field) {
        fixture f = {.fake = fresh()}; open_fixture(&f, true);
        f.fake.malformed_selector = EXTGPU_SELECTOR_COLD_BEGIN;
        f.fake.bad_field = field; f.fake.bad_count = field == -1;
        extgpu_cold_removal_info info;
        CHECK(extgpu_cold_removal_begin(f.device, &info) == EXTGPU_E_UNSAFE);
        ordinary_operations_blocked(&f, NULL);
        CHECK(extgpu_cold_removal_finish(f.device) == EXTGPU_E_UNSAFE);
        f.fake.malformed_selector = UINT32_MAX;
        CHECK(extgpu_cold_removal_begin(f.device, &info) == EXTGPU_OK);
        finish_cold(&f);
    }
}
static void provider_absence_requires_exact_old_identity_and_valid_empty_query(void)
{
    fixture f = {.fake = fresh()}; open_fixture(&f, true);
    extgpu_cold_removal_info info;
    CHECK(extgpu_cold_removal_confirm(f.device, 1) == EXTGPU_E_UNSAFE);
    CHECK(extgpu_cold_removal_begin(f.device, &info) == EXTGPU_OK);
    CHECK(extgpu_cold_removal_confirm(f.device, 1) == EXTGPU_E_UNSAFE); // Unseeded tests cannot query native registry.
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        extgpu_test_provider_snapshot snapshot = absence();
        switch (scenario) {
        case 0: snapshot.entry_count = 1; break; // DEXT gone, old PCI still present.
        case 1: snapshot.native_status = kIOReturnNotFound; break; // Failure != empty successful query.
        case 2: snapshot.dictionary_created = 0; break;
        case 3: snapshot.iterator_present = 0; break;
        case 4: snapshot.iterator_valid_before = 0; break;
        case 5: snapshot.iterator_valid_after = 0; break;
        case 6: snapshot.registry_id = 9002; break; // Replacement/BDF reuse proves nothing about 9001.
        case 7: snapshot.registry_id = 5001; break; // Driver endpoint identity is not PCI identity.
        }
        CHECK(extgpu_test_set_provider_snapshot(f.device, &snapshot) == EXTGPU_OK);
        CHECK(extgpu_cold_removal_confirm(f.device, 1) == EXTGPU_E_UNSAFE);
        CHECK(extgpu_cold_removal_finish(f.device) == EXTGPU_E_UNSAFE);
        CHECK(f.fake.broker_calls[EXTGPU_SELECTOR_COLD_FINISH] == 0);
    }
    for (uint32_t flags = 0; flags < 8; ++flags) if (flags != 1)
        CHECK(extgpu_cold_removal_confirm(f.device, flags) == EXTGPU_E_ARGUMENT);
    CHECK(extgpu_cold_removal_confirm(f.device, UINT32_MAX) == EXTGPU_E_ARGUMENT);
    finish_cold(&f);
}
static void successful_null_iterator_is_empty_but_errors_and_contradictions_retain(void)
{
    fixture f = {.fake = fresh()}; open_fixture(&f, true);
    void *memory = page(); extgpu_buffer *buffer = register_page(&f, memory, 0x400000);
    extgpu_cold_removal_info info;
    CHECK(extgpu_cold_removal_begin(f.device, &info) == EXTGPU_OK);
    const extgpu_test_provider_snapshot empty = {.registry_id = 9001, .native_status = kIOReturnSuccess,
        .dictionary_created = 1, .iterator_present = 0, .iterator_valid_before = 0,
        .iterator_valid_after = 0, .entry_count = 0};
    for (unsigned scenario = 0; scenario < 7; ++scenario) {
        extgpu_test_provider_snapshot bad = empty;
        switch (scenario) {
        case 0: bad.iterator_valid_before = 1; break;
        case 1: bad.iterator_valid_after = 1; break;
        case 2: bad.iterator_valid_before = bad.iterator_valid_after = 1; break;
        case 3: bad.native_status = kIOReturnNotFound; break;
        case 4: bad.registry_id = 9002; break;
        case 5: bad.entry_count = 1; break;
        case 6: bad.dictionary_created = 0; break;
        }
        CHECK(extgpu_test_set_provider_snapshot(f.device, &bad) == EXTGPU_OK);
        CHECK(extgpu_cold_removal_confirm(f.device, 1) == EXTGPU_E_UNSAFE);
        CHECK(extgpu_buffer_unregister(&buffer) == EXTGPU_E_UNSAFE && buffer);
        CHECK(!f.fake.broker_calls[EXTGPU_SELECTOR_COLD_RETIRE]);
    }
    CHECK(extgpu_test_set_provider_snapshot(f.device, &empty) == EXTGPU_OK);
    CHECK(extgpu_cold_removal_confirm(f.device, 1) == EXTGPU_OK);
    CHECK(extgpu_buffer_unregister(&buffer) == EXTGPU_OK && !buffer);
    free(memory);
    CHECK(extgpu_cold_removal_finish(f.device) == EXTGPU_OK);
    close_fixture(&f);
}
static void retire_malformed_or_lost_ack_preserves_backing_and_retries_tombstone(void)
{
    for (int field = -2; field < 10; ++field) {
        fixture f = {.fake = fresh()}; open_fixture(&f, true);
        void *memory = page(); extgpu_buffer *buffer = register_page(&f, memory, 0x400000);
        extgpu_cold_removal_info info;
        CHECK(extgpu_cold_removal_begin(f.device, &info) == EXTGPU_OK); confirm(&f);
        if (field == -2) { f.fake.fail_selector = EXTGPU_SELECTOR_COLD_RETIRE; f.fake.failure = kIOReturnError; }
        else { f.fake.malformed_selector = EXTGPU_SELECTOR_COLD_RETIRE; f.fake.bad_count = field == -1; f.fake.bad_field = field; }
        CHECK(extgpu_buffer_unregister(&buffer) == EXTGPU_E_UNSAFE && buffer);
        CHECK(extgpu_buffer_size(buffer) == (uint64_t)sysconf(_SC_PAGESIZE));
        CHECK(((unsigned char *)memory)[0] == 0x5a && f.fake.retire_commits == 1);
        CHECK(extgpu_cold_removal_finish(f.device) == EXTGPU_E_BUSY && !f.fake.broker_calls[EXTGPU_SELECTOR_COLD_FINISH]);
        ordinary_operations_blocked(&f, buffer);
        f.fake.fail_selector = f.fake.malformed_selector = UINT32_MAX;
        CHECK(extgpu_buffer_unregister(&buffer) == EXTGPU_OK && !buffer && f.fake.retire_commits == 1);
        CHECK(f.fake.calls[EXTGPU_SELECTOR_UNREGISTER_DMA_REGION] == 0 && f.fake.unmaps == 0);
        free(memory); finish_cold(&f);
    }
}
static void finish_malformed_lost_reply_and_close_failure_remain_retryable(void)
{
    for (int field = -2; field < 7; ++field) {
        fixture f = {.fake = fresh()}; open_fixture(&f, true);
        extgpu_cold_removal_info info;
        CHECK(extgpu_cold_removal_begin(f.device, &info) == EXTGPU_OK); confirm(&f);
        if (field == -2) { f.fake.fail_selector = EXTGPU_SELECTOR_COLD_FINISH; f.fake.failure = kIOReturnNoDevice; }
        else { f.fake.malformed_selector = EXTGPU_SELECTOR_COLD_FINISH; f.fake.bad_count = field == -1; f.fake.bad_field = field; }
        CHECK(extgpu_cold_removal_finish(f.device) == EXTGPU_E_UNSAFE);
        CHECK(extgpu_device_close(&f.device) == EXTGPU_E_UNSAFE && f.device);
        ordinary_operations_blocked(&f, NULL);
        f.fake.fail_selector = f.fake.malformed_selector = UINT32_MAX;
        CHECK(extgpu_cold_removal_finish(f.device) == EXTGPU_OK && f.fake.finish_commits == 1);
        CHECK(extgpu_device_claim(f.device) == EXTGPU_E_UNSAFE && !f.fake.calls[EXTGPU_SELECTOR_RELEASE_OWNER]);
        f.fake.fail_close = true;
        CHECK(extgpu_device_close(&f.device) == EXTGPU_E_BUSY && f.device);
        f.fake.fail_close = false;
        close_fixture(&f);
    }
}
static void unused_claim_and_resource_preconditions_do_not_freeze(void)
{
    fixture f = {.fake = fresh()}; open_fixture(&f, true);
    extgpu_mapping *mapping = NULL; extgpu_cold_removal_info info;
    f.fake.fail_map = true;
    CHECK(extgpu_bar_map(f.device, 0, &mapping) == EXTGPU_E_UNSUPPORTED && !mapping);
    CHECK(extgpu_cold_removal_begin(f.device, &info) == EXTGPU_E_BUSY); // Export attempt history persists.
    CHECK(!f.fake.calls[EXTGPU_SELECTOR_COLD_BEGIN]);
    CHECK(extgpu_device_release(f.device) == EXTGPU_OK); close_fixture(&f);

    f = (fixture){.fake = fresh()}; open_fixture(&f, true);
    f.fake.allocation = page(); extgpu_buffer *allocation = NULL; extgpu_dma_coverage coverage;
    CHECK(extgpu_dma_allocate(f.device, (uint64_t)sysconf(_SC_PAGESIZE), 0, &allocation, &coverage) == EXTGPU_OK);
    CHECK(extgpu_cold_removal_begin(f.device, &info) == EXTGPU_E_BUSY && !f.fake.calls[EXTGPU_SELECTOR_COLD_BEGIN]);
    CHECK(extgpu_buffer_unregister(&allocation) == EXTGPU_OK);
    free(f.fake.allocation);
    CHECK(extgpu_device_release(f.device) == EXTGPU_OK); close_fixture(&f);

    f = (fixture){.fake = fresh()}; open_fixture(&f, true);
    extgpu_irq *irq = NULL; uint32_t vectors = 0;
    CHECK(extgpu_irq_create(f.device, unexpected_irq, NULL, &irq, &vectors) == EXTGPU_E_UNSAFE && irq);
    CHECK(extgpu_cold_removal_begin(f.device, &info) == EXTGPU_E_BUSY && !f.fake.calls[EXTGPU_SELECTOR_COLD_BEGIN]);
    CHECK(extgpu_irq_close(&irq) == EXTGPU_OK && !irq);
    CHECK(extgpu_device_release(f.device) == EXTGPU_OK); close_fixture(&f);
}
static void uncertain_or_missing_claim_is_not_admitted(void)
{
    for (unsigned scenario = 0; scenario < 2; ++scenario) {
        fixture f = {.fake = fresh()};
        CHECK(extgpu_test_open(&transport, &f.fake, &f.session, &f.device) == EXTGPU_OK);
        CHECK(extgpu_test_set_registry_ids(f.device, 5001, 9001) == EXTGPU_OK);
        if (scenario) {
            f.fake.fail_selector = EXTGPU_SELECTOR_CLAIM; f.fake.failure = kIOReturnError;
            CHECK(extgpu_device_claim(f.device) == EXTGPU_E_UNSAFE);
        }
        extgpu_cold_removal_info info;
        CHECK(extgpu_cold_removal_begin(f.device, &info) == (scenario ? EXTGPU_E_UNSAFE : EXTGPU_E_NOT_CLAIMED));
        CHECK(!f.fake.calls[EXTGPU_SELECTOR_COLD_BEGIN]);
        f.fake.fail_selector = UINT32_MAX;
        if (scenario) CHECK(extgpu_device_release(f.device) == EXTGPU_OK);
        close_fixture(&f);
    }
}
static void multiple_registered_regions_retire_only_after_confirmation(void)
{
    fixture f = {.fake = fresh()}; open_fixture(&f, true);
    void *first = page(), *second = page();
    extgpu_buffer *a = register_page(&f, first, 0x400000), *b = register_page(&f, second, 0x800000);
    extgpu_cold_removal_info info;
    CHECK(extgpu_cold_removal_begin(f.device, &info) == EXTGPU_OK && info.enrolled_regions == 2);
    CHECK(extgpu_buffer_unregister(&a) == EXTGPU_E_UNSAFE && a);
    CHECK(extgpu_buffer_unregister(&b) == EXTGPU_E_UNSAFE && b);
    CHECK(!f.fake.broker_calls[EXTGPU_SELECTOR_COLD_RETIRE]);
    confirm(&f);
    CHECK(extgpu_buffer_unregister(&a) == EXTGPU_OK && !a);
    CHECK(extgpu_cold_removal_finish(f.device) == EXTGPU_E_BUSY);
    CHECK(extgpu_buffer_unregister(&b) == EXTGPU_OK && !b);
    CHECK(f.fake.retire_commits == 2 && f.fake.calls[EXTGPU_SELECTOR_UNREGISTER_DMA_REGION] == 0);
    free(first); free(second); finish_cold(&f);
}
static void broker_exact_identity_class_protocol_and_unseeded_preflight(void)
{
    for (unsigned scenario = 0; scenario < 7; ++scenario) {
        fixture f = {.fake = fresh()}; open_fixture(&f, true);
        if (scenario == 0) f.fake.broker_actual_id++;
        if (scenario == 1) f.fake.broker_class = "VFIOUserPCIDriver";
        if (scenario == 2) f.fake.broker_caps |= EXTGPU_CAP_CONFIG_WRITE;
        if (scenario == 3) f.fake.broker_epoch++;
        if (scenario == 4) f.fake.broker_minor = 2;
        if (scenario == 5) { f.fake.malformed_selector = EXTGPU_SELECTOR_GET_RECOVERY_ENDPOINT; f.fake.bad_count = true; }
        if (scenario == 6) f.fake.broker_id = 5001;
        extgpu_cold_removal_info info;
        CHECK(extgpu_cold_removal_begin(f.device, &info) != EXTGPU_OK);
        CHECK(!f.fake.calls[EXTGPU_SELECTOR_PREPARE_RECOVERY_ESCROW] && !f.fake.prepared);
        if (scenario < 2 || scenario >= 5) CHECK(f.fake.broker_opens == 0);
        uint32_t value = 0;
        CHECK(extgpu_config_read(f.device, 0, 4, &value) == EXTGPU_OK); // Preflight never armed.
        f.fake.malformed_selector = UINT32_MAX;
        CHECK(extgpu_device_release(f.device) == EXTGPU_OK); close_fixture(&f);
    }
    fixture f = {.fake = fresh()};
    CHECK(extgpu_test_open(&transport, &f.fake, &f.session, &f.device) == EXTGPU_OK);
    CHECK(extgpu_test_set_registry_ids(f.device, 5001, 9001) == EXTGPU_OK);
    CHECK(extgpu_device_claim(f.device) == EXTGPU_OK);
    extgpu_cold_removal_info info;
    CHECK(extgpu_cold_removal_begin(f.device, &info) == EXTGPU_E_UNSUPPORTED); // No native lookup fallback.
    CHECK(!f.fake.prepared && !f.fake.broker_opens);
    CHECK(extgpu_device_release(f.device) == EXTGPU_OK); close_fixture(&f);
}
static void lost_prepare_or_bind_ack_uses_same_private_cap_and_broker_instance(void)
{
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        fixture f = {.fake = fresh()}; open_fixture(&f, true);
        void *memory = page(); extgpu_buffer *buffer = register_page(&f, memory, 0x400000);
        if (scenario == 0) f.fake.fail_before_selector = EXTGPU_SELECTOR_PREPARE_RECOVERY_ESCROW;
        if (scenario == 1) { f.fake.fail_selector = EXTGPU_SELECTOR_PREPARE_RECOVERY_ESCROW; f.fake.failure = kIOReturnError; }
        if (scenario == 2) { f.fake.broker_fail_selector = EXTGPU_SELECTOR_BIND_RECOVERY_ESCROW; f.fake.broker_failure = kIOReturnError; }
        if (scenario == 3) f.fake.bind_wrong_owner = true;
        extgpu_cold_removal_info info;
        CHECK(extgpu_cold_removal_begin(f.device, &info) == EXTGPU_E_UNSAFE);
        CHECK(!f.fake.frozen && !f.fake.calls[EXTGPU_SELECTOR_COLD_BEGIN]);
        CHECK(extgpu_buffer_unregister(&buffer) == EXTGPU_E_UNSAFE && buffer);
        ordinary_operations_blocked(&f, buffer);
        f.fake.fail_before_selector = f.fake.fail_selector = f.fake.broker_fail_selector = UINT32_MAX;
        f.fake.bind_wrong_owner = false;
        CHECK(extgpu_cold_removal_begin(f.device, &info) == EXTGPU_OK);
        CHECK(f.fake.broker_opens == 1 && f.fake.calls[EXTGPU_SELECTOR_GET_RECOVERY_ENDPOINT] == 1);
        CHECK(f.fake.calls[EXTGPU_SELECTOR_PREPARE_RECOVERY_ESCROW] == (scenario == 0 ? 2u : 1u));
        CHECK(info.nonce[0] == f.fake.key[3] && info.nonce[1] == f.fake.key[4]);
        CHECK(memcmp(info.nonce, f.fake.private_cap, sizeof(info.nonce))); // Private capability is never public info.
        confirm(&f); CHECK(extgpu_buffer_unregister(&buffer) == EXTGPU_OK && !buffer);
        free(memory); finish_cold(&f);
    }
}
static void malformed_prepare_bind_and_snapshot_acks_retain_until_exact_retry(void)
{
    const uint32_t stages[] = {EXTGPU_SELECTOR_PREPARE_RECOVERY_ESCROW,
        EXTGPU_SELECTOR_BIND_RECOVERY_ESCROW, EXTGPU_SELECTOR_COLD_BEGIN};
    for (unsigned stage = 0; stage < 3; ++stage) {
        const int fields = stage == 0 ? 6 : 7;
        for (int field = -1; field < fields; ++field) {
            fixture f = {.fake = fresh()}; open_fixture(&f, true);
            if (stage == 0) { f.fake.malformed_selector = stages[stage]; f.fake.bad_field = field; f.fake.bad_count = field < 0; }
            else { f.fake.broker_malformed_selector = stages[stage]; f.fake.broker_bad_field = field; f.fake.broker_bad_count = field < 0; }
            extgpu_cold_removal_info info;
            CHECK(extgpu_cold_removal_begin(f.device, &info) == EXTGPU_E_UNSAFE);
            CHECK(f.fake.original_drops == 0 && f.fake.broker_closes == 0);
            unsigned gpu_begins = f.fake.calls[EXTGPU_SELECTOR_COLD_BEGIN];
            f.fake.malformed_selector = f.fake.broker_malformed_selector = UINT32_MAX;
            if (stage == 2) f.fake.original_gone = true;
            CHECK(extgpu_cold_removal_begin(f.device, &info) == EXTGPU_OK);
            if (stage == 2) CHECK(f.fake.calls[EXTGPU_SELECTOR_COLD_BEGIN] == gpu_begins);
            CHECK(f.fake.broker_opens == 1); finish_cold(&f);
        }
    }
}
static void revoked_original_port_never_receives_retire_finish_or_close_rpc(void)
{
    fixture f = {.fake = fresh()}; open_fixture(&f, true);
    void *memory = page(); extgpu_buffer *buffer = register_page(&f, memory, 0x400000);
    f.fake.fail_selector = EXTGPU_SELECTOR_COLD_BEGIN; f.fake.failure = kIOReturnNoDevice;
    extgpu_cold_removal_info info;
    CHECK(extgpu_cold_removal_begin(f.device, &info) == EXTGPU_E_UNSAFE && f.fake.frozen);
    unsigned gpu_calls[32]; memcpy(gpu_calls, f.fake.calls, sizeof(gpu_calls));
    f.fake.fail_selector = UINT32_MAX; f.fake.original_gone = true; ++f.fake.generation;
    CHECK(extgpu_cold_removal_begin(f.device, &info) == EXTGPU_OK && info.generation == 101);
    confirm(&f);
    CHECK(extgpu_buffer_unregister(&buffer) == EXTGPU_OK && !buffer);
    free(memory); finish_cold(&f);
    CHECK(!memcmp(gpu_calls, f.fake.calls, sizeof(gpu_calls)) && f.fake.closes == 0);
    CHECK(f.fake.original_drops == 1 && f.fake.broker_closes == 1);
}
static void broker_errors_partial_native_failure_and_local_drop_failures_retain(void)
{
    fixture f = {.fake = fresh()}; open_fixture(&f, true);
    void *first = page(), *second = page();
    extgpu_buffer *a = register_page(&f, first, 0x400000), *b = register_page(&f, second, 0x800000);
    extgpu_cold_removal_info info;
    CHECK(extgpu_cold_removal_begin(f.device, &info) == EXTGPU_OK); confirm(&f);
    f.fake.original_gone = true;
    CHECK(extgpu_buffer_unregister(&a) == EXTGPU_OK && !a && f.fake.retire_commits == 1);
    for (unsigned failure = 0; failure < 3; ++failure) {
        f.fake.broker_gone = failure == 0;
        f.fake.native_retire_failure = failure == 1 ? kIOReturnNotReady : failure == 2 ? kIOReturnError : 0;
        CHECK(extgpu_buffer_unregister(&b) == EXTGPU_E_UNSAFE && b);
        CHECK(f.fake.retire_commits == 1 && registered_count(&f.fake) == 1);
        CHECK(!f.fake.calls[EXTGPU_SELECTOR_COLD_RETIRE] && !f.fake.calls[EXTGPU_SELECTOR_COLD_FINISH]);
        CHECK(extgpu_cold_removal_finish(f.device) == EXTGPU_E_BUSY);
    }
    f.fake.broker_gone = false; f.fake.native_retire_failure = 0;
    CHECK(extgpu_buffer_unregister(&b) == EXTGPU_OK && !b && f.fake.retire_commits == 2);
    free(first); free(second);
    CHECK(extgpu_cold_removal_finish(f.device) == EXTGPU_OK);
    f.fake.drop_failure = KERN_FAILURE;
    CHECK(extgpu_device_close(&f.device) == EXTGPU_E_IO && f.device && f.fake.broker_closes == 0);
    f.fake.drop_failure = KERN_SUCCESS;
    close_fixture(&f);
    CHECK(f.fake.original_drops == 2 && f.fake.broker_closes == 1 && f.fake.broker_opens == 1);
}

int main(void)
{
    old_minor_compatibility_and_capability_preflight();
    begin_lost_reply_freezes_and_retries_exact_old_key();
    begin_malformed_replies_retain_terminal_session();
    provider_absence_requires_exact_old_identity_and_valid_empty_query();
    successful_null_iterator_is_empty_but_errors_and_contradictions_retain();
    retire_malformed_or_lost_ack_preserves_backing_and_retries_tombstone();
    finish_malformed_lost_reply_and_close_failure_remain_retryable();
    unused_claim_and_resource_preconditions_do_not_freeze();
    uncertain_or_missing_claim_is_not_admitted();
    multiple_registered_regions_retire_only_after_confirmation();
    broker_exact_identity_class_protocol_and_unseeded_preflight();
    lost_prepare_or_bind_ack_uses_same_private_cap_and_broker_instance();
    malformed_prepare_bind_and_snapshot_acks_retain_until_exact_retry();
    revoked_original_port_never_receives_retire_finish_or_close_rpc();
    broker_errors_partial_native_failure_and_local_drop_failures_retain();
    puts("PASS actual SDK broker cold-removal: v1.1 ordinary compatibility, exact private endpoint/escrow binding, lost ACK reconciliation, original-port revocation, retained backing and local IPC cleanup; no hardware");
    return 0;
}
