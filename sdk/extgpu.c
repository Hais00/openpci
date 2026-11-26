/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Original host SDK implementation. The wire ABI is described separately in
 * extgpu_driver_abi.h. No QEMU or broker participates in this data path. */
#include <extgpu/extgpu.h>
#include <extgpu/extgpu_driver_abi.h>
#include <extgpu/private_mmio_window.h>
#include <extgpu/private_mmio_ring.h>
#include <extgpu/private_irq_mailbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/OSMessageNotification.h>
#include <dispatch/dispatch.h>
#include <inttypes.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef EXTGPU_TESTING
#include "extgpu_test.h"
#endif

struct extgpu_session { pthread_mutex_t lock; size_t devices; };
struct mmio_worker {
    dispatch_semaphore_t wake;
    pthread_t thread;
    io_connect_t connection;
    struct openpci_mmio_window *window;
    uint64_t generation, pending;
    bool stop;
};
struct extgpu_device {
    pthread_mutex_t lock;
    extgpu_session *session;
    io_connect_t connection;
    uint64_t registry_id, pci_registry_id;
    extgpu_protocol_info protocol;
    extgpu_error_info error;
    bool claimed, protocol_valid, ownership_uncertain;
    struct openpci_mmio_window *mmio_window;
    dispatch_queue_t mmio_queue;
    struct mmio_worker mmio_worker;
    uint64_t mmio_generation;
    bool mmio_window_attempted, mmio_window_failed, mmio_window_word_writes;
    struct openpci_mmio_ring *mmio_ring;
    uint64_t mmio_ring_sequence, mmio_ring_kicks, mmio_ring_batches, mmio_ring_entries;
    bool mmio_ring_attempted, mmio_ring_failed;
    uint64_t mmio_ring_poll_us, mmio_ring_flags;
    uint64_t mmio_grouped_requests, mmio_grouped_writes;
    bool mmio_ring_active_publish;
    uint64_t mmio_last_completion_ns;
    uint64_t mmio_idle_scalar;
    uint64_t mmio_window_cold, mmio_window_warm, mmio_window_fallbacks;
    uint64_t mmio_window_word_replies, mmio_window_word_fallbacks;
    bool original_closed; /* IOServiceClose consumed its local reference, even on RPC error. */
    bool warm_vm_pending;
    uint32_t warm_vm_selector;
    uint64_t warm_group_scope[3];
    uint32_t pinned_scope_selector;
    uint64_t pinned_scope_generation, pinned_scope[3];
    uint32_t bar_mask;
    bool direct_bar_challenge_issued;
    bool direct_bar_authorized;
    bool direct_bar_retired;
    uint64_t direct_bar_challenge[3];
    uint32_t bar_cache_attempted_mask, bar_cache_options[6];
    size_t buffers;
    extgpu_buffer *buffer_list;
    const extgpu_buffer *vm_dynamic_window;
    uint64_t dma_bytes;
    bool allocated_buffer;
    bool dma_budget_uncertain;
    uint32_t dma_address_bits;
    extgpu_irq *irq;
    bool shared_irq_attempted;
    bool cold_attempted, cold_acknowledged, cold_confirmed, cold_finished;
    bool surprise_requested, surprise_detaching, surprise_released, surprise_escrow_closed;
    bool surprise_draining_irq;
    uint64_t surprise_bridge;
    uint64_t cold_key[EXTGPU_COLD_KEY_WORDS], cold_enrolled_regions;
    io_connect_t broker_connection;
    uint64_t broker_registry_id, cold_private_cap[EXTGPU_RECOVERY_PRIVATE_CAP_WORDS];
    bool broker_opened, broker_ready, broker_bound, cold_prepared;
    bool cold_gpu_begin_sent, cold_gpu_begin_acknowledged, original_local_dropped;
#ifdef EXTGPU_TESTING
    const extgpu_test_transport *test;
    void *test_context;
    bool test_provider_seeded;
    extgpu_test_provider_snapshot test_provider, test_bridge_provider;
    uint64_t test_reset_bridge, test_reset_audio;
    const extgpu_test_transport *test_broker;
    const extgpu_test_recovery_hooks *test_recovery;
    void *test_broker_context;
#endif
};
struct extgpu_mapping {
    extgpu_device *device;
    uint64_t generation, address, size;
    uint32_t bar;
};
struct extgpu_buffer {
    extgpu_device *device;
    extgpu_dma_coverage coverage;
    void *address;
    uint64_t mapped_size;
    bool allocated, unsafe, retired, vm_window, vm_segmented, vm_dynamic;
    extgpu_buffer *next;
    extgpu_vm_dma_segment vm_segments[];
};
struct extgpu_irq {
    extgpu_device *device;
    pthread_mutex_t lock;
    uint64_t generation;
    uint32_t vectors;
    bool armed, stopping, driver_retirement_started;
    bool shared_delivery;
    uint64_t mailbox_address;
    struct openpci_irq_mailbox *mailbox;
    extgpu_irq_callback callback;
    void *context;
    IONotificationPortRef port;
    dispatch_queue_t queue;
    dispatch_source_t source;
    dispatch_semaphore_t cancelled;
};
static const char irq_queue_key;

static extgpu_status native_status(kern_return_t kr)
{
    if (kr == KERN_SUCCESS) return EXTGPU_OK;
    if (kr == kIOReturnUnsupported) return EXTGPU_E_UNSUPPORTED;
    if (kr == kIOReturnBusy || kr == kIOReturnExclusiveAccess || kr == kIOReturnStillOpen) return EXTGPU_E_BUSY;
    if (kr == kIOReturnNoDevice || kr == kIOReturnNotFound) return EXTGPU_E_NOT_FOUND;
    if (kr == kIOReturnBadArgument) return EXTGPU_E_ARGUMENT;
    if (kr == kIOReturnNoMemory) return EXTGPU_E_MEMORY;
    if (kr == kIOReturnNotOpen) return EXTGPU_E_NOT_CLAIMED;
    return EXTGPU_E_IO;
}
static extgpu_status error(extgpu_device *d, extgpu_status status, kern_return_t native, const char *operation)
{
    d->error.status = status;
    d->error.native_code = native;
    size_t length = strnlen(operation, sizeof(d->error.operation) - 1);
    memcpy(d->error.operation, operation, length);
    d->error.operation[length] = '\0';
    return status;
}
static kern_return_t scalar(extgpu_device *d, uint32_t selector, const uint64_t *in,
                            uint32_t count, uint64_t *out, uint32_t *out_count)
{
#ifdef EXTGPU_TESTING
    if (d->test) return d->test->scalar(d->test_context, selector, in, count, out, out_count);
#endif
    return IOConnectCallScalarMethod(d->connection, selector, in, count, out, out_count);
}
static kern_return_t map_memory(extgpu_device *d, uint32_t type, uint64_t *address,
                                uint64_t *size, uint32_t options)
{
#ifdef EXTGPU_TESTING
    if (d->test) return d->test->map(d->test_context, type, address, size, options);
#endif
    mach_vm_address_t a = 0;
    mach_vm_size_t n = 0;
    kern_return_t kr = IOConnectMapMemory64(d->connection, type, mach_task_self(), &a, &n, options);
    *address = a; *size = n;
    return kr;
}
static kern_return_t unmap_memory(extgpu_device *d, uint32_t type, uint64_t address)
{
#ifdef EXTGPU_TESTING
    if (d->test) return d->test->unmap(d->test_context, type, address);
#endif
    return IOConnectUnmapMemory64(d->connection, type, mach_task_self(), address);
}
static extgpu_status confirm_registry_absent(extgpu_device *d, uint64_t id);

static kern_return_t task_region(mach_vm_address_t *address, mach_vm_size_t *size,
                                 struct vm_region_submap_info_64 *info)
{
    natural_t depth = 0;
    for (;;) {
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        const kern_return_t kr = mach_vm_region_recurse(mach_task_self(), address, size,
            &depth, (vm_region_recurse_info_t)info, &count);
        if (kr != KERN_SUCCESS || !info->is_submap) return kr;
        ++depth;
    }
}

/* The old PCI endpoint can be gone while XNU still reserves the same task
 * address for its IOMemoryMap. Replace that exact IOKit range with an
 * anonymous guard in one Mach VM operation. Keep the guard in place so a
 * later IOMemoryMap destructor cannot unmap an unrelated address reuse. */
static kern_return_t unmap_detached_memory(extgpu_device *d, uint32_t type, uint64_t base, uint64_t bytes)
{
    const kern_return_t native = unmap_memory(d, type, base);
    if (native == KERN_SUCCESS || !base || !bytes || bytes > UINT64_MAX - base) return native;

    mach_vm_address_t address = base;
    mach_vm_size_t size = 0;
    struct vm_region_submap_info_64 info = {0};
    kern_return_t kr = task_region(&address, &size, &info);
    if (kr == KERN_INVALID_ADDRESS || (kr == KERN_SUCCESS && address >= base + bytes))
        return KERN_SUCCESS;
    if (kr != KERN_SUCCESS || address != base || size != bytes) return native;

    const unsigned guard_tag = VM_MEMORY_APPLICATION_SPECIFIC_16;
    if (info.user_tag == guard_tag && info.protection == VM_PROT_NONE &&
        info.max_protection == VM_PROT_NONE) return KERN_SUCCESS;
    const bool bar = type >= EXTGPU_MEMORY_BAR0 && type <= EXTGPU_MEMORY_BAR5;
    if (info.user_tag != VM_MEMORY_IOKIT || (bar && info.share_mode != SM_EMPTY) ||
        base % vm_page_size || bytes % vm_page_size || !d->surprise_bridge ||
        confirm_registry_absent(d, d->pci_registry_id) != EXTGPU_OK ||
        confirm_registry_absent(d, d->surprise_bridge) != EXTGPU_OK) return native;

    kr = mach_vm_map(mach_task_self(), &address, bytes, 0,
        VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE | VM_MAKE_TAG(guard_tag),
        MEMORY_OBJECT_NULL, 0, false, VM_PROT_NONE, VM_PROT_NONE, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS) return kr;
    address = base;
    kr = task_region(&address, &size, &info);
    if (kr != KERN_SUCCESS) return kr;
    return address == base && size == bytes && info.user_tag == guard_tag &&
        info.protection == VM_PROT_NONE && info.max_protection == VM_PROT_NONE ?
        KERN_SUCCESS : KERN_PROTECTION_FAILURE;
}

static kern_return_t close_connection(extgpu_device *d)
{
#ifdef EXTGPU_TESTING
    if (d->test) return d->test->close(d->test_context);
#endif
    return IOServiceClose(d->connection);
}
static kern_return_t close_broker_connection(extgpu_device *d)
{
#ifdef EXTGPU_TESTING
    if (d->test) return d->test_broker->close(d->test_broker_context);
#endif
    return IOServiceClose(d->broker_connection);
}
static kern_return_t drop_original_local(extgpu_device *d)
{
#ifdef EXTGPU_TESTING
    if (d->test) return d->test_recovery->drop_original_local(d->test_broker_context);
#endif
    /* Primary IOKitUser-100231.120.3 IOKitLib.c: IOObjectRelease is exactly
     * mach_port_deallocate. This is local IPC bookkeeping AFTER FINISH ACK,
     * not an IOServiceClose RPC or evidence of any hardware retirement. */
    return IOObjectRelease(d->connection);
}
static extgpu_status rpc(extgpu_device *d, uint32_t selector, const uint64_t *in,
                          uint32_t count, uint64_t *out, uint32_t expected, const char *op)
{
    uint32_t returned = expected;
    kern_return_t kr = scalar(d, selector, in, count, out, &returned);
    if (kr != KERN_SUCCESS) return error(d, native_status(kr), kr, op);
    if (returned != expected) return error(d, EXTGPU_E_PROTOCOL, 0, op);
    return error(d, EXTGPU_OK, 0, op);
}
static extgpu_status broker_rpc(extgpu_device *d, uint32_t selector, const uint64_t *in,
                                 uint32_t count, uint64_t *out, uint32_t expected, const char *op)
{
    uint32_t returned = expected;
    kern_return_t kr;
#ifdef EXTGPU_TESTING
    if (d->test) {
        if (!d->test_broker) return error(d, EXTGPU_E_UNSUPPORTED, 0, "Broker.test_transport_missing");
        kr = d->test_broker->scalar(d->test_broker_context, selector, in, count, out, &returned);
    } else
#endif
        kr = IOConnectCallScalarMethod(d->broker_connection, selector, in, count, out, &returned);
    if (kr != KERN_SUCCESS) return error(d, native_status(kr), kr, op);
    if (returned != expected) return error(d, EXTGPU_E_PROTOCOL, 0, op);
    return error(d, EXTGPU_OK, 0, op);
}
static extgpu_status query(extgpu_device *d)
{
    if (d->original_closed) return error(d, EXTGPU_E_UNSAFE, 0, "connection.closed");
    if (d->surprise_detaching) return error(d, EXTGPU_E_UNSAFE, 0, "Surprise.operation_denied");
    if (d->cold_attempted) return error(d, EXTGPU_E_UNSAFE, 0, "Cold.ordinary_query_denied");
    if (d->warm_vm_pending) return error(d, EXTGPU_E_UNSAFE, 0, "WarmVM.query_denied");
    d->protocol_valid = false;
    uint64_t in[3] = {EXTGPU_PROTOCOL_MAJOR, EXTGPU_PROTOCOL_MINIMUM_MINOR, EXTGPU_PROTOCOL_FINGERPRINT};
    uint64_t out[8] = {0};
    extgpu_status result = rpc(d, EXTGPU_SELECTOR_QUERY_PROTOCOL, in, 3, out, 8, "QueryProtocol");
    if (result != EXTGPU_OK) return result;
    const bool configurable_dma = (out[3] & EXTGPU_CAP_CONFIGURABLE_DMA_BUDGET) != 0;
    const uint64_t word_window_dependencies = EXTGPU_CAP_MMIO_WINDOW | EXTGPU_CAP_BAR_WORDS | EXTGPU_CAP_BAR;
    if (out[0] != EXTGPU_PROTOCOL_MAJOR || out[1] < in[1] || out[1] > UINT32_MAX || !out[2] ||
        out[7] != EXTGPU_PROTOCOL_FINGERPRINT || !(out[3] & EXTGPU_CAP_GENERATION) ||
        (out[3] & EXTGPU_STATE_QUARANTINED) || out[4] > EXTGPU_MAX_DMA_REGIONS ||
        (configurable_dma ? (out[1] < EXTGPU_DMA_BUDGET_MINIMUM_MINOR || !out[5]) :
                            out[5] > EXTGPU_MAX_PREPARED_DMA_BYTES) ||
        ((out[3] & EXTGPU_CAP_SURPRISE_REMOVAL) &&
         (out[1] < 30 || !(out[3] & EXTGPU_CAP_RECOVERY_BROKER))) ||
        ((out[3] & EXTGPU_CAP_VM_DMA_SEGMENTS) && out[1] < EXTGPU_VM_DMA_MINIMUM_MINOR) ||
        ((out[3] & EXTGPU_CAP_VM_DYNAMIC_DMA) && out[1] < EXTGPU_VM_DYNAMIC_DMA_MINIMUM_MINOR) ||
        ((out[3] & EXTGPU_CAP_MMIO_WINDOW_WORD_WRITES) &&
         (out[1] < EXTGPU_MMIO_WINDOW_WORD_WRITES_MINIMUM_MINOR ||
          (out[3] & word_window_dependencies) != word_window_dependencies)) ||
        ((out[3] & EXTGPU_CAP_MMIO_RING) &&
         (out[1] < EXTGPU_MMIO_RING_MINIMUM_MINOR || !(out[3] & EXTGPU_CAP_BAR))) ||
        ((out[3] & EXTGPU_CAP_MMIO_RING_POLL) &&
         (out[1] < EXTGPU_MMIO_RING_POLL_MINIMUM_MINOR || !(out[3] & EXTGPU_CAP_MMIO_RING))) ||
        ((out[3] & EXTGPU_CAP_MMIO_RING_ADAPTIVE) &&
         (out[1] < EXTGPU_MMIO_RING_ADAPTIVE_MINIMUM_MINOR || !(out[3] & EXTGPU_CAP_MMIO_RING_POLL))) ||
        ((out[3] & EXTGPU_CAP_MMIO_RING_IRQ_ARM) &&
         (out[1] < EXTGPU_MMIO_RING_IRQ_ARM_MINIMUM_MINOR ||
          (out[3] & (EXTGPU_CAP_MMIO_RING_ADAPTIVE | EXTGPU_CAP_IRQ_SHARED_NOTIFICATION)) !=
          (EXTGPU_CAP_MMIO_RING_ADAPTIVE | EXTGPU_CAP_IRQ_SHARED_NOTIFICATION))) ||
        ((out[3] & EXTGPU_CAP_MMIO_RING_EVENT) &&
         (out[1] < EXTGPU_MMIO_RING_EVENT_MINIMUM_MINOR || !(out[3] & EXTGPU_CAP_MMIO_RING_ADAPTIVE))) ||
        ((out[3] & EXTGPU_CAP_MMIO_RING_GROUPED_STORES) &&
         (out[1] < EXTGPU_MMIO_RING_GROUPED_STORES_MINIMUM_MINOR || !(out[3] & EXTGPU_CAP_MMIO_RING))) ||
        out[6] > EXTGPU_MAX_RETURNED_SEGMENTS)
        return error(d, EXTGPU_E_PROTOCOL, 0, "QueryProtocol.values");
    d->protocol = (extgpu_protocol_info){
        .major = (uint32_t)out[0], .minor = (uint32_t)out[1], .generation = out[2],
        .capabilities = out[3], .maximum_dma_regions = out[4],
        .maximum_dma_bytes = out[5], .maximum_dma_segments = out[6], .abi_fingerprint = out[7]};
    d->protocol_valid = true;
    d->dma_budget_uncertain = false;
    return EXTGPU_OK;
}
static extgpu_status ready(extgpu_device *d, uint64_t capability, bool claim)
{
    if (d->original_closed) return error(d, EXTGPU_E_UNSAFE, 0, "connection.closed");
    if (d->surprise_detaching) return error(d, EXTGPU_E_UNSAFE, 0, "Surprise.operation_denied");
    if (d->cold_attempted) return error(d, EXTGPU_E_UNSAFE, 0, "Cold.ordinary_operation_denied");
    if (d->warm_vm_pending) return error(d, EXTGPU_E_UNSAFE, 0, "WarmVM.operation_denied");
    if (!d->protocol_valid) return error(d, EXTGPU_E_STALE, 0, "protocol.invalid");
    if ((d->protocol.capabilities & capability) != capability)
        return error(d, EXTGPU_E_UNSUPPORTED, 0, "capability.absent");
    if (claim && !d->claimed) return error(d, EXTGPU_E_NOT_CLAIMED, 0, "claim.required");
    if (claim && d->ownership_uncertain) return error(d, EXTGPU_E_UNSAFE, 0, "claim.ownership_uncertain");
    return EXTGPU_OK;
}
static bool add_overflow(uint64_t a, uint64_t b) { return b > UINT64_MAX - a; }
static bool resources(extgpu_device *d) { return d->bar_mask || d->buffers || d->irq; }
static bool width_valid(uint32_t offset, uint32_t width)
{ return (width == 1 || width == 2 || width == 4) && offset % width == 0 && offset <= 4096 - width; }

uint32_t extgpu_sdk_abi_version(void) { return EXTGPU_SDK_ABI_VERSION; }

const char *extgpu_status_string(extgpu_status s)
{
    switch (s) {
    case EXTGPU_OK: return "success";
    case EXTGPU_E_ARGUMENT: return "invalid argument";
    case EXTGPU_E_MEMORY: return "allocation failed";
    case EXTGPU_E_IO: return "IOKit operation failed";
    case EXTGPU_E_NOT_FOUND: return "device not found";
    case EXTGPU_E_PROTOCOL: return "driver protocol mismatch";
    case EXTGPU_E_UNSUPPORTED: return "capability unavailable";
    case EXTGPU_E_BUSY: return "resource or ownership busy";
    case EXTGPU_E_NOT_CLAIMED: return "exclusive claim required";
    case EXTGPU_E_STALE: return "stale generation or protocol";
    case EXTGPU_E_BOUNDS: return "range outside resource";
    case EXTGPU_E_COVERAGE: return "DMA segment does not cover requested bytes";
    case EXTGPU_E_UNSAFE: return "cleanup unacknowledged; retain resource and backing";
    case EXTGPU_E_CAPACITY: return "capacity limit reached";
    }
    return "unknown status";
}
extgpu_status extgpu_session_create(extgpu_session **out)
{
    if (!out) return EXTGPU_E_ARGUMENT;
    *out = calloc(1, sizeof(**out));
    if (!*out) return EXTGPU_E_MEMORY;
    if (pthread_mutex_init(&(*out)->lock, NULL)) { free(*out); *out = NULL; return EXTGPU_E_MEMORY; }
    return EXTGPU_OK;
}
extgpu_status extgpu_session_destroy(extgpu_session **ptr)
{
    if (!ptr || !*ptr) return EXTGPU_E_ARGUMENT;
    extgpu_session *s = *ptr;
    pthread_mutex_lock(&s->lock);
    if (s->devices) { pthread_mutex_unlock(&s->lock); return EXTGPU_E_BUSY; }
    pthread_mutex_unlock(&s->lock);
    pthread_mutex_destroy(&s->lock); free(s); *ptr = NULL;
    return EXTGPU_OK;
}
static bool service_matches(io_service_t service)
{
    CFTypeRef value = IORegistryEntryCreateCFProperty(service, CFSTR("IOUserClass"), kCFAllocatorDefault, 0);
    bool equal = value && CFGetTypeID(value) == CFStringGetTypeID() &&
                 CFEqual(value, CFSTR(EXTGPU_DRIVER_CLASS));
    if (value) CFRelease(value);
    /* Reset companions are private driver services, not SDK GPU endpoints. */
    if (equal) {
        CFTypeRef role = IORegistryEntryCreateCFProperty(service, CFSTR("OpenPCIResetCompanion"),
                                                         kCFAllocatorDefault, 0);
        if (role == kCFBooleanTrue) equal = false;
        if (role) CFRelease(role);
    }
    return equal;
}
static uint32_t pci_property(io_registry_entry_t entry, CFStringRef name)
{
    CFTypeRef value = IORegistryEntryCreateCFProperty(entry, name, kCFAllocatorDefault, 0);
    uint32_t result = 0;
    if (value && CFGetTypeID(value) == CFDataGetTypeID() && CFDataGetLength(value) >= 4) {
        const UInt8 *bytes = CFDataGetBytePtr(value);
        result = (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
    } else if (value && CFGetTypeID(value) == CFNumberGetTypeID()) {
        int64_t n = 0;
        if (CFNumberGetValue(value, kCFNumberSInt64Type, &n) && n >= 0 && (uint64_t)n <= UINT32_MAX) result = (uint32_t)n;
    }
    if (value) CFRelease(value);
    return result;
}
static extgpu_device_info describe(io_service_t service)
{
    extgpu_device_info info = {0};
    io_name_t name = {0};
    uint64_t registry_id = 0;
    if (IORegistryEntryGetRegistryEntryID(service, &registry_id) == KERN_SUCCESS)
        info.registry_id = registry_id;
    if (IORegistryEntryGetName(service, name) == KERN_SUCCESS)
        snprintf(info.registry_name, sizeof(info.registry_name), "%s", name);
    io_registry_entry_t entry = service;
    bool pci_seen = false;
    IOObjectRetain(entry);
    for (unsigned depth = 0; depth < 128; ++depth) {
        if (IOObjectConformsTo(entry, "IOPCIDevice")) {
            if (!pci_seen) {
                pci_seen = true;
                uint64_t pci_id = 0;
                if (IORegistryEntryGetRegistryEntryID(entry, &pci_id) == KERN_SUCCESS)
                    info.pci_registry_id = pci_id;
                info.vendor_id = pci_property(entry, CFSTR("vendor-id"));
                info.device_id = pci_property(entry, CFSTR("device-id"));
            }
            if (IORegistryEntryGetName(entry, name) == KERN_SUCCESS)
                snprintf(info.host_root, sizeof(info.host_root), "%s", name);
        }
        io_registry_entry_t parent = IO_OBJECT_NULL;
        kern_return_t kr = IORegistryEntryGetParentEntry(entry, kIOServicePlane, &parent);
        IOObjectRelease(entry); entry = IO_OBJECT_NULL;
        if (kr != KERN_SUCCESS || !parent) break;
        entry = parent;
    }
    if (entry) IOObjectRelease(entry);
    return info;
}
extgpu_status extgpu_enumerate(extgpu_session *s, extgpu_device_info *items, size_t capacity, size_t *count)
{
    if (!s || !count || (!items && capacity)) return EXTGPU_E_ARGUMENT;
    *count = 0;
    CFMutableDictionaryRef match = IOServiceMatching("IOUserService");
    if (!match) return EXTGPU_E_MEMORY;
    io_iterator_t iterator = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault, match, &iterator);
    if (kr != KERN_SUCCESS) return native_status(kr);
    io_service_t service;
    while ((service = IOIteratorNext(iterator))) {
        if (service_matches(service)) {
            if (*count < capacity) items[*count] = describe(service);
            ++*count;
        }
        IOObjectRelease(service);
        if (*count > 16384) { IOObjectRelease(iterator); return EXTGPU_E_CAPACITY; }
    }
    IOObjectRelease(iterator);
    return items && *count > capacity ? EXTGPU_E_CAPACITY : EXTGPU_OK;
}
static extgpu_device *new_device(extgpu_session *s)
{
    extgpu_device *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    if (pthread_mutex_init(&d->lock, NULL)) { free(d); return NULL; }
    d->session = s;
    d->dma_address_bits = 32;
    return d;
}
extgpu_status extgpu_device_open(extgpu_session *s, uint64_t id, extgpu_device **out)
{
    if (!s || !id || !out) return EXTGPU_E_ARGUMENT;
    *out = NULL;
    CFMutableDictionaryRef match = IORegistryEntryIDMatching(id);
    if (!match) return EXTGPU_E_MEMORY;
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, match);
    if (!service) return EXTGPU_E_NOT_FOUND;
    uint64_t actual = 0;
    if (IORegistryEntryGetRegistryEntryID(service, &actual) != KERN_SUCCESS || actual != id || !service_matches(service)) {
        IOObjectRelease(service); return EXTGPU_E_NOT_FOUND;
    }
    /* Resolve the exact PCI provider before opening the user client. Later
     * disappearance of this DEXT endpoint is not provider-removal evidence. */
    const extgpu_device_info description = describe(service);
    if (description.registry_id != id || !description.pci_registry_id ||
        description.pci_registry_id == id) {
        IOObjectRelease(service); return EXTGPU_E_NOT_FOUND;
    }
    extgpu_device *d = new_device(s);
    if (!d) { IOObjectRelease(service); return EXTGPU_E_MEMORY; }
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &d->connection);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS) { pthread_mutex_destroy(&d->lock); free(d); return native_status(kr); }
    d->registry_id = id;
    d->pci_registry_id = description.pci_registry_id;
    extgpu_status status = query(d);
    if (status != EXTGPU_OK) {
        IOServiceClose(d->connection); pthread_mutex_destroy(&d->lock); free(d); return status;
    }
    pthread_mutex_lock(&s->lock); ++s->devices; pthread_mutex_unlock(&s->lock);
    *out = d;
    return EXTGPU_OK;
}
extgpu_status extgpu_device_protocol(extgpu_device *d, extgpu_protocol_info *out)
{
    if (!d || !out) return EXTGPU_E_ARGUMENT;
    pthread_mutex_lock(&d->lock);
    extgpu_status st = d->original_closed ? error(d, EXTGPU_E_UNSAFE, 0, "connection.closed") :
        d->cold_attempted ? error(d, EXTGPU_E_UNSAFE, 0, "Cold.protocol_denied") :
        d->warm_vm_pending ? error(d, EXTGPU_E_UNSAFE, 0, "WarmVM.protocol_denied") :
        (d->protocol_valid ? EXTGPU_OK : error(d, EXTGPU_E_STALE, 0, "protocol.invalid"));
    if (st == EXTGPU_OK && d->dma_budget_uncertain)
        st = error(d, EXTGPU_E_UNSAFE, 0, "DMA.budget_uncertain");
    if (st == EXTGPU_OK) *out = d->protocol;
    pthread_mutex_unlock(&d->lock); return st;
}
extgpu_status extgpu_device_last_error(extgpu_device *d, extgpu_error_info *out)
{
    if (!d || !out) return EXTGPU_E_ARGUMENT;
    pthread_mutex_lock(&d->lock); *out = d->error; pthread_mutex_unlock(&d->lock);
    return EXTGPU_OK;
}
extgpu_status extgpu_device_identity(extgpu_device *d, extgpu_pci_identity *out)
{
    if (!d || !out) return EXTGPU_E_ARGUMENT;
    pthread_mutex_lock(&d->lock);
    uint64_t values[6] = {0};
    extgpu_status st = ready(d, 0, false);
    if (st == EXTGPU_OK) st = rpc(d, EXTGPU_SELECTOR_GET_IDENTITY, NULL, 0, values, 6, "GetIdentity");
    if (st == EXTGPU_OK && (values[0] > 255 || values[1] > 31 || values[2] > 7 || values[3] > 65535 || values[4] > 65535 || values[5] > UINT32_MAX))
        st = error(d, EXTGPU_E_PROTOCOL, 0, "GetIdentity.values");
    if (st == EXTGPU_OK) *out = (extgpu_pci_identity){(uint32_t)values[0], (uint32_t)values[1], (uint32_t)values[2], (uint32_t)values[3], (uint32_t)values[4], (uint32_t)values[5]};
    pthread_mutex_unlock(&d->lock); return st;
}
static bool claim_negative_is_preflight(kern_return_t native)
{
    /* Canonical Claim returns these negatives only before provider/client
     * ownership is published. IPC errors and malformed successful replies do
     * not establish whether the request took effect. */
    switch (native) {
    case kIOReturnBadArgument: case kIOReturnUnsupported:
    case kIOReturnBusy: case kIOReturnExclusiveAccess:
    case kIOReturnNoMemory: case kIOReturnNotReady:
    case kIOReturnNotPermitted: case kIOReturnNotOpen:
    case kIOReturnNotAttached: case kIOReturnNoDevice: case kIOReturnNotFound:
        return true;
    default: return false;
    }
}
extgpu_status extgpu_device_claim(extgpu_device *d)
{
    if (!d) return EXTGPU_E_ARGUMENT;
    pthread_mutex_lock(&d->lock);
    extgpu_status st = d->surprise_requested ? error(d, EXTGPU_E_BUSY, 0, "Surprise.close_before_reclaim") :
        d->cold_attempted ? error(d, EXTGPU_E_UNSAFE, 0, "Cold.claim_denied") :
        (d->claimed ? (d->ownership_uncertain ? EXTGPU_E_UNSAFE : EXTGPU_E_BUSY) : query(d));
    if (st == EXTGPU_OK) st = ready(d, EXTGPU_CAP_EXCLUSIVE_CLAIM, false);
    if (st == EXTGPU_OK) {
        st = rpc(d, EXTGPU_SELECTOR_CLAIM, NULL, 0, NULL, 0, "Claim");
        if (st == EXTGPU_OK) { d->claimed = true; d->ownership_uncertain = false; d->dma_address_bits = 32; }
        else if (!claim_negative_is_preflight(d->error.native_code)) {
            d->claimed = true; /* Unknown reply may follow a successful claim. */
            d->ownership_uncertain = true;
            st = error(d, EXTGPU_E_UNSAFE, d->error.native_code, "Claim.ownership_uncertain");
        }
    }
    pthread_mutex_unlock(&d->lock); return st;
}
extgpu_status extgpu_device_release(extgpu_device *d)
{
    if (!d) return EXTGPU_E_ARGUMENT;
    pthread_mutex_lock(&d->lock);
    extgpu_status st = d->original_closed ? error(d, EXTGPU_E_UNSAFE, 0, "connection.closed") :
        d->cold_attempted ? error(d, EXTGPU_E_UNSAFE, 0, "Cold.release_denied") :
        (d->claimed ? EXTGPU_OK : error(d, EXTGPU_E_NOT_CLAIMED, 0, "Release.no_claim"));
    if (st == EXTGPU_OK && resources(d)) st = error(d, EXTGPU_E_BUSY, 0, "Release.live_resources");
    /* Reset or a lost release reply may have advanced the epoch. Refresh only
     * after every child has drained; a transient query failure is retryable by
     * calling release again, without admitting operations on stale handles. */
    if (st == EXTGPU_OK) st = query(d);
    if (st == EXTGPU_OK) st = ready(d, EXTGPU_CAP_SAFE_RELEASE, false);
    uint64_t generation = d->protocol.generation;
    bool release_sent = st == EXTGPU_OK;
    if (release_sent) st = rpc(d, EXTGPU_SELECTOR_RELEASE_OWNER, &generation, 1, NULL, 0, "ReleaseOwner");
    if (st == EXTGPU_OK) { d->claimed = false; d->ownership_uncertain = false; d->protocol_valid = false; }
    else if (release_sent && d->ownership_uncertain && st == EXTGPU_E_NOT_CLAIMED && d->error.native_code == kIOReturnNotOpen) {
        /* A fresh generation-checked driver explicitly says this client owns
         * nothing. This is NOT a successful release acknowledgement: retain
         * NOT_CLAIMED as the result, but permit closing the unclaimed client. */
        d->claimed = false; d->ownership_uncertain = false;
    } else if (release_sent && !claim_negative_is_preflight(d->error.native_code)) {
        /* An unknown transport/malformed-success reply may follow actual
         * release and an epoch change. Admit no stale-owner operations. */
        d->ownership_uncertain = true; d->protocol_valid = false;
        d->error.status = EXTGPU_E_UNSAFE; st = EXTGPU_E_UNSAFE;
    }
    pthread_mutex_unlock(&d->lock); return st;
}
extgpu_status extgpu_device_set_dma_address_bits(extgpu_device *d, uint32_t bits)
{
    if (!d || bits < 32 || bits > 64) return EXTGPU_E_ARGUMENT;
    pthread_mutex_lock(&d->lock);
    extgpu_status st = ready(d, EXTGPU_CAP_DMA_ADDRESS_BITS, true);
    if (st == EXTGPU_OK && resources(d)) st = error(d, EXTGPU_E_BUSY, 0, "DMAMask.live_resources");
    uint64_t in = bits;
    if (st == EXTGPU_OK) st = rpc(d, EXTGPU_SELECTOR_SET_DMA_ADDRESS_BITS, &in, 1, NULL, 0, "SetDMAAddressBits");
    if (st == EXTGPU_OK) d->dma_address_bits = bits;
    pthread_mutex_unlock(&d->lock); return st;
}
static extgpu_status surprise_close_escrow(extgpu_device *d, const uint64_t proof[8]);

extgpu_status extgpu_device_close(extgpu_device **ptr)
{
    if (!ptr || !*ptr) return EXTGPU_E_ARGUMENT;
    extgpu_device *d = *ptr;
    pthread_mutex_lock(&d->lock);
    if (d->cold_attempted && !d->cold_finished) { pthread_mutex_unlock(&d->lock); return EXTGPU_E_UNSAFE; }
    if (d->claimed || resources(d)) { pthread_mutex_unlock(&d->lock); return EXTGPU_E_BUSY; }
    if (d->surprise_requested && !d->surprise_escrow_closed) {
        const extgpu_status st = surprise_close_escrow(d, NULL);
        if (st != EXTGPU_OK) { pthread_mutex_unlock(&d->lock); return st; }
    }
    if (d->mmio_ring) {
        if (d->mmio_grouped_requests)
            fprintf(stderr, "OPENPCI_MMIO_GROUPED requests=%" PRIu64 " writes=%" PRIu64 "\n",
                    d->mmio_grouped_requests, d->mmio_grouped_writes);
        if (d->mmio_ring_flags & OPENPCI_MMIO_RING_EVENT_FLAG)
            fputs("OPENPCI_MMIO_RING_EVENT enabled=1\n", stderr);
        if (!d->surprise_released && (d->mmio_ring_flags & OPENPCI_MMIO_RING_IRQ_ARM_FLAG))
            fprintf(stderr, "OPENPCI_MMIO_RING_IRQ arms=%" PRIu64 " hits=%" PRIu64
                    " extensions=%" PRIu64 "\n",
                    __atomic_load_n(&d->mmio_ring->irqArms, __ATOMIC_RELAXED),
                    __atomic_load_n(&d->mmio_ring->irqHits, __ATOMIC_RELAXED),
                    __atomic_load_n(&d->mmio_ring->irqExtensions, __ATOMIC_RELAXED));
        unmap_memory(d, EXTGPU_MEMORY_MMIO_RING, (uintptr_t)d->mmio_ring);
        d->mmio_ring = NULL;
        fprintf(stderr, "OPENPCI_MMIO_RING batches=%" PRIu64 " entries=%" PRIu64
                " kicks=%" PRIu64 " uncertain=%d\n", d->mmio_ring_batches, d->mmio_ring_entries,
                d->mmio_ring_kicks, d->mmio_ring_failed);
        if (d->mmio_ring_poll_us)
            fprintf(stderr, "OPENPCI_MMIO_RING_POLL idle_us=%" PRIu64 " adaptive=%d\n",
                    d->mmio_ring_poll_us, !!(d->mmio_ring_flags & OPENPCI_MMIO_RING_ADAPTIVE_FLAG));
    }
    if (d->mmio_worker.wake || d->mmio_queue) {
        // Finish kicks before relinquishing the connection's Mach name.
        if (d->mmio_worker.wake) {
            struct mmio_worker *worker = &d->mmio_worker;
            __atomic_store_n(&worker->stop, true, __ATOMIC_RELEASE);
            dispatch_semaphore_signal(worker->wake);
            const int joined = pthread_join(worker->thread, NULL);
            if (joined) {
                const extgpu_status status = error(d, EXTGPU_E_UNSAFE, joined, "MMIOWindow.worker_join");
                pthread_mutex_unlock(&d->lock);
                return status;
            }
            dispatch_release(worker->wake);
            worker->wake = NULL;
        } else {
            dispatch_sync(d->mmio_queue, ^{});
        }
        unmap_memory(d, EXTGPU_MEMORY_MMIO_WINDOW, (uintptr_t)d->mmio_window);
        if (d->mmio_queue) dispatch_release(d->mmio_queue);
        d->mmio_queue = NULL;
        d->mmio_window = NULL;
        fprintf(stderr, "OPENPCI_MMIO_WINDOW cold=%" PRIu64 " warm=%" PRIu64
                " fallbacks=%" PRIu64 " idle_scalar=%" PRIu64 " uncertain=%d\n", d->mmio_window_cold,
                d->mmio_window_warm, d->mmio_window_fallbacks, d->mmio_idle_scalar, d->mmio_window_failed);
        fprintf(stderr, "OPENPCI_MMIO_WINDOW_WORD_WRITES enabled=%d replies=%" PRIu64 " fallbacks=%" PRIu64 "\n",
                d->mmio_window_word_writes, d->mmio_window_word_replies, d->mmio_window_word_fallbacks);
    }
    kern_return_t kr = KERN_SUCCESS;
    if (d->cold_finished || d->surprise_released) {
        if (!d->broker_bound) { pthread_mutex_unlock(&d->lock); return EXTGPU_E_UNSAFE; }
        if (!d->original_local_dropped) {
            kr = drop_original_local(d);
            if (kr != KERN_SUCCESS && kr != KERN_INVALID_NAME) {
                extgpu_status st = error(d, native_status(kr), kr, "Cold.local_reference_retained");
                pthread_mutex_unlock(&d->lock); return st;
            }
            d->original_local_dropped = true;
            d->connection = IO_OBJECT_NULL;
        }
    }
    if (d->broker_opened) {
        kr = close_broker_connection(d);
        /* Primary IOKitLib.c IOServiceClose always deallocates its connect
         * reference even when io_service_close fails. Never reuse that name. */
        d->broker_opened = false;
        d->broker_connection = IO_OBJECT_NULL;
        if (kr != KERN_SUCCESS) {
            extgpu_status st = error(d, native_status(kr), kr, "Broker.close_local_reference_consumed");
            pthread_mutex_unlock(&d->lock); return st;
        }
    }
    if (!d->cold_finished && !d->surprise_released && !d->original_closed) {
        kr = close_connection(d);
        /* IOServiceClose deallocates the connect reference even when its RPC
         * fails. Retain only this SDK object for local cleanup retry; the old
         * Mach name may already have been recycled for an unrelated service. */
        d->original_closed = true;
        d->connection = IO_OBJECT_NULL;
        d->protocol_valid = false;
        if (kr != KERN_SUCCESS) {
            extgpu_status st = error(d, native_status(kr), kr, "IOServiceClose.local_reference_consumed");
            pthread_mutex_unlock(&d->lock); return st;
        }
    }
    pthread_mutex_unlock(&d->lock);
    pthread_mutex_lock(&d->session->lock); --d->session->devices; pthread_mutex_unlock(&d->session->lock);
    pthread_mutex_destroy(&d->lock); free(d); *ptr = NULL;
    return EXTGPU_OK;
}
extgpu_status extgpu_device_reset(extgpu_device *d)
{
    if (!d) return EXTGPU_E_ARGUMENT;
    pthread_mutex_lock(&d->lock);
    extgpu_status st = ready(d, EXTGPU_CAP_RESET, true);
    if (st == EXTGPU_OK && d->surprise_requested)
        st = error(d, EXTGPU_E_BUSY, 0, "Surprise.reset_requires_new_session");
    if (st == EXTGPU_OK && resources(d)) st = error(d, EXTGPU_E_BUSY, 0, "Reset.live_resources");
    uint64_t previous = d->protocol.generation;
    if (st == EXTGPU_OK) {
        st = rpc(d, EXTGPU_SELECTOR_RESET_DEVICE, NULL, 0, NULL, 0, "ResetDevice");
        d->protocol_valid = false;
        extgpu_status reset_result = st;
        extgpu_error_info reset_error = d->error;
        st = query(d);
        if (st == EXTGPU_OK) {
            if (d->protocol.generation != previous) d->dma_address_bits = 32;
            if (reset_result == EXTGPU_OK && d->protocol.generation == previous) {
                d->protocol_valid = false; st = error(d, EXTGPU_E_PROTOCOL, 0, "Reset.generation_unchanged");
            } else if (reset_result != EXTGPU_OK) { d->error = reset_error; st = reset_result; }
        }
    }
    pthread_mutex_unlock(&d->lock); return st;
}

static bool cold_key_matches(extgpu_device *d, const uint64_t *reply)
{
    return memcmp(d->cold_key, reply, sizeof(d->cold_key)) == 0;
}
static extgpu_status cold_unacknowledged(extgpu_device *d, const char *operation)
{
    return error(d, EXTGPU_E_UNSAFE, d->error.native_code, operation);
}
static void cold_info(extgpu_device *d, extgpu_cold_removal_info *out)
{
    *out = (extgpu_cold_removal_info){d->cold_key[0], d->cold_key[1], d->cold_key[2],
        {d->cold_key[3], d->cold_key[4]}, d->cold_enrolled_regions};
}
static extgpu_status cold_broker_preflight(extgpu_device *d)
{
    extgpu_status st = EXTGPU_OK;
    if (!d->broker_registry_id) {
        uint64_t generation = d->protocol.generation, endpoint[EXTGPU_RECOVERY_ENDPOINT_OUTPUT_COUNT] = {0};
        st = rpc(d, EXTGPU_SELECTOR_GET_RECOVERY_ENDPOINT, &generation, EXTGPU_RECOVERY_ENDPOINT_INPUT_COUNT,
                 endpoint, EXTGPU_RECOVERY_ENDPOINT_OUTPUT_COUNT, "Broker.get_endpoint");
        if (st != EXTGPU_OK) return st;
        if (!endpoint[0] || endpoint[0] == d->registry_id || endpoint[0] == d->pci_registry_id ||
            endpoint[1] != EXTGPU_PROTOCOL_MAJOR || endpoint[2] < EXTGPU_RECOVERY_PROTOCOL_MINIMUM_MINOR || endpoint[2] > UINT32_MAX)
            return error(d, EXTGPU_E_PROTOCOL, 0, "Broker.endpoint_identity");
        d->broker_registry_id = endpoint[0];
    }
    if (!d->broker_opened) {
        kern_return_t kr;
#ifdef EXTGPU_TESTING
        if (d->test) {
            if (!d->test_broker || !d->test_recovery)
                return error(d, EXTGPU_E_UNSUPPORTED, 0, "Broker.test_hooks_missing");
            uint64_t actual = 0;
            const char *class_name = NULL;
            kr = d->test_recovery->lookup(d->test_broker_context, d->broker_registry_id, &actual, &class_name);
            if (kr != KERN_SUCCESS) return error(d, native_status(kr), kr, "Broker.lookup");
            if (actual != d->broker_registry_id || !class_name || strcmp(class_name, EXTGPU_RECOVERY_BROKER_CLASS))
                return error(d, EXTGPU_E_NOT_FOUND, 0, "Broker.registry_or_class_mismatch");
            kr = d->test_recovery->open(d->test_broker_context);
        } else
#endif
        {
            CFMutableDictionaryRef match = IORegistryEntryIDMatching(d->broker_registry_id);
            if (!match) return error(d, EXTGPU_E_MEMORY, 0, "Broker.match_dictionary");
            io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, match);
            if (!service) return error(d, EXTGPU_E_NOT_FOUND, 0, "Broker.exact_service_missing");
            uint64_t actual = 0;
            CFTypeRef value = IORegistryEntryCreateCFProperty(service, CFSTR("IOUserClass"), kCFAllocatorDefault, 0);
            bool matches = IORegistryEntryGetRegistryEntryID(service, &actual) == KERN_SUCCESS &&
                actual == d->broker_registry_id && value && CFGetTypeID(value) == CFStringGetTypeID() &&
                CFEqual(value, CFSTR(EXTGPU_RECOVERY_BROKER_CLASS));
            if (value) CFRelease(value);
            if (!matches) { IOObjectRelease(service); return error(d, EXTGPU_E_NOT_FOUND, 0, "Broker.registry_or_class_mismatch"); }
            kr = IOServiceOpen(service, mach_task_self(), 0, &d->broker_connection);
            IOObjectRelease(service);
        }
        if (kr != KERN_SUCCESS) return error(d, native_status(kr), kr, "Broker.open");
        d->broker_opened = true;
    }
    if (!d->broker_ready) {
        uint64_t in[3] = {EXTGPU_PROTOCOL_MAJOR, EXTGPU_RECOVERY_PROTOCOL_MINIMUM_MINOR, EXTGPU_PROTOCOL_FINGERPRINT};
        uint64_t out[EXTGPU_QUERY_OUTPUT_COUNT] = {0};
        st = broker_rpc(d, EXTGPU_SELECTOR_QUERY_PROTOCOL, in, 3, out, EXTGPU_QUERY_OUTPUT_COUNT, "Broker.query");
        if (st != EXTGPU_OK) return st;
        if (out[0] != EXTGPU_PROTOCOL_MAJOR || out[1] < EXTGPU_RECOVERY_PROTOCOL_MINIMUM_MINOR || out[1] > UINT32_MAX ||
            out[2] != d->broker_registry_id || out[3] != EXTGPU_RECOVERY_BROKER_CAPABILITIES ||
            out[4] > EXTGPU_MAX_DMA_REGIONS || out[5] > EXTGPU_MAX_PREPARED_DMA_BYTES ||
            out[6] > EXTGPU_MAX_RETURNED_SEGMENTS || out[7] != EXTGPU_PROTOCOL_FINGERPRINT)
            return error(d, EXTGPU_E_PROTOCOL, 0, "Broker.query_values");
        d->broker_ready = true;
    }
    return st;
}
static void cold_escrow_input(extgpu_device *d, uint64_t in[EXTGPU_RECOVERY_ESCROW_INPUT_COUNT])
{
    memcpy(in, d->cold_key, sizeof(d->cold_key));
    memcpy(in + EXTGPU_COLD_KEY_WORDS, d->cold_private_cap, sizeof(d->cold_private_cap));
}
static extgpu_status cold_prepare(extgpu_device *d)
{
    uint64_t in[EXTGPU_RECOVERY_ESCROW_INPUT_COUNT], out[EXTGPU_RECOVERY_PREPARE_OUTPUT_COUNT] = {0};
    cold_escrow_input(d, in);
    extgpu_status st = rpc(d, EXTGPU_SELECTOR_PREPARE_RECOVERY_ESCROW, in, EXTGPU_RECOVERY_ESCROW_INPUT_COUNT,
                           out, EXTGPU_RECOVERY_PREPARE_OUTPUT_COUNT, "Broker.prepare_escrow");
    if (st == EXTGPU_OK && (!cold_key_matches(d, out) || out[5] != d->broker_registry_id))
        st = error(d, EXTGPU_E_PROTOCOL, 0, "Broker.prepare_reply");
    if (st == EXTGPU_OK) d->cold_prepared = true;
    return st;
}
static extgpu_status cold_bind(extgpu_device *d)
{
    uint64_t in[EXTGPU_RECOVERY_ESCROW_INPUT_COUNT], out[EXTGPU_RECOVERY_BIND_OUTPUT_COUNT] = {0};
    cold_escrow_input(d, in);
    extgpu_status st = broker_rpc(d, EXTGPU_SELECTOR_BIND_RECOVERY_ESCROW, in, EXTGPU_RECOVERY_ESCROW_INPUT_COUNT,
                                  out, EXTGPU_RECOVERY_BIND_OUTPUT_COUNT, "Broker.bind_escrow");
    if (st == EXTGPU_OK && (!cold_key_matches(d, out) || out[5] != d->cold_enrolled_regions || out[6] != EXTGPU_COLD_STATE_BOUND))
        st = error(d, EXTGPU_E_PROTOCOL, 0, "Broker.bind_reply");
    if (st == EXTGPU_OK) { d->broker_bound = true; d->cold_prepared = true; }
    return st;
}
static extgpu_status cold_snapshot(extgpu_device *d)
{
    uint64_t out[EXTGPU_COLD_BEGIN_OUTPUT_COUNT] = {0};
    extgpu_status st = broker_rpc(d, EXTGPU_SELECTOR_COLD_BEGIN, d->cold_key, EXTGPU_COLD_BEGIN_INPUT_COUNT,
                                  out, EXTGPU_COLD_BEGIN_OUTPUT_COUNT, "Broker.frozen_snapshot");
    if (st == EXTGPU_OK && (!cold_key_matches(d, out) || out[5] != d->cold_enrolled_regions || out[6] != EXTGPU_COLD_STATE_FROZEN))
        st = error(d, EXTGPU_E_PROTOCOL, 0, "Broker.snapshot_reply");
    if (st == EXTGPU_OK) d->cold_acknowledged = true;
    return st;
}
extgpu_status extgpu_cold_removal_begin(extgpu_device *d, extgpu_cold_removal_info *out)
{
    if (!d || !out) return EXTGPU_E_ARGUMENT;
    pthread_mutex_lock(&d->lock);
    if (d->surprise_requested) { pthread_mutex_unlock(&d->lock); return EXTGPU_E_BUSY; }
    extgpu_status st = EXTGPU_OK;
    bool first_attempt = false;
    if (d->cold_finished) st = error(d, EXTGPU_E_UNSAFE, 0, "Cold.begin_after_finish");
    if (st == EXTGPU_OK && !d->cold_attempted) {
        st = ready(d, EXTGPU_CAP_COLD_REMOVAL | EXTGPU_CAP_RECOVERY_BROKER, true);
        if (st == EXTGPU_OK && d->protocol.minor < EXTGPU_RECOVERY_PROTOCOL_MINIMUM_MINOR)
            st = error(d, EXTGPU_E_UNSUPPORTED, 0, "Cold.protocol_minor");
        if (st == EXTGPU_OK && (!d->registry_id || !d->pci_registry_id || d->registry_id == d->pci_registry_id))
            st = error(d, EXTGPU_E_UNSAFE, 0, "Cold.provider_identity_missing");
        if (st == EXTGPU_OK && (d->bar_cache_attempted_mask || d->bar_mask || d->irq || d->allocated_buffer))
            st = error(d, EXTGPU_E_BUSY, 0, "Cold.unsupported_resources_or_BAR_history");
        if (st == EXTGPU_OK) st = cold_broker_preflight(d);
        if (st == EXTGPU_OK) {
            d->cold_key[0] = d->protocol.generation;
            d->cold_key[1] = d->registry_id;
            d->cold_key[2] = d->pci_registry_id;
            arc4random_buf(&d->cold_key[3], 2 * sizeof(uint64_t));
            if (!(d->cold_key[3] | d->cold_key[4])) d->cold_key[4] = 1;
            do { arc4random_buf(d->cold_private_cap, sizeof(d->cold_private_cap)); }
            while (!(d->cold_private_cap[0] | d->cold_private_cap[1]) ||
                   !memcmp(d->cold_private_cap, d->cold_key + 3, sizeof(d->cold_private_cap)));
            d->cold_enrolled_regions = d->buffers;
            /* Irreversible locally before sending: even a lost reply may
             * follow an acknowledged BME clear/freeze in the old endpoint. */
            d->cold_attempted = true;
            d->protocol_valid = false;
            first_attempt = true;
        }
    }
    if (st == EXTGPU_OK && first_attempt) st = cold_prepare(d);
    if (st == EXTGPU_OK && !d->broker_bound) {
        st = cold_bind(d);
        if (st != EXTGPU_OK && !first_attempt && !d->cold_prepared && d->error.native_code == kIOReturnNotFound) {
            st = cold_prepare(d); // Same old connection/key/capability only; never discover/reopen.
            if (st == EXTGPU_OK) st = cold_bind(d);
        }
    }
    if (st == EXTGPU_OK && !d->cold_acknowledged) {
        if (d->cold_gpu_begin_sent) {
            st = cold_snapshot(d); // Lost GPU ACK reconciles without the old PCI connection.
            if (st != EXTGPU_OK && d->error.native_code == kIOReturnNotReady && !d->cold_gpu_begin_acknowledged)
                st = EXTGPU_OK;
        }
    }
    if (st == EXTGPU_OK && !d->cold_acknowledged) {
        uint64_t reply[EXTGPU_COLD_BEGIN_OUTPUT_COUNT] = {0};
        d->cold_gpu_begin_sent = true;
        st = rpc(d, EXTGPU_SELECTOR_COLD_BEGIN, d->cold_key, EXTGPU_COLD_BEGIN_INPUT_COUNT,
                 reply, EXTGPU_COLD_BEGIN_OUTPUT_COUNT, "Cold.begin");
        if (st == EXTGPU_OK && (!cold_key_matches(d, reply) ||
            reply[5] != d->cold_enrolled_regions || reply[6] != EXTGPU_COLD_STATE_FROZEN))
            st = error(d, EXTGPU_E_PROTOCOL, 0, "Cold.begin_reply");
        if (st == EXTGPU_OK) { d->cold_gpu_begin_acknowledged = true; st = cold_snapshot(d); }
    }
    if (st != EXTGPU_OK && d->cold_attempted) st = cold_unacknowledged(d, "Cold.begin_unacknowledged");
    if (st == EXTGPU_OK) cold_info(d, out);
    pthread_mutex_unlock(&d->lock);
    return st;
}

static bool registry_absence_valid(uint64_t expected, uint64_t id, kern_return_t native,
                                       bool dictionary, bool iterator, bool valid_before,
                                       bool valid_after, uint64_t entries)
{
    /* IOKitLib.h explicitly permits SUCCESS + NULL for an empty match. A
     * non-NULL iterator still needs valid observations before and after Next.
     * NULL has no validity observations; contradictory flags are not proof. */
    return id == expected && native == KERN_SUCCESS && dictionary && entries == 0 &&
        (iterator ? valid_before && valid_after : !valid_before && !valid_after);
}
static extgpu_status confirm_registry_absent(extgpu_device *d, uint64_t id)
{
#ifdef EXTGPU_TESTING
    if (d->test) {
        const bool bridge = id == d->surprise_bridge;
        if (!bridge && !d->test_provider_seeded)
            return error(d, EXTGPU_E_UNSAFE, 0, "Recovery.test_provider_unseeded");
        const extgpu_test_provider_snapshot *s = bridge ? &d->test_bridge_provider : &d->test_provider;
        if (!registry_absence_valid(id, s->registry_id, s->native_status,
            s->dictionary_created == 1, s->iterator_present == 1,
            s->iterator_valid_before == 1, s->iterator_valid_after == 1, s->entry_count))
            return error(d, EXTGPU_E_UNSAFE, s->native_status, "Cold.provider_not_proven_absent");
        return EXTGPU_OK;
    }
#endif
    CFMutableDictionaryRef match = IORegistryEntryIDMatching(id);
    if (!match) return error(d, EXTGPU_E_UNSAFE, 0, "Cold.provider_dictionary_failed");
    io_iterator_t iterator = IO_OBJECT_NULL;
    const kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault, match, &iterator);
    bool valid_before = false, valid_after = false;
    uint64_t entries = 0;
    if (kr == KERN_SUCCESS && iterator) {
        valid_before = IOIteratorIsValid(iterator);
        const io_service_t entry = IOIteratorNext(iterator);
        if (entry) { entries = 1; IOObjectRelease(entry); }
        valid_after = IOIteratorIsValid(iterator);
    }
    if (iterator) IOObjectRelease(iterator);
    if (!registry_absence_valid(id, id, kr, true, iterator != IO_OBJECT_NULL,
                                     valid_before, valid_after, entries))
        return error(d, EXTGPU_E_UNSAFE, kr, "Cold.provider_not_proven_absent");
    return EXTGPU_OK;
}
extgpu_status extgpu_cold_removal_confirm(extgpu_device *d, uint32_t flags)
{
    if (!d || flags != EXTGPU_COLD_PHYSICAL_POWER_OFF) return EXTGPU_E_ARGUMENT;
    pthread_mutex_lock(&d->lock);
    extgpu_status st = EXTGPU_OK;
    if (!d->cold_acknowledged || d->cold_finished)
        st = error(d, EXTGPU_E_UNSAFE, 0, "Cold.confirm_requires_frozen_ACK");
    if (st == EXTGPU_OK && !d->cold_confirmed) st = confirm_registry_absent(d, d->cold_key[2]);
    if (st == EXTGPU_OK) d->cold_confirmed = true;
    pthread_mutex_unlock(&d->lock);
    return st;
}
extgpu_status extgpu_cold_removal_finish(extgpu_device *d)
{
    if (!d) return EXTGPU_E_ARGUMENT;
    pthread_mutex_lock(&d->lock);
    extgpu_status st = EXTGPU_OK;
    if (!d->cold_finished) {
        if (!d->cold_acknowledged || !d->cold_confirmed || !d->broker_bound || !d->broker_opened)
            st = error(d, EXTGPU_E_UNSAFE, 0, "Cold.finish_requires_confirmation");
        if (st == EXTGPU_OK && (resources(d) || d->allocated_buffer))
            st = error(d, EXTGPU_E_BUSY, 0, "Cold.finish_live_resources");
        if (st == EXTGPU_OK) {
            uint64_t in[EXTGPU_COLD_FINISH_INPUT_COUNT], reply[EXTGPU_COLD_FINISH_OUTPUT_COUNT] = {0};
            memcpy(in, d->cold_key, sizeof(d->cold_key));
            in[5] = EXTGPU_COLD_CONFIRM_REQUIRED;
            st = broker_rpc(d, EXTGPU_SELECTOR_COLD_FINISH, in, EXTGPU_COLD_FINISH_INPUT_COUNT,
                     reply, EXTGPU_COLD_FINISH_OUTPUT_COUNT, "Cold.finish");
            if (st == EXTGPU_OK && (!cold_key_matches(d, reply) ||
                reply[5] != EXTGPU_COLD_STATE_CLOSED || !reply[6]))
                st = error(d, EXTGPU_E_PROTOCOL, 0, "Cold.finish_reply");
            if (st == EXTGPU_OK) {
                d->cold_finished = true;
                d->claimed = false;
                d->ownership_uncertain = false;
                d->protocol_valid = false;
            } else st = cold_unacknowledged(d, "Cold.finish_unacknowledged");
        }
    }
    pthread_mutex_unlock(&d->lock);
    return st;
}
extgpu_status extgpu_config_read(extgpu_device *d, uint32_t off, uint32_t width, uint32_t *out)
{
    if (!d || !out || !width_valid(off, width)) return EXTGPU_E_ARGUMENT;
    pthread_mutex_lock(&d->lock);
    uint64_t in[2] = {off, width}, value = 0;
    extgpu_status st = ready(d, EXTGPU_CAP_CONFIG_READ, true);
    if (st == EXTGPU_OK) st = rpc(d, EXTGPU_SELECTOR_CONFIG_READ, in, 2, &value, 1, "ConfigRead");
    uint64_t mask = width == 4 ? UINT32_MAX : (UINT64_C(1) << (width * 8)) - 1;
    if (st == EXTGPU_OK && value > mask) st = error(d, EXTGPU_E_PROTOCOL, 0, "ConfigRead.width");
    if (st == EXTGPU_OK) *out = (uint32_t)value;
    pthread_mutex_unlock(&d->lock); return st;
}
extgpu_status extgpu_config_write(extgpu_device *d, uint32_t off, uint32_t width, uint32_t value)
{
    if (!d || !width_valid(off, width) || (width < 4 && value >= (UINT32_C(1) << (width * 8)))) return EXTGPU_E_ARGUMENT;
    pthread_mutex_lock(&d->lock);
    uint64_t in[3] = {off, width, value};
    extgpu_status st = ready(d, EXTGPU_CAP_CONFIG_WRITE, true);
    if (st == EXTGPU_OK) st = rpc(d, EXTGPU_SELECTOR_CONFIG_WRITE, in, 3, NULL, 0, "ConfigWrite");
    pthread_mutex_unlock(&d->lock); return st;
}
static extgpu_status bar_info(extgpu_device *d, uint32_t bar, extgpu_bar_info *info)
{
    uint64_t in = bar, out[3] = {0};
    extgpu_status st = ready(d, EXTGPU_CAP_BAR_MAPPING, true);
    if (st == EXTGPU_OK) st = rpc(d, EXTGPU_SELECTOR_GET_BAR_INFO, &in, 1, out, 3, "GetBARInfo");
    if (st == EXTGPU_OK && (out[0] > UINT8_MAX || !out[1] || out[2] > UINT8_MAX))
        st = error(d, EXTGPU_E_PROTOCOL, 0, "GetBARInfo.values");
    if (st == EXTGPU_OK) *info = (extgpu_bar_info){bar, (uint32_t)out[0], (uint32_t)out[2], 0, out[1]};
    return st;
}
extgpu_status extgpu_bar_get_info(extgpu_device *d, uint32_t bar, extgpu_bar_info *out)
{
    if (!d || bar >= 6 || !out) return EXTGPU_E_ARGUMENT;
    pthread_mutex_lock(&d->lock); extgpu_status st = bar_info(d, bar, out); pthread_mutex_unlock(&d->lock); return st;
}
static uint64_t surprise_tunnel_bridge(extgpu_device *d)
{
#ifdef EXTGPU_TESTING
    if (d->test) return d->test_reset_bridge;
#endif
    CFMutableDictionaryRef match = IORegistryEntryIDMatching(d->pci_registry_id);
    if (!match) return 0;
    io_service_t pci = IOServiceGetMatchingService(kIOMainPortDefault, match);
    if (!pci) return 0;
    uint64_t bridge = 0;
    io_registry_entry_t parent = IO_OBJECT_NULL;
    CFTypeRef tunneled = IORegistryEntryCreateCFProperty(pci, CFSTR("IOPCITunnelled"), kCFAllocatorDefault, 0);
    if (tunneled == kCFBooleanTrue &&
        IORegistryEntryGetParentEntry(pci, kIOServicePlane, &parent) == KERN_SUCCESS &&
        IOObjectConformsTo(parent, "IOPCI2PCIBridge")) {
        if (IORegistryEntryGetRegistryEntryID(parent, &bridge) != KERN_SUCCESS ||
            bridge == d->pci_registry_id || bridge == d->registry_id) bridge = 0;
    }
    if (parent) IOObjectRelease(parent);
    if (tunneled) CFRelease(tunneled);
    IOObjectRelease(pci);
    return bridge;
}

extgpu_status extgpu_surprise_removal_arm(extgpu_device *d)
{
    if (!d) return EXTGPU_E_ARGUMENT;
    pthread_mutex_lock(&d->lock);
    extgpu_status st = ready(d, EXTGPU_CAP_SURPRISE_REMOVAL | EXTGPU_CAP_RECOVERY_BROKER, true);
    if (st == EXTGPU_OK && (resources(d) || d->direct_bar_retired))
        st = error(d, EXTGPU_E_BUSY, 0, "Surprise.live_resources");
    if (st == EXTGPU_OK && !d->surprise_requested) {
        d->surprise_bridge = surprise_tunnel_bridge(d);
        if (!d->surprise_bridge)
            st = error(d, EXTGPU_E_UNSUPPORTED, 0, "Surprise.tunnel_scope");
        if (st == EXTGPU_OK) st = cold_broker_preflight(d);
        if (st == EXTGPU_OK) {
            d->cold_key[0] = d->protocol.generation;
            d->cold_key[1] = d->registry_id;
            d->cold_key[2] = d->pci_registry_id;
            arc4random_buf(d->cold_key + 3, 2 * sizeof(uint64_t));
            if (!(d->cold_key[3] | d->cold_key[4])) d->cold_key[4] = 1;
            do { arc4random_buf(d->cold_private_cap, sizeof(d->cold_private_cap)); }
            while (!(d->cold_private_cap[0] | d->cold_private_cap[1]) ||
                   !memcmp(d->cold_private_cap, d->cold_key + 3, sizeof(d->cold_private_cap)));
            d->surprise_requested = true;
        }
    }
    if (st == EXTGPU_OK && !d->broker_bound) {
        uint64_t in[EXTGPU_RECOVERY_ESCROW_INPUT_COUNT], out[EXTGPU_RECOVERY_PREPARE_OUTPUT_COUNT] = {0};
        cold_escrow_input(d, in);
        st = rpc(d, EXTGPU_SELECTOR_ARM_SURPRISE_REMOVAL, in, EXTGPU_RECOVERY_ESCROW_INPUT_COUNT,
                 out, EXTGPU_RECOVERY_PREPARE_OUTPUT_COUNT, "Surprise.arm");
        if (st == EXTGPU_OK && (!cold_key_matches(d, out) || out[5] != d->broker_registry_id))
            st = error(d, EXTGPU_E_PROTOCOL, 0, "Surprise.arm_reply");
        const extgpu_status bound = cold_bind(d);
        if (bound == EXTGPU_OK) st = EXTGPU_OK;
        else if (st == EXTGPU_OK) st = bound;
        /* A lost arm reply can be recovered by bind on the same connection.
         * Do not start a hardware session unless the VMM gets that ACK. */
    }
    pthread_mutex_unlock(&d->lock);
    return st;
}

static extgpu_status surprise_close_escrow(extgpu_device *d, const uint64_t proof[8])
{
    if (!d->broker_bound) {
        const extgpu_status st = cold_bind(d);
        if (st != EXTGPU_OK) {
            if (!d->surprise_detaching && d->error.native_code == kIOReturnNotFound) {
                d->surprise_escrow_closed = true;
                return EXTGPU_OK;
            }
            return st;
        }
    }
    uint64_t in[EXTGPU_SURPRISE_RELEASE_INPUT_COUNT] = {0}, out[EXTGPU_SURPRISE_RELEASE_OUTPUT_COUNT] = {0};
    memcpy(in, d->cold_key, sizeof(d->cold_key));
    in[5] = d->surprise_detaching;
    if (proof) memcpy(in + 6, proof, 8 * sizeof(uint64_t));
    extgpu_status st = broker_rpc(d, EXTGPU_SELECTOR_RELEASE_SURPRISE_REMOVAL, in,
        EXTGPU_SURPRISE_RELEASE_INPUT_COUNT, out, EXTGPU_SURPRISE_RELEASE_OUTPUT_COUNT, "Surprise.release");
    if (st == EXTGPU_OK && memcmp(in, out, sizeof(out)))
        st = error(d, EXTGPU_E_PROTOCOL, 0, "Surprise.release_reply");
    if (st == EXTGPU_OK) d->surprise_escrow_closed = true;
    return st;
}

extgpu_status extgpu_direct_bar_challenge(extgpu_device *d, uint64_t out_challenge[3])
{
    if (!d || !out_challenge) return EXTGPU_E_ARGUMENT;
    pthread_mutex_lock(&d->lock);
    extgpu_status st = ready(d, 0, false);
    if (st == EXTGPU_OK && (d->claimed || d->direct_bar_challenge_issued))
        st = error(d, EXTGPU_E_BUSY, 0, "DirectBAR.challenge_state");
    uint64_t challenge[3] = {0};
    if (st == EXTGPU_OK)
        st = rpc(d, UINT32_C(0x7e00), NULL, 0, challenge, 3, "DirectBAR.challenge");
    if (st == EXTGPU_OK && challenge[2] != d->protocol.generation)
        st = error(d, EXTGPU_E_STALE, 0, "DirectBAR.challenge_generation");
    if (st == EXTGPU_OK) {
        memcpy(d->direct_bar_challenge, challenge, sizeof challenge);
        d->direct_bar_challenge_issued = true;
        memcpy(out_challenge, challenge, sizeof challenge);
    }
    pthread_mutex_unlock(&d->lock);
    return st;
}

extgpu_status extgpu_direct_bar_authorize(extgpu_device *d, const uint64_t signature[8])
{
    if (!d || !signature) return EXTGPU_E_ARGUMENT;
    pthread_mutex_lock(&d->lock);
    extgpu_status st = ready(d, 0, false);
    if (st == EXTGPU_OK &&
        (d->claimed || !d->direct_bar_challenge_issued || d->direct_bar_authorized))
        st = error(d, EXTGPU_E_BUSY, 0, "DirectBAR.authorize_state");
    if (st == EXTGPU_OK)
        st = rpc(d, UINT32_C(0x7e10), signature, 8, NULL, 0, "DirectBAR.authorize");
    if (st == EXTGPU_OK) d->direct_bar_authorized = true;
    pthread_mutex_unlock(&d->lock);
    return st;
}

extgpu_status extgpu_direct_bar_retire(extgpu_device *d, const uint64_t signature[8])
{
    if (!d || !signature) return EXTGPU_E_ARGUMENT;
    pthread_mutex_lock(&d->lock);
    extgpu_status st = ready(d, 0, true);
    if (st == EXTGPU_OK &&
        (!d->direct_bar_authorized || d->direct_bar_retired || d->bar_mask))
        st = error(d, EXTGPU_E_BUSY, 0, "DirectBAR.retire_state");
    if (st == EXTGPU_OK)
        st = rpc(d, UINT32_C(0x7e11), signature, 8, NULL, 0, "DirectBAR.retire");
    if (st == EXTGPU_OK) d->direct_bar_retired = true;
    pthread_mutex_unlock(&d->lock);
    return st;
}

extgpu_status extgpu_bar_map(extgpu_device *d, uint32_t bar, extgpu_mapping **out)
{
    return extgpu_bar_map_with_cache_policy(d, bar, EXTGPU_BAR_CACHE_AUTOMATIC, out);
}
extgpu_status extgpu_bar_map_with_cache_policy(extgpu_device *d, uint32_t bar,
                                              extgpu_bar_cache_policy policy,
                                              extgpu_mapping **out)
{
    if (!d || bar >= 6 || !out) return EXTGPU_E_ARGUMENT;
    *out = NULL;
    if (policy > EXTGPU_BAR_CACHE_WRITE_COMBINE) return EXTGPU_E_ARGUMENT;
    extgpu_mapping *m = calloc(1, sizeof(*m));
    if (!m) return EXTGPU_E_MEMORY;
    pthread_mutex_lock(&d->lock);
    extgpu_bar_info info;
    extgpu_status st = d->direct_bar_retired ?
        error(d, EXTGPU_E_UNSAFE, 0, "DirectBAR.export_after_retire") :
        bar_info(d, bar, &info);
    if (st == EXTGPU_OK && (d->bar_mask & (1u << bar))) st = EXTGPU_E_BUSY;
    if (st == EXTGPU_OK && (info.type & 1u)) st = error(d, EXTGPU_E_UNSUPPORTED, 0, "MapBAR.io_port_aperture");
    if (st == EXTGPU_OK && policy == EXTGPU_BAR_CACHE_WRITE_COMBINE && !(info.type & 0x08u))
        st = error(d, EXTGPU_E_UNSUPPORTED, 0, "MapBAR.non_prefetchable_wc");
    if (st == EXTGPU_OK) {
        uint32_t options = kIOMapAnywhere;
        if (policy == EXTGPU_BAR_CACHE_INHIBIT) options |= kIOMapInhibitCache;
        else if (policy == EXTGPU_BAR_CACHE_WRITE_COMBINE ||
                 (policy == EXTGPU_BAR_CACHE_AUTOMATIC && (info.type & 0x08u)))
            options |= kIOMapWriteCombineCache;
        if ((d->bar_cache_attempted_mask & (1u << bar)) && d->bar_cache_options[bar] != options)
            st = error(d, EXTGPU_E_BUSY, 0, "MapBAR.cache_policy_changed");
        if (st != EXTGPU_OK) {
            pthread_mutex_unlock(&d->lock);
            free(m);
            return st;
        }
        /* Cache requests can affect a shared named memory entry even when an
         * export fails. Keep history for this device handle, not just live
         * user mappings; neither unmap nor FLR proves all aliases revoked. */
        d->bar_cache_attempted_mask |= 1u << bar;
        d->bar_cache_options[bar] = options;
        kern_return_t kr = map_memory(d, EXTGPU_MEMORY_BAR0 + bar, &m->address, &m->size, options);
        if (kr != KERN_SUCCESS && !m->address) st = error(d, native_status(kr), kr, "MapBAR");
        else {
            m->device = d; m->bar = bar; m->generation = d->protocol.generation;
            d->bar_mask |= 1u << bar; *out = m;
            if (kr != KERN_SUCCESS || !m->address || m->size < info.size) st = error(d, EXTGPU_E_UNSAFE, kr, "MapBAR.short_mapping_retained");
        }
    }
    pthread_mutex_unlock(&d->lock);
    if (!*out) free(m);
    return st;
}
void *extgpu_mapping_address(const extgpu_mapping *m) { return m ? (void *)(uintptr_t)m->address : NULL; }
uint64_t extgpu_mapping_size(const extgpu_mapping *m) { return m ? m->size : 0; }
uint64_t extgpu_mapping_generation(const extgpu_mapping *m) { return m ? m->generation : 0; }
extgpu_status extgpu_mapping_unmap(extgpu_mapping **ptr)
{
    if (!ptr || !*ptr) return EXTGPU_E_ARGUMENT;
    extgpu_mapping *m = *ptr; extgpu_device *d = m->device;
    pthread_mutex_lock(&d->lock);
    extgpu_status st = d->cold_attempted ? error(d, EXTGPU_E_UNSAFE, 0, "Cold.unmap_denied") :
        (m->generation == d->protocol.generation ? EXTGPU_OK : EXTGPU_E_STALE);
    if (st == EXTGPU_OK) {
        kern_return_t kr = unmap_memory(d, EXTGPU_MEMORY_BAR0 + m->bar, m->address);
        st = error(d, native_status(kr), kr, "UnmapBAR");
        if (st == EXTGPU_OK) d->bar_mask &= ~(1u << m->bar);
    }
    pthread_mutex_unlock(&d->lock);
    if (st == EXTGPU_OK) { free(m); *ptr = NULL; }
    return st;
}
extgpu_status extgpu_surprise_removal_unmap(extgpu_mapping **ptr)
{
    if (!ptr || !*ptr) return EXTGPU_E_ARGUMENT;
    extgpu_mapping *m = *ptr;
    extgpu_device *d = m->device;
    pthread_mutex_lock(&d->lock);
    extgpu_status st = EXTGPU_E_UNSAFE;
    if (d->surprise_requested && d->broker_bound && m->generation == d->cold_key[0]) {
        d->surprise_detaching = true;
        const kern_return_t kr = unmap_detached_memory(d, EXTGPU_MEMORY_BAR0 + m->bar, m->address, m->size);
        st = error(d, native_status(kr), kr, "Surprise.unmap_bar");
        if (st == EXTGPU_OK) d->bar_mask &= ~(1u << m->bar);
    }
    pthread_mutex_unlock(&d->lock);
    if (st == EXTGPU_OK) { free(m); *ptr = NULL; }
    return st;
}

static void kick_mmio_window(io_connect_t connection, struct openpci_mmio_window *window,
                             uint64_t generation, uint64_t posted)
{
    const uint64_t words[] = {generation, posted};
    uint32_t none = 0;
    IOConnectCallScalarMethod(connection, EXTGPU_SELECTOR_MMIO_WINDOW, words, 2, NULL, &none);
    // Only an unclaimed ticket can be marked safe to retry.
    openpci_mmio_change(window, posted, openpci_mmio_ticket(posted) | OPENPCI_MMIO_ABORTED);
}

static void *run_mmio_worker(void *opaque)
{
    struct mmio_worker *worker = opaque;
    for (;;) {
        dispatch_semaphore_wait(worker->wake, DISPATCH_TIME_FOREVER);
        if (__atomic_load_n(&worker->stop, __ATOMIC_ACQUIRE)) return NULL;
        const uint64_t posted = __atomic_exchange_n(&worker->pending, 0, __ATOMIC_ACQ_REL);
        if (posted && openpci_mmio_state(worker->window) == posted)
            kick_mmio_window(worker->connection, worker->window, worker->generation, posted);
    }
}

static void start_mmio_worker(extgpu_device *d)
{
    const char *enabled = getenv("OPENPCI_MMIO_DEDICATED_WORKER");
    if (enabled && !strcmp(enabled, "0")) return;

    struct mmio_worker *worker = &d->mmio_worker;
    worker->wake = dispatch_semaphore_create(0);
    if (!worker->wake) return;
    worker->connection = d->connection;
    worker->window = d->mmio_window;
    worker->generation = d->mmio_generation;
    if (pthread_create(&worker->thread, NULL, run_mmio_worker, worker)) {
        dispatch_release(worker->wake);
        worker->wake = NULL;
    }
}

static void enable_mmio_window(extgpu_device *d)
{
    d->mmio_window_attempted = true;
#ifdef EXTGPU_TESTING
    if (d->test) return;
#endif
    const char *enabled = getenv("OPENPCI_MMIO_WINDOW");
    if (!enabled || strcmp(enabled, "1") || !(d->protocol.capabilities & EXTGPU_CAP_MMIO_WINDOW)) return;

    uint64_t address = 0, bytes = 0;
    kern_return_t kr = map_memory(d, EXTGPU_MEMORY_MMIO_WINDOW, &address, &bytes, kIOMapAnywhere);
    struct openpci_mmio_window *window = (void *)(uintptr_t)address;
    if (kr != KERN_SUCCESS || !window || bytes < OPENPCI_MMIO_WINDOW_BYTES ||
        address % _Alignof(struct openpci_mmio_window) ||
        __atomic_load_n(&window->generation, __ATOMIC_ACQUIRE) != d->protocol.generation ||
        openpci_mmio_state(window) != OPENPCI_MMIO_OFFLINE) {
        if (kr == KERN_SUCCESS && address) unmap_memory(d, EXTGPU_MEMORY_MMIO_WINDOW, address);
        return;
    }
    d->mmio_generation = d->protocol.generation;
    d->mmio_window = window;
    start_mmio_worker(d);
    if (!d->mmio_worker.wake) {
        d->mmio_queue = dispatch_queue_create("org.openpci.mmio", DISPATCH_QUEUE_SERIAL);
        if (!d->mmio_queue) {
            unmap_memory(d, EXTGPU_MEMORY_MMIO_WINDOW, address);
            d->mmio_window = NULL;
            return;
        }
    }
    const char *word_writes = getenv("OPENPCI_MMIO_WINDOW_WORD_WRITES");
    d->mmio_window_word_writes = (d->protocol.capabilities & EXTGPU_CAP_MMIO_WINDOW_WORD_WRITES) &&
        (!word_writes || strcmp(word_writes, "0"));
}

static bool mmio_window_in_burst(const extgpu_device *d)
{
    /* The dedicated worker can reopen the window across longer gaps. */
    const uint64_t gap_limit = d->mmio_worker.wake ? UINT64_C(100000) : UINT64_C(10000);
    return d->mmio_last_completion_ns &&
        clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - d->mmio_last_completion_ns <= gap_limit;
}

// True means the window returned a completion or an uncertain result. Both
// forbid a duplicate MMIO, even for reads (which may have side effects).
static bool mmio_through_window(extgpu_device *d, const uint64_t in[4], uint64_t *value,
                                enum openpci_mmio_operation operation, const uint64_t *words,
                                const char *op, extgpu_status *status)
{
    struct openpci_mmio_window *window = d->mmio_window;
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        const uint64_t old = openpci_mmio_state(window);
        const unsigned phase = openpci_mmio_phase(old);
        if (phase != OPENPCI_MMIO_OFFLINE && phase != OPENPCI_MMIO_READY) return false;
        const uint64_t ticket = openpci_mmio_ticket(old) + 8;
        if (!ticket) return false;
        const bool cold = phase == OPENPCI_MMIO_OFFLINE;
        if (cold && !mmio_window_in_burst(d)) {
            ++d->mmio_idle_scalar;
            return false;
        }
        const uint64_t posted = ticket | (cold ? OPENPCI_MMIO_START : OPENPCI_MMIO_REQUEST);
        __atomic_store_n(&window->index, in[0], __ATOMIC_RELAXED);
        __atomic_store_n(&window->offset, in[1], __ATOMIC_RELAXED);
        __atomic_store_n(&window->width, in[2], __ATOMIC_RELAXED);
        __atomic_store_n(&window->input, in[3], __ATOMIC_RELAXED);
        __atomic_store_n(&window->write, operation, __ATOMIC_RELAXED);
        if (operation == OPENPCI_MMIO_BAR_WORD_WRITE) {
            for (uint64_t word = 0; word < in[2]; ++word)
                __atomic_store_n(&window->words[word], words[word], __ATOMIC_RELAXED);
        }
        if (!openpci_mmio_change(window, old, posted)) continue;

        if (cold) {
            struct mmio_worker *worker = &d->mmio_worker;
            if (worker->wake) {
                /* A newer ticket can replace work that timed out before the
                 * driver claimed it. The DEXT validates the exact ticket. */
                __atomic_store_n(&worker->pending, posted, __ATOMIC_RELEASE);
                dispatch_semaphore_signal(worker->wake);
            } else {
                const io_connect_t connection = d->connection;
                const uint64_t generation = d->mmio_generation;
                dispatch_async(d->mmio_queue, ^{ kick_mmio_window(connection, window, generation, posted); });
            }
        }

        const uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        for (;;) {
            uint64_t state = openpci_mmio_state(window);
            const unsigned current = openpci_mmio_phase(state);
            if (openpci_mmio_ticket(state) != ticket) break;
            if (current == OPENPCI_MMIO_ABORTED) {
                if (!openpci_mmio_change(window, state, ticket | OPENPCI_MMIO_OFFLINE)) break;
                return false;
            }
            if (current == OPENPCI_MMIO_DONE || current == OPENPCI_MMIO_DONE_OFFLINE) {
                const uint64_t output = __atomic_load_n(&window->output, __ATOMIC_RELAXED);
                const kern_return_t result = (kern_return_t)__atomic_load_n(&window->status, __ATOMIC_RELAXED);
                const unsigned next = current == OPENPCI_MMIO_DONE ? OPENPCI_MMIO_READY : OPENPCI_MMIO_OFFLINE;
                if (!openpci_mmio_change(window, state, ticket | next)) continue;
                if (cold) ++d->mmio_window_cold; else ++d->mmio_window_warm;
                if (operation == OPENPCI_MMIO_BAR_WORD_WRITE) ++d->mmio_window_word_replies;
                if (operation == OPENPCI_MMIO_SCALAR_READ && result == KERN_SUCCESS) *value = output;
                *status = error(d, native_status(result), result, op);
                return true;
            }
            const uint64_t elapsed = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start;
            if (elapsed >= UINT64_C(100000000)) {
                if (state != posted) break;
                if (openpci_mmio_change(window, posted, ticket | OPENPCI_MMIO_OFFLINE)) return false;
                continue;
            }
            if (openpci_mmio_should_spin(current, operation, elapsed)) openpci_mmio_pause();
            else nanosleep(&(const struct timespec){.tv_nsec = 10000}, NULL);
        }
        d->mmio_window_failed = true;
        *status = error(d, EXTGPU_E_UNSAFE, kIOReturnNotReady, "MMIOWindow.completion_uncertain");
        return true;
    }
    return false;
}

static bool mmio_window_is_current(extgpu_device *d)
{
    if (!d->mmio_window_attempted) enable_mmio_window(d);
    return d->mmio_window && d->mmio_generation == d->protocol.generation;
}

static void enable_mmio_ring(extgpu_device *d)
{
    d->mmio_ring_attempted = true;
    const char *enabled = getenv("OPENPCI_MMIO_RING");
    const char *event = getenv("OPENPCI_MMIO_RING_EVENT");
    const bool event_only = event && !strcmp(event, "1") &&
        (d->protocol.capabilities & EXTGPU_CAP_MMIO_RING_EVENT);
    const char *poll = getenv("OPENPCI_MMIO_RING_POLL_US");
    char *end = NULL;
    const uint64_t idle_us = poll ? strtoul(poll, &end, 10) : 0;
    const bool extended = !event_only && end && !*end && idle_us >= OPENPCI_MMIO_RING_POLL_MIN_US &&
        idle_us <= OPENPCI_MMIO_RING_POLL_MAX_US &&
        (d->protocol.capabilities & EXTGPU_CAP_MMIO_RING_POLL);
    if (!event_only && !extended &&
        (!enabled || strcmp(enabled, "1") || !(d->protocol.capabilities & EXTGPU_CAP_MMIO_RING)))
        return;
    uint64_t address = 0, bytes = 0;
    const kern_return_t result = map_memory(d, EXTGPU_MEMORY_MMIO_RING, &address, &bytes, kIOMapAnywhere);
    struct openpci_mmio_ring *ring = (void *)(uintptr_t)address;
    if (result != KERN_SUCCESS || !address || bytes < sizeof(*ring) ||
        address % _Alignof(struct openpci_mmio_ring) ||
        __atomic_load_n(&ring->generation, __ATOMIC_ACQUIRE) != d->protocol.generation ||
        __atomic_load_n(&ring->published, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&ring->completed, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&ring->active, __ATOMIC_ACQUIRE)) {
        if (result == KERN_SUCCESS && address) unmap_memory(d, EXTGPU_MEMORY_MMIO_RING, address);
        return;
    }
    d->mmio_ring_poll_us = extended ? idle_us : 0;
    if (event_only) d->mmio_ring_flags = OPENPCI_MMIO_RING_EVENT_FLAG;
    d->mmio_ring_active_publish = (d->protocol.capabilities & EXTGPU_CAP_MMIO_RING_ADAPTIVE) != 0;
    const char *adaptive = getenv("OPENPCI_MMIO_RING_ADAPTIVE");
    if (extended && d->mmio_ring_active_publish && adaptive && !strcmp(adaptive, "1"))
        d->mmio_ring_flags |= OPENPCI_MMIO_RING_ADAPTIVE_FLAG;
    const char *irq_arm = getenv("OPENPCI_MMIO_RING_IRQ_ARM");
    if (extended && (d->protocol.capabilities & EXTGPU_CAP_MMIO_RING_IRQ_ARM) && irq_arm && !strcmp(irq_arm, "1"))
        d->mmio_ring_flags |= OPENPCI_MMIO_RING_IRQ_ARM_FLAG;
    d->mmio_ring = ring;
}

/* All slots for one instruction are committed together. A failed or lost
 * reply after publication cannot justify replaying any of its MMIO. */
static bool mmio_through_ring(extgpu_device *d, enum openpci_mmio_ring_op operation,
                             uint64_t index, uint64_t offset, uint64_t width, uint32_t count,
                             uint64_t *values, const char *name, extgpu_status *status)
{
    if (!d->mmio_ring_attempted) enable_mmio_ring(d);
    struct openpci_mmio_ring *ring = d->mmio_ring;
    if (!ring) return false;
    if (!d->mmio_ring_failed &&
        __atomic_load_n(&ring->generation, __ATOMIC_ACQUIRE) != d->protocol.generation)
        return false;
    const uint64_t begin = d->mmio_ring_sequence;
    const uint64_t end = begin + count;
    if (d->mmio_ring_failed || end < begin ||
        __atomic_load_n(&ring->active, __ATOMIC_ACQUIRE) == OPENPCI_RING_RETIRED)
        goto uncertain;

    const bool read = operation == OPENPCI_RING_READ || operation == OPENPCI_RING_READ_WORDS;
    for (uint32_t i = 0; i < count; ++i) {
        struct openpci_mmio_ring_slot *slot = openpci_mmio_ring_slot_at(ring, begin + i);
        __atomic_store_n(&slot->operation, operation, __ATOMIC_RELAXED);
        __atomic_store_n(&slot->index, index, __ATOMIC_RELAXED);
        __atomic_store_n(&slot->offset, offset + i * width, __ATOMIC_RELAXED);
        __atomic_store_n(&slot->width, width, __ATOMIC_RELAXED);
        __atomic_store_n(&slot->input, read ? 0 : values[i], __ATOMIC_RELAXED);
    }

    const bool kick = d->mmio_ring_active_publish ? openpci_mmio_ring_publish_active(ring, end)
                                                  : openpci_mmio_ring_publish(ring, end);
    if (kick) {
        const uint64_t request[] = {d->protocol.generation, end, d->mmio_ring_poll_us, d->mmio_ring_flags};
        const uint32_t fields = d->mmio_ring_flags ? 4 : (d->mmio_ring_poll_us ? 3 : 2);
        ++d->mmio_ring_kicks;
        if (rpc(d, EXTGPU_SELECTOR_MMIO_RING_KICK, request, fields, NULL, 0, "MMIORing.kick") != EXTGPU_OK &&
            __atomic_load_n(&ring->completed, __ATOMIC_ACQUIRE) != end)
            goto uncertain;
    }
    if (__atomic_load_n(&ring->completed, __ATOMIC_ACQUIRE) != end) {
        if (d->mmio_ring_flags & OPENPCI_MMIO_RING_EVENT_FLAG) goto uncertain;
        const uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        unsigned polls = 0;
        do {
            /* Amortize the clock read across shared-memory completion checks. */
            if (++polls == 8) {
                polls = 0;
                const uint64_t elapsed = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start;
                if (elapsed >= UINT64_C(100000000) ||
                    __atomic_load_n(&ring->active, __ATOMIC_ACQUIRE) == OPENPCI_RING_RETIRED)
                    goto uncertain;
                if (elapsed >= UINT64_C(40000)) {
                    nanosleep(&(const struct timespec){.tv_nsec = 10000}, NULL);
                    continue;
                }
            }
            openpci_mmio_pause();
        } while (__atomic_load_n(&ring->completed, __ATOMIC_ACQUIRE) != end);
    }
    const kern_return_t result = (kern_return_t)__atomic_load_n(&ring->status, __ATOMIC_RELAXED);
    if (read && result == KERN_SUCCESS)
        for (uint32_t i = 0; i < count; ++i)
            values[i] = __atomic_load_n(&openpci_mmio_ring_slot_at(ring, begin + i)->output, __ATOMIC_RELAXED);
    d->mmio_ring_sequence = end;
    ++d->mmio_ring_batches;
    d->mmio_ring_entries += count;
    *status = error(d, native_status(result), result, name);
    return true;

uncertain:
    d->mmio_ring_failed = true;
    *status = error(d, EXTGPU_E_UNSAFE, kIOReturnNotReady, "MMIORing.completion_uncertain");
    return true;
}

static extgpu_status mmio(extgpu_device *d, uint32_t index, uint64_t off, uint32_t width, uint64_t *value, bool write)
{
    if (!d || index > UINT8_MAX || !value || !(width == 1 || width == 2 || width == 4 || width == 8) || off % width || add_overflow(off, width)) return EXTGPU_E_ARGUMENT;
    pthread_mutex_lock(&d->lock);
    extgpu_status st = ready(d, EXTGPU_CAP_BAR_MAPPING, true);
    uint64_t in[4] = {index, off, width, *value};
    if (st == EXTGPU_OK) {
        const char *op = write ? "MMIOWrite" : "MMIORead";
        if (mmio_through_ring(d, write ? OPENPCI_RING_WRITE : OPENPCI_RING_READ,
                              index, off, width, 1, value, op, &st)) {
            pthread_mutex_unlock(&d->lock);
            return st;
        }
        const bool window = mmio_window_is_current(d);
        const enum openpci_mmio_operation operation = write ? OPENPCI_MMIO_SCALAR_WRITE : OPENPCI_MMIO_SCALAR_READ;
        if (d->mmio_window_failed) st = error(d, EXTGPU_E_UNSAFE, kIOReturnNotReady, "MMIOWindow.completion_uncertain");
        else if (!window || !mmio_through_window(d, in, value, operation, NULL, op, &st)) {
            if (d->mmio_window) ++d->mmio_window_fallbacks;
            st = rpc(d, write ? EXTGPU_SELECTOR_MMIO_WRITE : EXTGPU_SELECTOR_MMIO_READ,
                     in, write ? 4 : 3, write ? NULL : value, write ? 0 : 1, op);
        }
        if (window) d->mmio_last_completion_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    }
    pthread_mutex_unlock(&d->lock); return st;
}
extgpu_status extgpu_mmio_read(extgpu_device *d, uint32_t i, uint64_t off, uint32_t width, uint64_t *out)
{ if (!out) return EXTGPU_E_ARGUMENT; uint64_t v = 0; extgpu_status st = mmio(d, i, off, width, &v, false); if (st == EXTGPU_OK) *out = v; return st; }
extgpu_status extgpu_mmio_write(extgpu_device *d, uint32_t i, uint64_t off, uint32_t width, uint64_t value)
{ if (width && width < 8 && value >= (UINT64_C(1) << (width * 8))) return EXTGPU_E_ARGUMENT; return mmio(d, i, off, width, &value, true); }

extgpu_status extgpu_mmio_write_grouped(extgpu_device *d, uint32_t index, uint64_t off,
                                     uint32_t width, uint32_t count, const uint64_t *values)
{
    if (!d || !values || index > UINT8_MAX || count < 2 || count > EXTGPU_MMIO_GROUPED_STORES_MAX ||
        !(width == 1 || width == 2 || width == 4 || width == 8) ||
        off % width || add_overflow(off, (uint64_t)width * count))
        return EXTGPU_E_ARGUMENT;
    uint64_t input[EXTGPU_MMIO_GROUPED_STORES_MAX];
    for (uint32_t i = 0; i < count; ++i) {
        if (width < 8 && values[i] >= (UINT64_C(1) << (width * 8))) return EXTGPU_E_ARGUMENT;
        input[i] = values[i];
    }
    pthread_mutex_lock(&d->lock);
    extgpu_status st = ready(d, EXTGPU_CAP_BAR_MAPPING | EXTGPU_CAP_MMIO_RING_GROUPED_STORES, true);
    if (st == EXTGPU_OK) {
        if (d->mmio_window_failed)
            st = error(d, EXTGPU_E_UNSAFE, kIOReturnNotReady, "MMIOWindow.completion_uncertain");
        else if (!mmio_through_ring(d, OPENPCI_RING_WRITE_GROUPED, index, off, width, count,
                                    input, "MMIOGroupedWrite", &st))
            st = error(d, EXTGPU_E_UNSUPPORTED, 0, "MMIORing.not_available");
        else if (st == EXTGPU_OK) {
            ++d->mmio_grouped_requests;
            d->mmio_grouped_writes += count;
        }
    }
    pthread_mutex_unlock(&d->lock);
    return st;
}

extgpu_status extgpu_bar_words(extgpu_device *d, uint32_t index, uint64_t off,
                           extgpu_bar_words_op op, uint32_t count, uint64_t *words)
{
    if (!d || !words || index > UINT8_MAX || op > EXTGPU_BAR_WORDS_FILL || !count ||
        count > (op == EXTGPU_BAR_WORDS_FILL ? EXTGPU_BAR_WORDS_MAX_FILL : EXTGPU_BAR_WORDS_MAX) ||
        (off & 7u) || add_overflow(off, UINT64_C(8) * count))
        return EXTGPU_E_ARGUMENT;
    uint64_t request[EXTGPU_BAR_WORDS_HEADER + EXTGPU_BAR_WORDS_MAX] = {index, off, op, count};
    uint64_t response[EXTGPU_BAR_WORDS_MAX] = {0};
    uint32_t payload = op == EXTGPU_BAR_WORDS_WRITE ? count : op == EXTGPU_BAR_WORDS_FILL ? 1 : 0;
    uint32_t returned = op == EXTGPU_BAR_WORDS_READ ? count : 0;
    memcpy(request + EXTGPU_BAR_WORDS_HEADER, words, payload * sizeof(*words));
    pthread_mutex_lock(&d->lock);
    extgpu_status st = ready(d, EXTGPU_CAP_BAR_WORDS | EXTGPU_CAP_BAR_MAPPING, true);
    if (st == EXTGPU_OK) {
        const bool inline_fill = op == EXTGPU_BAR_WORDS_FILL && count <= EXTGPU_BAR_WORDS_MAX &&
            !d->mmio_ring_failed && !d->mmio_window_failed;
        uint64_t *shared_words = words;
        if (inline_fill) {
            /* Shared transports already support a bounded ordered write.
             * Leave the original one-word FILL request intact for RPC. */
            shared_words = request + EXTGPU_BAR_WORDS_HEADER;
            for (uint32_t i = 1; i < count; ++i) shared_words[i] = shared_words[0];
        }
        if ((op != EXTGPU_BAR_WORDS_FILL || inline_fill) &&
            mmio_through_ring(d, op == EXTGPU_BAR_WORDS_READ ? OPENPCI_RING_READ_WORDS : OPENPCI_RING_WRITE_WORDS,
                              index, off, 8, count, shared_words, "BARWords", &st)) {
            pthread_mutex_unlock(&d->lock);
            return st;
        }
        const bool window = (op == EXTGPU_BAR_WORDS_WRITE || inline_fill) &&
            (d->protocol.capabilities & EXTGPU_CAP_MMIO_WINDOW_WORD_WRITES) && mmio_window_is_current(d) &&
            d->mmio_window_word_writes;
        const uint64_t input[4] = {index, off, count, 0};
        if (d->mmio_ring_failed) st = error(d, EXTGPU_E_UNSAFE, kIOReturnNotReady, "MMIORing.completion_uncertain");
        else if (d->mmio_window_failed) st = error(d, EXTGPU_E_UNSAFE, kIOReturnNotReady, "MMIOWindow.completion_uncertain");
        else if (!window || !mmio_through_window(d, input, NULL, OPENPCI_MMIO_BAR_WORD_WRITE,
                                                 request + EXTGPU_BAR_WORDS_HEADER, "BARWords", &st)) {
            if (window) {
                ++d->mmio_window_fallbacks;
                ++d->mmio_window_word_fallbacks;
            }
            st = rpc(d, EXTGPU_SELECTOR_BAR_WORDS, request, EXTGPU_BAR_WORDS_HEADER + payload,
                     returned ? response : NULL, returned, "BARWords");
        }
        if (window) d->mmio_last_completion_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    }
    pthread_mutex_unlock(&d->lock);
    if (st == EXTGPU_OK && returned) memcpy(words, response, returned * sizeof(*words));
    return st;
}

static bool coverage_fits(uint32_t bits, uint64_t count, uint64_t bus, uint64_t length, uint64_t requested)
{
    if (count != 1 || length != requested || !requested || add_overflow(bus, requested - 1)) return false;
    const uint64_t limit = bits == 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
    return bus <= limit && requested - 1 <= limit - bus;
}
static bool complete_coverage(extgpu_device *d, uint64_t count, uint64_t bus, uint64_t length, uint64_t requested)
{
    return coverage_fits(d->dma_address_bits, count, bus, length, requested);
}
static void release_buffer_accounting(extgpu_buffer *b)
{
    extgpu_device *d = b->device;
    extgpu_buffer **slot = &d->buffer_list;
    while (*slot && *slot != b) slot = &(*slot)->next;
    if (*slot == b) *slot = b->next;
    --d->buffers; d->dma_bytes -= b->coverage.requested_bytes;
    if (b->allocated) d->allocated_buffer = false;
}
static void reserve_buffer(extgpu_device *d, extgpu_buffer *b, uint64_t size)
{
    b->next = d->buffer_list; d->buffer_list = b;
    ++d->buffers; d->dma_bytes += size;
}
static bool overlaps_registered_iova(extgpu_device *d, uint64_t base, uint64_t size)
{
    for (extgpu_buffer *b = d->buffer_list; b; b = b->next) {
        if (b->allocated) continue;
        uint64_t other = b->coverage.requested_iova;
        uint64_t bytes = b->coverage.requested_bytes;
        /* Every admitted interval has already passed overflow checks. Unsafe
         * reservations remain in the same list until retirement is ACKed. */
        if (base < other + bytes && other < base + size) return true;
    }
    return false;
}
static extgpu_status retire_buffer(extgpu_buffer *b)
{
    extgpu_device *d = b->device;
    if (b->vm_window)
        return error(d, EXTGPU_E_UNSAFE, 0, "WarmVM.window_requires_warm_release");
    if (d->warm_vm_pending)
        return error(d, EXTGPU_E_UNSAFE, 0, "WarmVM.ordinary_unregister_denied");
    if (d->cold_attempted) {
        extgpu_status st = EXTGPU_OK;
        if (!d->cold_acknowledged || !d->cold_confirmed || !d->broker_bound || !d->broker_opened || d->cold_finished || b->allocated ||
            b->coverage.generation != d->cold_key[0])
            st = error(d, EXTGPU_E_UNSAFE, 0, "Cold.retire_requires_confirmation");
        if (st == EXTGPU_OK) {
            uint64_t in[EXTGPU_COLD_RETIRE_INPUT_COUNT], reply[EXTGPU_COLD_RETIRE_OUTPUT_COUNT] = {0};
            memcpy(in, d->cold_key, sizeof(d->cold_key));
            in[5] = b->coverage.requested_iova;
            in[6] = b->coverage.requested_bytes;
            in[7] = EXTGPU_COLD_CONFIRM_REQUIRED;
            st = broker_rpc(d, EXTGPU_SELECTOR_COLD_RETIRE, in, EXTGPU_COLD_RETIRE_INPUT_COUNT,
                     reply, EXTGPU_COLD_RETIRE_OUTPUT_COUNT, "Cold.retire");
            if (st == EXTGPU_OK && (!cold_key_matches(d, reply) || reply[5] != in[5] ||
                reply[6] != in[6] || reply[7] != 0 ||
                reply[8] != EXTGPU_COLD_STATE_SUCCESS_TOMBSTONE || !reply[9]))
                st = error(d, EXTGPU_E_PROTOCOL, 0, "Cold.retire_reply");
        }
        if (st != EXTGPU_OK) {
            b->unsafe = true;
            return cold_unacknowledged(d, "Cold.retire_unacknowledged");
        }
        b->retired = true;
        release_buffer_accounting(b);
        return EXTGPU_OK;
    }
    if (b->coverage.generation != d->protocol.generation) return error(d, EXTGPU_E_STALE, 0, "DMA.generation");
    if (b->allocated && b->address) {
        kern_return_t kr = unmap_memory(d, EXTGPU_MEMORY_DMA_BUFFER, (uintptr_t)b->address);
        if (kr != KERN_SUCCESS) { b->unsafe = true; return error(d, EXTGPU_E_UNSAFE, kr, "DMA.unmap_retained"); }
        b->address = NULL; b->mapped_size = 0;
    }
    uint64_t iova = b->coverage.requested_iova;
    extgpu_status st = rpc(d, b->allocated ? EXTGPU_SELECTOR_FREE_DMA_BUFFER : EXTGPU_SELECTOR_UNREGISTER_DMA_REGION,
                           b->allocated ? NULL : &iova, b->allocated ? 0 : 1, NULL, 0, "DMA.retire");
    if (st != EXTGPU_OK) { b->unsafe = true; return error(d, EXTGPU_E_UNSAFE, d->error.native_code, "DMA.retire_unacknowledged"); }
    b->retired = true; release_buffer_accounting(b);
    return EXTGPU_OK;
}
static extgpu_status dma_capacity(extgpu_device *d, uint64_t size)
{
    if (d->dma_budget_uncertain) return error(d, EXTGPU_E_UNSAFE, 0, "DMA.budget_uncertain");
    if (!size || d->buffers >= d->protocol.maximum_dma_regions || size > d->protocol.maximum_dma_bytes ||
        d->dma_bytes > d->protocol.maximum_dma_bytes - size) return error(d, EXTGPU_E_CAPACITY, 0, "DMA.capacity");
    return EXTGPU_OK;
}
extgpu_status extgpu_device_set_dma_budget(extgpu_device *d, uint64_t bytes)
{
    if (!d || !bytes) return EXTGPU_E_ARGUMENT;
    pthread_mutex_lock(&d->lock);
    extgpu_status st = ready(d, EXTGPU_CAP_CONFIGURABLE_DMA_BUDGET, true);
    if (st == EXTGPU_OK && d->buffers)
        st = error(d, EXTGPU_E_BUSY, 0, "DMA.budget_live_buffers");
    if (st == EXTGPU_OK) {
        const uint64_t request[EXTGPU_DMA_BUDGET_WORDS] = {d->protocol.generation, bytes};
        uint64_t reply[EXTGPU_DMA_BUDGET_WORDS] = {0};
        st = rpc(d, EXTGPU_SELECTOR_SET_DMA_BUDGET, request, EXTGPU_DMA_BUDGET_WORDS,
                 reply, EXTGPU_DMA_BUDGET_WORDS, "SetDMABudget");
        if (st == EXTGPU_OK && !memcmp(request, reply, sizeof request)) {
            d->protocol.maximum_dma_bytes = bytes;
            d->dma_budget_uncertain = false;
        } else if (st == EXTGPU_OK || st == EXTGPU_E_IO || st == EXTGPU_E_PROTOCOL) {
            d->dma_budget_uncertain = true;
            st = error(d, EXTGPU_E_UNSAFE, d->error.native_code, "DMA.budget_unacknowledged");
        }
    }
    pthread_mutex_unlock(&d->lock);
    return st;
}
static extgpu_status read_vm_dma_segments(extgpu_device *d, extgpu_buffer *buffer,
                                          const uint64_t summary[3], uint64_t page, uint32_t address_bits)
{
    const uint64_t size = buffer->coverage.requested_bytes;
    if (!summary[0] || summary[0] > EXTGPU_VM_DMA_SEGMENT_LIMIT || summary[2] != size)
        return error(d, EXTGPU_E_COVERAGE, 0, "VM_DMA.summary");
    uint64_t offset = 0;
    for (uint64_t i = 0; i < summary[0]; ++i) {
        const uint64_t request[] = {buffer->coverage.generation, buffer->coverage.requested_iova, i};
        uint64_t reply[2] = {0};
        extgpu_status st = rpc(d, EXTGPU_SELECTOR_GET_VM_DMA_SEGMENT, request, 3, reply, 2, "GetVMDMASegment");
        if (st != EXTGPU_OK) return st;
        const uint64_t bus = reply[0], bytes = reply[1];
        if ((!i && bus != summary[1]) || bytes > size - offset || bus % page || bytes % page ||
            add_overflow(bus, bytes) || !coverage_fits(address_bits, 1, bus, bytes, bytes))
            return error(d, EXTGPU_E_COVERAGE, 0, "VM_DMA.segment");
        for (uint64_t j = 0; j < i; ++j) {
            const extgpu_vm_dma_segment *other = &buffer->vm_segments[j];
            if (bus < other->bus_address + other->bytes && other->bus_address < bus + bytes)
                return error(d, EXTGPU_E_COVERAGE, 0, "VM_DMA.overlapping_segments");
        }
        buffer->vm_segments[i] = (extgpu_vm_dma_segment){offset, bus, bytes};
        offset += bytes;
    }
    return offset == size ? EXTGPU_OK : error(d, EXTGPU_E_COVERAGE, 0, "VM_DMA.incomplete");
}

static extgpu_status register_dma(extgpu_device *d, uint64_t iova, void *host, uint64_t size,
                                 extgpu_buffer **out, extgpu_dma_coverage *coverage,
                                 bool vm_window, bool segmented, bool dynamic,
                                 uint32_t address_bits, extgpu_vm_dma_segment *segments)
{
    if (!d || !out || !coverage || !host || !size || size > SIZE_MAX || add_overflow(iova, size) || add_overflow((uintptr_t)host, size)) return EXTGPU_E_ARGUMENT;
    *out = NULL; memset(coverage, 0, sizeof(*coverage));
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0 || (uintptr_t)host % (uint64_t)page || size % (uint64_t)page || iova % (uint64_t)page) return EXTGPU_E_ARGUMENT;
    const size_t table_bytes = segmented ? EXTGPU_VM_DMA_SEGMENT_LIMIT * sizeof(extgpu_vm_dma_segment) : 0;
    extgpu_buffer *b = calloc(1, sizeof(*b) + table_bytes); if (!b) return EXTGPU_E_MEMORY;
    pthread_mutex_lock(&d->lock);
    extgpu_status st = ready(d, EXTGPU_CAP_DMA_REGISTER | (segmented ? EXTGPU_CAP_VM_DMA_SEGMENTS : 0), true);
    if (st == EXTGPU_OK && address_bits) st = ready(d, EXTGPU_CAP_VM_DMA_CONSTRAINED, true);
    if (st == EXTGPU_OK && dynamic != (d->vm_dynamic_window != NULL))
        st = error(d, EXTGPU_E_UNSUPPORTED, 0, "VM_DMA.registration_mode");
    if (st == EXTGPU_OK && vm_window && resources(d))
        st = error(d, EXTGPU_E_BUSY, 0, "WarmVM.requires_unused_claim");
    if (st == EXTGPU_OK) st = dma_capacity(d, size);
    if (st == EXTGPU_OK && overlaps_registered_iova(d, iova, size))
        st = error(d, EXTGPU_E_BUSY, 0, "RegisterDMA.overlapping_IOVA");
    if (st == EXTGPU_OK) {
        b->device = d; b->address = host; b->vm_window = vm_window; b->vm_segmented = segmented;
        b->vm_dynamic = dynamic;
        b->coverage = (extgpu_dma_coverage){.generation=d->protocol.generation, .requested_iova=iova, .requested_bytes=size};
        reserve_buffer(d, b, size);
        const uint64_t in[] = {iova, (uintptr_t)host, size, address_bits, segmented};
        uint64_t values[3] = {0};
        const uint32_t selector = address_bits ? EXTGPU_SELECTOR_REGISTER_VM_DMA_CONSTRAINED :
            dynamic ? EXTGPU_SELECTOR_REGISTER_VM_DYNAMIC_DMA :
            segmented ? EXTGPU_SELECTOR_REGISTER_VM_DMA : EXTGPU_SELECTOR_REGISTER_DMA_REGION;
        st = rpc(d, selector, in, address_bits ? 5 : 3, values, 3, "RegisterDMA");
        const bool duplicate = st != EXTGPU_OK && d->error.native_code == kIOReturnStillOpen;
        const bool preflight_failure = dynamic && st != EXTGPU_OK &&
            (d->error.native_code == kIOReturnNoSpace || d->error.native_code == kIOReturnNoMemory ||
             d->error.native_code == kIOReturnNotAligned);
        const uint32_t effective_bits = address_bits ? address_bits : d->dma_address_bits;
        if (st == EXTGPU_OK && segmented)
            st = read_vm_dma_segments(d, b, values, (uint64_t)page, effective_bits);
        const bool valid = st == EXTGPU_OK &&
            (segmented || coverage_fits(effective_bits, values[0], values[1], values[2], size));
        if (valid) {
            b->coverage.bus_address=values[1]; b->coverage.covered_bytes=values[2];
            b->coverage.segment_count=(uint32_t)values[0]; b->coverage.complete=1;
            if (segments) memcpy(segments, b->vm_segments, b->coverage.segment_count * sizeof(*segments));
            *out=b; *coverage=b->coverage;
        } else if (duplicate) {
            release_buffer_accounting(b);
            st = error(d, EXTGPU_E_BUSY, kIOReturnStillOpen, "RegisterDMA.driver_duplicate");
        } else if (preflight_failure) {
            release_buffer_accounting(b);
            if (d->error.native_code == kIOReturnNoSpace)
                st = error(d, EXTGPU_E_CAPACITY, kIOReturnNoSpace, "VM_DMA.driver_capacity");
        } else if (vm_window || (st != EXTGPU_OK && d->error.native_code != 0)) {
            /* Canonical StillOpen is a preflight duplicate-key rejection with
             * no mutation. Other negative replies can retain prepared pages
             * when CompleteDMA rollback failed. VM windows also require the
             * scoped warm release when positive coverage is malformed. */
            b->unsafe = true; *out = b; *coverage = b->coverage;
            st = error(d, EXTGPU_E_UNSAFE, d->error.native_code,
                       vm_window ? "WarmVM.registration_uncertain" : "RegisterDMA.negative_uncertain_reserved");
        } else {
            extgpu_status initial = st == EXTGPU_OK ? EXTGPU_E_COVERAGE : st;
            if (retire_buffer(b) != EXTGPU_OK) { *out=b; *coverage=b->coverage; st=EXTGPU_E_UNSAFE; }
            else st=error(d, initial, 0, "RegisterDMA.rolled_back");
        }
    }
    pthread_mutex_unlock(&d->lock); if (!*out) free(b); return st;
}
extgpu_status extgpu_dma_register(extgpu_device *d, uint64_t iova, void *host, uint64_t size,
                                 extgpu_buffer **out, extgpu_dma_coverage *coverage)
{
    return register_dma(d, iova, host, size, out, coverage, false, false, false, 0, NULL);
}
extgpu_status extgpu_vm_ram_register(extgpu_device *d, uint64_t iova, void *host, uint64_t size,
                                    extgpu_buffer **out, extgpu_dma_coverage *coverage)
{
    return register_dma(d, iova, host, size, out, coverage, true, false, false, 0, NULL);
}
extgpu_status extgpu_vm_ram_register_segmented(extgpu_device *d, uint64_t iova, void *host, uint64_t size,
                                    extgpu_buffer **out, extgpu_dma_coverage *coverage)
{
    return register_dma(d, iova, host, size, out, coverage, true, true, false, 0, NULL);
}

extgpu_status extgpu_vm_dma_enable(extgpu_device *d, const extgpu_buffer *window)
{
    if (!d || !window || window->device != d || !window->vm_window) return EXTGPU_E_ARGUMENT;
    pthread_mutex_lock(&d->lock);
    extgpu_status st = ready(d, EXTGPU_CAP_VM_DYNAMIC_DMA, true);
    if (st == EXTGPU_OK && (d->buffers != 1 || d->buffer_list != window || window->unsafe ||
                            !window->coverage.complete || d->irq || d->bar_mask || d->bar_cache_attempted_mask))
        st = error(d, EXTGPU_E_BUSY, 0, "VM_DMA.requires_initial_window");
    if (st == EXTGPU_OK) {
        const uint64_t request[] = {window->coverage.generation, window->coverage.requested_iova,
                                    window->coverage.requested_bytes};
        uint64_t reply[3] = {0};
        st = rpc(d, EXTGPU_SELECTOR_ENABLE_VM_DYNAMIC_DMA, request, 3, reply, 3, "EnableVMDynamicDMA");
        if (st == EXTGPU_OK && memcmp(request, reply, sizeof request))
            st = error(d, EXTGPU_E_PROTOCOL, 0, "VM_DMA.enable_reply");
        if (st == EXTGPU_OK) d->vm_dynamic_window = window;
    }
    pthread_mutex_unlock(&d->lock);
    return st;
}

extgpu_status extgpu_vm_dma_map(extgpu_device *d, uint64_t key, void *host, uint64_t size,
                                extgpu_dma_coverage *coverage)
{
    extgpu_buffer *internal = NULL;
    return register_dma(d, key, host, size, &internal, coverage, false, false, true, 0, NULL);
}

extgpu_status extgpu_vm_dma_map_constrained(extgpu_device *d, uint64_t key, void *host,
                                uint64_t size, uint32_t bits, extgpu_vm_dma_segment *segments,
                                extgpu_dma_coverage *coverage)
{
    if (bits < 32 || bits > 64) return EXTGPU_E_ARGUMENT;
    extgpu_buffer *internal = NULL;
    return register_dma(d, key, host, size, &internal, coverage, false, segments != NULL, true,
                        bits, segments);
}

extgpu_status extgpu_vm_dma_unmap(extgpu_device *d, uint64_t key)
{
    if (!d) return EXTGPU_E_ARGUMENT;
    pthread_mutex_lock(&d->lock);
    extgpu_status st = ready(d, EXTGPU_CAP_VM_DYNAMIC_DMA, true);
    if (st == EXTGPU_OK) {
        extgpu_buffer *b = d->buffer_list;
        while (b && (!b->vm_dynamic || b->coverage.requested_iova != key)) b = b->next;
        if (!b) st = error(d, EXTGPU_E_NOT_FOUND, 0, "VM_DMA.unknown_key");
        else {
            st = retire_buffer(b);
            if (st == EXTGPU_OK) free(b);
        }
    }
    pthread_mutex_unlock(&d->lock);
    return st;
}

extgpu_status extgpu_vm_ram_segments(const extgpu_buffer *b, extgpu_vm_dma_segment *out,
                                    size_t capacity, size_t *count)
{
    if (!b || !count || (!out && capacity) || !b->vm_window) return EXTGPU_E_ARGUMENT;
    extgpu_device *d = b->device;
    pthread_mutex_lock(&d->lock);
    extgpu_status st = ready(d, EXTGPU_CAP_DMA_REGISTER, true);
    if (st == EXTGPU_OK && (b->unsafe || b->retired || !b->coverage.complete ||
                            b->coverage.generation != d->protocol.generation))
        st = error(d, EXTGPU_E_UNSAFE, 0, "VM_DMA.segment_table_unavailable");
    if (st == EXTGPU_OK) {
        *count = b->coverage.segment_count;
        if (out && capacity < *count) st = error(d, EXTGPU_E_CAPACITY, 0, "VM_DMA.segment_table_capacity");
        else if (out && b->vm_segmented) memcpy(out, b->vm_segments, *count * sizeof(*out));
        else if (out) *out = (extgpu_vm_dma_segment){0, b->coverage.bus_address, b->coverage.covered_bytes};
    }
    pthread_mutex_unlock(&d->lock);
    return st;
}
extgpu_status extgpu_dma_allocate(extgpu_device *d, uint64_t size, uint64_t alignment,
                                  extgpu_buffer **out, extgpu_dma_coverage *coverage)
{
    if (!d || !size || !out || !coverage || (alignment && (alignment & (alignment-1)))) return EXTGPU_E_ARGUMENT;
    *out=NULL; memset(coverage,0,sizeof(*coverage));
    extgpu_buffer *b=calloc(1,sizeof(*b)); if (!b) return EXTGPU_E_MEMORY;
    pthread_mutex_lock(&d->lock);
    extgpu_status st=ready(d,EXTGPU_CAP_DMA_ALLOCATE,true);
    if (st == EXTGPU_OK && d->surprise_requested)
        st = error(d, EXTGPU_E_UNSUPPORTED, 0, "Surprise.allocated_buffer");
    if (st==EXTGPU_OK && d->vm_dynamic_window) st=error(d,EXTGPU_E_UNSUPPORTED,0,"VM_DMA.registration_mode");
    if (st==EXTGPU_OK) st=dma_capacity(d,size);
    if (st==EXTGPU_OK && d->allocated_buffer) st=EXTGPU_E_BUSY;
    if (st==EXTGPU_OK) {
        b->device=d; b->allocated=true;
        b->coverage=(extgpu_dma_coverage){.generation=d->protocol.generation,.requested_bytes=size};
        reserve_buffer(d,b,size); d->allocated_buffer=true;
        uint64_t in[2]={size,alignment}, values[6]={0};
        st=rpc(d,EXTGPU_SELECTOR_ALLOCATE_DMA_BUFFER,in,2,values,6,"AllocateDMA");
        uint64_t required_alignment = alignment ? alignment : UINT64_C(16384);
        if (st==EXTGPU_OK && (values[0]!=size || !complete_coverage(d,values[2],values[3],values[4],size) ||
                             (values[3] & (required_alignment-1)))) st=EXTGPU_E_COVERAGE;
        if (st==EXTGPU_OK) {
            b->coverage.bus_address=values[3]; b->coverage.covered_bytes=values[4]; b->coverage.segment_count=1; b->coverage.complete=1;
            uint64_t address=0, mapped=0;
            kern_return_t kr=map_memory(d,EXTGPU_MEMORY_DMA_BUFFER,&address,&mapped,kIOMapAnywhere);
            b->address=(void *)(uintptr_t)address; b->mapped_size=mapped;
            if (kr!=KERN_SUCCESS) st=error(d,native_status(kr),kr,"MapDMA");
            else if (!address || mapped<size) st=EXTGPU_E_COVERAGE;
        }
        if (st==EXTGPU_OK) { *out=b; *coverage=b->coverage; }
        else { extgpu_status initial=st; if (retire_buffer(b)!=EXTGPU_OK) { *out=b; *coverage=b->coverage; st=EXTGPU_E_UNSAFE; } else st=error(d,initial,0,"AllocateDMA.rolled_back"); }
    }
    pthread_mutex_unlock(&d->lock); if (!*out) free(b); return st;
}
void *extgpu_buffer_address(const extgpu_buffer *b)
{ return b && !b->unsafe && !b->device->cold_attempted && !b->device->warm_vm_pending ? b->address : NULL; }
uint64_t extgpu_buffer_size(const extgpu_buffer *b) { return b ? b->coverage.requested_bytes : 0; }
extgpu_status extgpu_buffer_coverage(const extgpu_buffer *b, extgpu_dma_coverage *out)
{ if (!b || !out) return EXTGPU_E_ARGUMENT; *out=b->coverage; return b->unsafe || b->device->cold_attempted || b->device->warm_vm_pending ? EXTGPU_E_UNSAFE : EXTGPU_OK; }

static CFTypeRef registry_property(io_registry_entry_t entry, CFStringRef name)
{
    return IORegistryEntryCreateCFProperty(entry, name, kCFAllocatorDefault, 0);
}

static bool matching_reset_companion(io_registry_entry_t audio, CFTypeRef display_hash)
{
    io_iterator_t iterator = IO_OBJECT_NULL;
    if (IORegistryEntryGetChildIterator(audio, kIOServicePlane, &iterator) != KERN_SUCCESS)
        return false;
    io_registry_entry_t child = IOIteratorNext(iterator);
    bool matches = false;
    if (child) {
        CFTypeRef role = registry_property(child, CFSTR("OpenPCIResetCompanion"));
        CFTypeRef server = registry_property(child, CFSTR("IOUserServerName"));
        CFTypeRef hash = registry_property(child, CFSTR("IOUserServerCDHash"));
        matches = IOObjectConformsTo(child, "IOUserService") && role == kCFBooleanTrue &&
            server && CFEqual(server, CFSTR(EXTGPU_DRIVER_BUNDLE_ID)) &&
            hash && CFEqual(hash, display_hash);
        if (role) CFRelease(role);
        if (server) CFRelease(server);
        if (hash) CFRelease(hash);
        IOObjectRelease(child);
        io_registry_entry_t other = IOIteratorNext(iterator);
        if (other) { IOObjectRelease(other); matches = false; }
    }
    matches = matches && IOIteratorIsValid(iterator);
    IOObjectRelease(iterator);
    return matches;
}

struct warm_reset_group {
    uint32_t vendor, display, audio, alternate_display;
    uint32_t selector, receipt_selector;
    uint64_t capability, receipt_capability;
};

static const struct warm_reset_group vega_group = {
    .vendor = 0x1002, .display = 0x687f, .audio = 0xaaf8,
    .selector = EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA, .receipt_selector = EXTGPU_SELECTOR_VEGA_RELEASE_RECEIPT,
    .capability = EXTGPU_CAP_WARM_VM_VEGA_RELEASE, .receipt_capability = EXTGPU_CAP_VEGA_RELEASE_RECEIPT,
};
static const struct warm_reset_group rtx3090_group = {
    .vendor = EXTGPU_RTX3090_VENDOR, .display = EXTGPU_RTX3090_DISPLAY, .audio = EXTGPU_RTX3090_AUDIO,
    .selector = EXTGPU_SELECTOR_WARM_RELEASE_VM_RTX3090, .receipt_selector = EXTGPU_SELECTOR_RTX3090_RELEASE_RECEIPT,
    .capability = EXTGPU_CAP_WARM_VM_RTX3090_RELEASE, .receipt_capability = EXTGPU_CAP_WARM_VM_RTX3090_RELEASE,
};
static const struct warm_reset_group polaris_group = {
    .vendor = EXTGPU_POLARIS_VENDOR, .display = EXTGPU_POLARIS_DISPLAY, .audio = EXTGPU_POLARIS_AUDIO,
    .alternate_display = EXTGPU_POLARIS_DISPLAY_2048SP,
    .selector = EXTGPU_SELECTOR_WARM_RELEASE_VM_POLARIS, .receipt_selector = EXTGPU_SELECTOR_POLARIS_RELEASE_RECEIPT,
    .capability = EXTGPU_CAP_WARM_VM_POLARIS_RELEASE, .receipt_capability = EXTGPU_CAP_WARM_VM_POLARIS_RELEASE,
};

static bool ready_group_reset_scope(extgpu_device *d, const struct warm_reset_group *group, uint64_t result[3])
{
    if (!d->pci_registry_id) return false;
#ifdef EXTGPU_TESTING
    if (d->test) {
        if (!d->test_reset_bridge || !d->test_reset_audio) return false;
        result[0] = d->test_reset_bridge;
        result[1] = d->pci_registry_id;
        result[2] = d->test_reset_audio;
        return true;
    }
#endif
    CFMutableDictionaryRef match = IORegistryEntryIDMatching(d->registry_id);
    io_registry_entry_t display = match ? IOServiceGetMatchingService(kIOMainPortDefault, match) : IO_OBJECT_NULL;
    if (!display) return false;
    CFTypeRef hash = registry_property(display, CFSTR("IOUserServerCDHash"));
    CFTypeRef server = registry_property(display, CFSTR("IOUserServerName"));
    io_registry_entry_t entry = IO_OBJECT_NULL;
    io_registry_entry_t parent = IO_OBJECT_NULL;
    io_iterator_t iterator = IO_OBJECT_NULL;
    bool valid = false, display_found = false;
    uint64_t bridge = 0, audio = 0;
    uint64_t display_pci = 0;
    unsigned members = 0;
    if (!service_matches(display) || !hash || CFGetTypeID(hash) != CFStringGetTypeID() ||
        !server || !CFEqual(server, CFSTR(EXTGPU_DRIVER_BUNDLE_ID)) ||
        IORegistryEntryGetParentEntry(display, kIOServicePlane, &entry) != KERN_SUCCESS ||
        !IOObjectConformsTo(entry, "IOPCIDevice") ||
        IORegistryEntryGetRegistryEntryID(entry, &display_pci) != KERN_SUCCESS ||
        display_pci != d->pci_registry_id ||
        IORegistryEntryGetParentEntry(entry, kIOServicePlane, &parent) != KERN_SUCCESS ||
        !IOObjectConformsTo(parent, "IOPCI2PCIBridge") ||
        IORegistryEntryGetRegistryEntryID(parent, &bridge) != KERN_SUCCESS || !bridge ||
        IORegistryEntryGetChildIterator(parent, kIOServicePlane, &iterator) != KERN_SUCCESS)
        goto done;

    for (io_registry_entry_t child; (child = IOIteratorNext(iterator)) != IO_OBJECT_NULL;) {
        uint64_t id = 0;
        uint32_t vendor = pci_property(child, CFSTR("vendor-id"));
        uint32_t device = pci_property(child, CFSTR("device-id"));
        uint32_t class_code = pci_property(child, CFSTR("class-code"));
        CFTypeRef tunneled = registry_property(child, CFSTR("IOPCITunnelled"));
        bool member_ok = IOObjectConformsTo(child, "IOPCIDevice") &&
            IORegistryEntryGetRegistryEntryID(child, &id) == KERN_SUCCESS && id &&
            tunneled == kCFBooleanTrue && vendor == group->vendor;
        if (tunneled) CFRelease(tunneled);
        bool sibling = member_ok && id != d->pci_registry_id && device == group->audio &&
            class_code == 0x040300 && matching_reset_companion(child, hash);
        IOObjectRelease(child);
        if (!member_ok || ++members > 2) goto done;
        bool display_device = device == group->display || (group->alternate_display && device == group->alternate_display);
        if (id == d->pci_registry_id && display_device && class_code == 0x030000 && !display_found)
            display_found = true;
        else if (sibling && !audio)
            audio = id;
        else goto done;
    }
    valid = members == 2 && display_found && audio && IOIteratorIsValid(iterator);
    if (valid) {
        result[0] = bridge;
        result[1] = d->pci_registry_id;
        result[2] = audio;
    }
done:
    if (iterator) IOObjectRelease(iterator);
    if (parent) IOObjectRelease(parent);
    if (entry) IOObjectRelease(entry);
    if (server) CFRelease(server);
    if (hash) CFRelease(hash);
    IOObjectRelease(display);
    return valid;
}

static extgpu_status verify_reset_group_server(extgpu_device *d, const struct warm_reset_group *group,
                                               const uint64_t scope[3])
{
    if (d->protocol.capabilities & EXTGPU_CAP_VM_GROUP_PREFLIGHT) {
        uint64_t request[EXTGPU_VM_GROUP_PREFLIGHT_WORDS] = {
            d->protocol.generation, group->selector, scope[0], scope[1], scope[2]
        };
        return rpc(d, EXTGPU_SELECTOR_VM_GROUP_PREFLIGHT, request, EXTGPU_VM_GROUP_PREFLIGHT_WORDS,
                   NULL, 0, "WarmVM.group_driver_preflight");
    }

    /* Older drivers keep their companion table process-local. Their legacy
     * topology check is only usable when there is a single matching server. */
#ifdef EXTGPU_TESTING
    if (d->test) return EXTGPU_OK;
#endif
    CFMutableDictionaryRef match = IOServiceMatching("IOUserServer");
    if (!match) return error(d, EXTGPU_E_IO, 0, "WarmVM.group_server_match");
    const void *name_key = CFSTR("IOUserServerName");
    const void *bundle_name = CFSTR(EXTGPU_DRIVER_BUNDLE_ID);
    CFDictionaryRef properties = CFDictionaryCreate(kCFAllocatorDefault, &name_key, &bundle_name, 1,
                                                      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!properties) {
        CFRelease(match);
        return error(d, EXTGPU_E_MEMORY, 0, "WarmVM.group_server_match");
    }
    CFDictionarySetValue(match, CFSTR("IOPropertyMatch"), properties);
    CFRelease(properties);
    io_iterator_t iterator = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault, match, &iterator);
    if (kr != KERN_SUCCESS) return error(d, native_status(kr), kr, "WarmVM.group_server_inventory");
    io_service_t first = IOIteratorNext(iterator);
    io_service_t other = IOIteratorNext(iterator);
    bool unique = first && !other && IOIteratorIsValid(iterator);
    if (first) IOObjectRelease(first);
    if (other) IOObjectRelease(other);
    IOObjectRelease(iterator);
    if (!unique) return error(d, EXTGPU_E_UNSUPPORTED, 0, "WarmVM.group_server_not_unique");
    return EXTGPU_OK;
}

extgpu_status extgpu_warm_release_vm_vega_preflight(extgpu_device *d)
{
    if (!d) return EXTGPU_E_ARGUMENT;
    pthread_mutex_lock(&d->lock);
    uint64_t scope[3];
    extgpu_status st = ready(d, EXTGPU_CAP_WARM_VM_VEGA_RELEASE, false);
    if (st == EXTGPU_OK && !ready_group_reset_scope(d, &vega_group, scope))
        st = error(d, EXTGPU_E_UNSUPPORTED, 0, "WarmVM.vega_domain_or_companion_missing");
    if (st == EXTGPU_OK) st = verify_reset_group_server(d, &vega_group, scope);
    pthread_mutex_unlock(&d->lock);
    return st;
}

static extgpu_status pin_reset_group(extgpu_device *d, const struct warm_reset_group *group)
{
    if (!d) return EXTGPU_E_ARGUMENT;
    pthread_mutex_lock(&d->lock);
    uint64_t scope[3];
    extgpu_status st = ready(d, group->capability, true);
    if (st == EXTGPU_OK && (resources(d) || d->bar_cache_attempted_mask))
        st = error(d, EXTGPU_E_BUSY, 0, "WarmVM.group_requires_unused_claim");
    if (st == EXTGPU_OK && !ready_group_reset_scope(d, group, scope))
        st = error(d, EXTGPU_E_UNSUPPORTED, 0, "WarmVM.group_domain_or_companion_missing");
    if (st == EXTGPU_OK) st = verify_reset_group_server(d, group, scope);
    if (st == EXTGPU_OK && d->pinned_scope_generation == d->protocol.generation &&
        (d->pinned_scope_selector != group->selector || memcmp(scope, d->pinned_scope, sizeof scope)))
        st = error(d, EXTGPU_E_UNSAFE, 0, "WarmVM.original_scope_changed");
    if (st == EXTGPU_OK) {
        memcpy(d->pinned_scope, scope, sizeof scope);
        d->pinned_scope_generation = d->protocol.generation;
        d->pinned_scope_selector = group->selector;
    }
    pthread_mutex_unlock(&d->lock);
    return st;
}

extgpu_status extgpu_warm_release_vm_vega_pin(extgpu_device *d)
{
    return pin_reset_group(d, &vega_group);
}

extgpu_status extgpu_warm_release_vm_rtx3090_preflight(extgpu_device *d)
{
    return pin_reset_group(d, &rtx3090_group);
}

extgpu_status extgpu_warm_release_vm_polaris_preflight(extgpu_device *d)
{
    return pin_reset_group(d, &polaris_group);
}

static extgpu_status warm_release_vm_locked(extgpu_device *d, extgpu_buffer **window,
                                            const struct warm_reset_group *group)
{
    extgpu_buffer *b = window ? *window : NULL;
    extgpu_status st = EXTGPU_OK;
    const uint32_t method = group ? group->selector : EXTGPU_SELECTOR_WARM_RELEASE_VM_FLR;
    const uint64_t capability = group ? group->capability : EXTGPU_CAP_WARM_VM_FLR_RELEASE;
    const bool requires_pinned_scope = group && (group != &vega_group || !window);
    const bool has_pinned_scope = group && d->pinned_scope_selector == method &&
        d->pinned_scope_generation == d->protocol.generation;
    const bool uncertain_startup = b && b->vm_window && b->unsafe && !b->coverage.complete &&
        (d->protocol.capabilities & EXTGPU_CAP_WARM_VM_STARTUP_RELEASE);
    if (d->original_closed || d->cold_attempted || d->ownership_uncertain ||
        !d->protocol_valid || !d->claimed ||
        (d->warm_vm_pending && d->warm_vm_selector != method) ||
        (b && ((b->unsafe && !uncertain_startup) || b->retired ||
               b->coverage.generation != d->protocol.generation)))
        st = error(d, EXTGPU_E_UNSAFE, 0, "WarmVM.owner_or_window_uncertain");
    else if (!(d->protocol.capabilities & capability) ||
             (!window && !(d->protocol.capabilities & EXTGPU_CAP_WARM_VM_NO_DMA_RELEASE)))
        st = error(d, EXTGPU_E_UNSUPPORTED, 0, "WarmVM.capability_absent");
    else if (requires_pinned_scope && !has_pinned_scope)
        st = error(d, EXTGPU_E_UNSUPPORTED, 0, "WarmVM.requires_original_scope");
    else if (!window && (resources(d) ||
                         (d->bar_cache_attempted_mask && !d->direct_bar_retired)))
        st = error(d, EXTGPU_E_BUSY, 0, "WarmVM.requires_no_resources");
    else if (b && (d->irq || d->bar_mask ||
                   (d->bar_cache_attempted_mask && !d->direct_bar_retired) ||
             (d->vm_dynamic_window != b && (d->buffers != 1 || d->buffer_list != b)) || b->allocated ||
             (!uncertain_startup && (b->coverage.complete != 1 || !b->coverage.segment_count ||
              (!b->vm_segmented && b->coverage.segment_count != 1) ||
              b->coverage.covered_bytes != b->coverage.requested_bytes))))
        st = error(d, EXTGPU_E_BUSY, 0, "WarmVM.requires_one_registered_window");

    bool receipt_only = false;
    if (st == EXTGPU_OK && group) {
        uint64_t live_scope[3] = {0};
        const bool exact = ready_group_reset_scope(d, group, live_scope);
        if (d->warm_vm_pending) {
            if (!exact || memcmp(live_scope, d->warm_group_scope, sizeof live_scope)) {
                receipt_only = (d->protocol.capabilities & group->receipt_capability) != 0;
                if (!receipt_only)
                    st = error(d, EXTGPU_E_UNSAFE, kIOReturnNotReady, "WarmVM.awaiting_original_scope");
            }
        } else if (has_pinned_scope && (!exact || memcmp(live_scope, d->pinned_scope, sizeof live_scope))) {
            st = error(d, EXTGPU_E_UNSAFE, kIOReturnNotReady, "WarmVM.awaiting_original_scope");
        } else if (!exact) {
            st = error(d, EXTGPU_E_UNSUPPORTED, 0, "WarmVM.vega_domain_not_exact");
        } else {
            memcpy(d->warm_group_scope, live_scope, sizeof live_scope);
        }
    }
    if (st == EXTGPU_OK) {
        uint64_t request[EXTGPU_WARM_VEGA_REQUEST_WORDS] = {
            d->protocol.generation, b ? b->coverage.requested_iova : 0,
            b ? b->coverage.requested_bytes : 0};
        uint64_t reply[EXTGPU_WARM_VEGA_REPLY_WORDS] = {0};
        unsigned request_words = group ? EXTGPU_WARM_VEGA_REQUEST_WORDS : EXTGPU_WARM_VM_REQUEST_WORDS;
        unsigned reply_words = request_words + 1;
        if (group) memcpy(request + 3, d->warm_group_scope, sizeof d->warm_group_scope);
        bool retry = d->warm_vm_pending;
        d->warm_vm_pending = true;
        d->warm_vm_selector = method;
        uint32_t selector = receipt_only ? group->receipt_selector : method;
        st = rpc(d, selector, request, request_words, reply, reply_words,
                 receipt_only ? "WarmVM.group_receipt" : "WarmVM.release");
        if (st == EXTGPU_OK && (memcmp(request, reply, request_words * sizeof(*request)) ||
                              reply[request_words] != EXTGPU_WARM_VM_CLOSED))
            st = error(d, EXTGPU_E_PROTOCOL, 0, "WarmVM.reply_mismatch");
        if (st == EXTGPU_OK) {
            if (b) {
                if (d->vm_dynamic_window == b) {
                    while (d->buffer_list && d->buffer_list != b) {
                        extgpu_buffer *completed = d->buffer_list;
                        release_buffer_accounting(completed);
                        free(completed);
                    }
                    d->vm_dynamic_window = NULL;
                }
                b->retired = true;
                release_buffer_accounting(b);
                free(b);
                *window = NULL;
            }
            d->claimed = false;
            d->protocol_valid = false;
            d->warm_vm_pending = false;
        } else if (receipt_only) {
            st = error(d, EXTGPU_E_UNSAFE, d->error.native_code, "WarmVM.awaiting_original_scope");
        } else if (!retry && d->error.native_code == kIOReturnUnsupported) {
            /* The wire contract reserves this exact reply for preflight. */
            d->warm_vm_pending = false;
        } else
            st = error(d, EXTGPU_E_UNSAFE, d->error.native_code, "WarmVM.release_unacknowledged");
    }
    return st;
}

static extgpu_status warm_release_vm(extgpu_device *d, extgpu_buffer **window, const struct warm_reset_group *group)
{
    if (!d || !window || !*window || (*window)->device != d) return EXTGPU_E_ARGUMENT;
    pthread_mutex_lock(&d->lock);
    extgpu_status st = warm_release_vm_locked(d, window, group);
    pthread_mutex_unlock(&d->lock);
    return st;
}

extgpu_status extgpu_warm_release_vm_no_dma(extgpu_device *d)
{
    if (!d) return EXTGPU_E_ARGUMENT;
    pthread_mutex_lock(&d->lock);
    const struct warm_reset_group *group = NULL;
    if (d->pinned_scope_generation == d->protocol.generation) {
        switch (d->pinned_scope_selector) {
        case EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA: group = &vega_group; break;
        case EXTGPU_SELECTOR_WARM_RELEASE_VM_RTX3090: group = &rtx3090_group; break;
        case EXTGPU_SELECTOR_WARM_RELEASE_VM_POLARIS: group = &polaris_group; break;
        }
    }
    extgpu_status st = warm_release_vm_locked(d, NULL, group);
    pthread_mutex_unlock(&d->lock);
    return st;
}

extgpu_status extgpu_warm_release_vm_flr(extgpu_device *d, extgpu_buffer **window)
{
    return warm_release_vm(d, window, NULL);
}

extgpu_status extgpu_warm_release_vm_flr_preflight(extgpu_device *d)
{
    if (!d) return EXTGPU_E_ARGUMENT;
    pthread_mutex_lock(&d->lock);
    extgpu_status st = ready(d, EXTGPU_CAP_WARM_VM_FLR_RELEASE | EXTGPU_CAP_VM_FLR_PREFLIGHT, true);
    if (st == EXTGPU_OK && (!d->pci_registry_id || resources(d) || d->bar_cache_attempted_mask))
        st = error(d, EXTGPU_E_BUSY, 0, "VMFLR.requires_pristine_claim");
    if (st == EXTGPU_OK) {
        uint64_t generation = d->protocol.generation;
        uint64_t reply[EXTGPU_FLR_PREFLIGHT_REPLY_WORDS] = {0};
        st = rpc(d, EXTGPU_SELECTOR_VM_FLR_PREFLIGHT, &generation, 1,
                 reply, EXTGPU_FLR_PREFLIGHT_REPLY_WORDS, "VMFLR.preflight");
        if (st == EXTGPU_OK && (reply[0] != generation || reply[1] != d->pci_registry_id || reply[2] > 1))
            st = error(d, EXTGPU_E_PROTOCOL, 0, "VMFLR.preflight_mismatch");
        if (st == EXTGPU_OK && !reply[2])
            st = error(d, EXTGPU_E_UNSUPPORTED, 0, "VMFLR.endpoint_not_advertised");
    }
    pthread_mutex_unlock(&d->lock);
    return st;
}

extgpu_status extgpu_restore_vega_bars(extgpu_device *d)
{
    if (!d) return EXTGPU_E_ARGUMENT;
    pthread_mutex_lock(&d->lock);
    extgpu_status st = ready(d, EXTGPU_CAP_RESTORE_VEGA_BARS, true);
    if (st == EXTGPU_OK && (!d->pci_registry_id || resources(d) || d->bar_cache_attempted_mask))
        st = error(d, EXTGPU_E_BUSY, 0, "RestoreVegaBARs.requires_pristine_claim");
    if (st == EXTGPU_OK) {
        const uint64_t in[] = {d->protocol.generation, d->pci_registry_id};
        uint64_t reply[EXTGPU_RESTORE_VEGA_BARS_WORDS] = {0};
        st = rpc(d, EXTGPU_SELECTOR_RESTORE_VEGA_BARS, in, EXTGPU_RESTORE_VEGA_BARS_WORDS,
                 reply, EXTGPU_RESTORE_VEGA_BARS_WORDS, "RestoreVegaBARs");
        if (st == EXTGPU_OK && memcmp(in, reply, sizeof in))
            st = error(d, EXTGPU_E_PROTOCOL, 0, "RestoreVegaBARs.reply_mismatch");
    }
    pthread_mutex_unlock(&d->lock);
    return st;
}

extgpu_status extgpu_warm_release_vm_vega(extgpu_device *d, extgpu_buffer **window)
{
    return warm_release_vm(d, window, &vega_group);
}

extgpu_status extgpu_warm_release_vm_rtx3090(extgpu_device *d, extgpu_buffer **window)
{
    return warm_release_vm(d, window, &rtx3090_group);
}

extgpu_status extgpu_warm_release_vm_polaris(extgpu_device *d, extgpu_buffer **window)
{
    return warm_release_vm(d, window, &polaris_group);
}

extgpu_status extgpu_probe_vega_perst(extgpu_device *d, bool *reset_issued)
{
    if (!d || !reset_issued) return EXTGPU_E_ARGUMENT;
    pthread_mutex_lock(&d->lock);
    extgpu_status st = EXTGPU_OK;
    if (d->original_closed || d->cold_attempted || d->ownership_uncertain ||
        !d->protocol_valid || !d->claimed ||
        (d->warm_vm_pending && d->warm_vm_selector != EXTGPU_SELECTOR_PROBE_VEGA_PERST))
        st = error(d, EXTGPU_E_UNSAFE, 0, "VegaPERST.owner_uncertain");
    else if (!(d->protocol.capabilities & EXTGPU_CAP_VEGA_PERST_PROBE))
        st = error(d, EXTGPU_E_UNSUPPORTED, 0, "VegaPERST.capability_absent");
    else if (resources(d) || d->bar_cache_attempted_mask)
        st = error(d, EXTGPU_E_BUSY, 0, "VegaPERST.requires_idle_claim");
    else if (!d->warm_vm_pending && !ready_group_reset_scope(d, &vega_group, d->warm_group_scope))
        st = error(d, EXTGPU_E_UNSUPPORTED, 0, "VegaPERST.scope_missing");

    if (st == EXTGPU_OK) {
        uint64_t request[EXTGPU_WARM_VEGA_REQUEST_WORDS] = {d->protocol.generation, 0, 0};
        memcpy(request + 3, d->warm_group_scope, sizeof d->warm_group_scope);
        uint64_t reply[EXTGPU_VEGA_PERST_REPLY_WORDS] = {0};
        bool retry = d->warm_vm_pending;
        d->warm_vm_pending = true;
        d->warm_vm_selector = EXTGPU_SELECTOR_PROBE_VEGA_PERST;
        st = rpc(d, EXTGPU_SELECTOR_PROBE_VEGA_PERST, request, EXTGPU_WARM_VEGA_REQUEST_WORDS,
                 reply, EXTGPU_VEGA_PERST_REPLY_WORDS, "VegaPERST.probe");
        uint64_t outcome = reply[EXTGPU_WARM_VEGA_REPLY_WORDS];
        bool native_failure = (outcome >> 32) == 1 && (uint32_t)outcome != 0;
        if (st == EXTGPU_OK && (memcmp(request, reply, sizeof request) ||
            reply[EXTGPU_WARM_VEGA_REQUEST_WORDS] != EXTGPU_WARM_VM_CLOSED ||
            (outcome != EXTGPU_VEGA_PERST_RESET && outcome != EXTGPU_VEGA_PERST_UNSUPPORTED && !native_failure)))
            st = error(d, EXTGPU_E_PROTOCOL, 0, "VegaPERST.reply_mismatch");
        if (st == EXTGPU_OK) {
            *reset_issued = outcome == EXTGPU_VEGA_PERST_RESET;
            d->claimed = false;
            d->protocol_valid = false;
            d->warm_vm_pending = false;
            const kern_return_t native = native_failure ? (kern_return_t)(uint32_t)outcome :
                outcome == EXTGPU_VEGA_PERST_UNSUPPORTED ? kIOReturnUnsupported : kIOReturnSuccess;
            error(d, native_status(native), native, "VegaPERST.closed");
        } else if (!retry && d->error.native_code == kIOReturnUnsupported) {
            d->warm_vm_pending = false; /* Rejected by the driver's idle preflight. */
        } else {
            st = error(d, EXTGPU_E_UNSAFE, d->error.native_code, "VegaPERST.outcome_unacknowledged");
        }
    }
    pthread_mutex_unlock(&d->lock);
    return st;
}

extgpu_status extgpu_buffer_unregister(extgpu_buffer **ptr)
{
    if (!ptr || !*ptr) return EXTGPU_E_ARGUMENT;
    extgpu_buffer *b=*ptr; extgpu_device *d=b->device;
    pthread_mutex_lock(&d->lock); extgpu_status st=retire_buffer(b); pthread_mutex_unlock(&d->lock);
    if (st==EXTGPU_OK) { free(b); *ptr=NULL; } return st;
}
extgpu_status extgpu_dma_probe(extgpu_buffer *b, uint64_t off, uint64_t *out)
{
    if (!b || !out || b->allocated || off%8 || b->coverage.requested_bytes<8 || off>b->coverage.requested_bytes-8) return EXTGPU_E_ARGUMENT;
    extgpu_device *d=b->device; pthread_mutex_lock(&d->lock);
    extgpu_status st=ready(d,EXTGPU_CAP_DMA_REGISTER,true);
    if (b->unsafe || b->coverage.generation!=d->protocol.generation) st=EXTGPU_E_UNSAFE;
    uint64_t in[2]={b->coverage.requested_iova,off};
    if (st==EXTGPU_OK) st=rpc(d,EXTGPU_SELECTOR_PROBE_DMA_REGION,in,2,out,1,"ProbeDMA");
    pthread_mutex_unlock(&d->lock); return st;
}

static void irq_async(void *opaque, IOReturn result, void **arguments, uint32_t argument_count)
{
    (void)arguments; (void)argument_count;
    extgpu_irq *irq=opaque;
    pthread_mutex_lock(&irq->lock);
    if (!irq->shared_delivery) irq->armed=false;
    extgpu_irq_callback callback=!irq->stopping && result==kIOReturnSuccess ? irq->callback : NULL;
    void *context=irq->context;
    pthread_mutex_unlock(&irq->lock);
    if (callback) callback(irq,context);
}
static void irq_receive(void *opaque)
{
    extgpu_irq *irq=opaque;
    union { mach_msg_header_t header; unsigned char bytes[4096]; } message;
    /* Bounded drain per dispatch turn; another wake handles remaining messages. */
    for (unsigned n=0;n<64;++n) {
        kern_return_t kr=mach_msg(&message.header,MACH_RCV_MSG|MACH_RCV_TIMEOUT,0,sizeof(message),
                                  IONotificationPortGetMachPort(irq->port),0,MACH_PORT_NULL);
        if (kr!=KERN_SUCCESS) break;
        IODispatchCalloutFromMessage(NULL,&message.header,irq->port);
    }
}
static void irq_cancelled(void *opaque)
{
    extgpu_irq *irq=opaque;
    IONotificationPortDestroy(irq->port); irq->port=NULL;
    dispatch_semaphore_signal(irq->cancelled);
}
static extgpu_status irq_arm_locked(extgpu_irq *irq, uint64_t pending[EXTGPU_IRQ_WORDS])
{
    extgpu_device *d=irq->device;
    uint64_t reference[kIOAsyncCalloutCount]={0};
    reference[kIOAsyncCalloutFuncIndex]=(uintptr_t)irq_async;
    reference[kIOAsyncCalloutRefconIndex]=(uintptr_t)irq;
    uint64_t reply[EXTGPU_IRQ_WORDS] = {0};
    uint32_t words = EXTGPU_IRQ_WORDS;
    const bool shared = irq->shared_delivery;
    const uint32_t selector = shared ? EXTGPU_SELECTOR_IRQ_SUBSCRIBE_SHARED :
        (pending ? EXTGPU_SELECTOR_IRQ_DRAIN_AND_REARM : EXTGPU_SELECTOR_WAIT_INTERRUPT);
    const char *operation = shared ? "IRQ.subscribe_shared" : (pending ? "IRQ.drain_and_rearm" : "WaitInterrupt");
    kern_return_t kr=IOConnectCallAsyncScalarMethod(d->connection,selector,
        IONotificationPortGetMachPort(irq->port),reference,kIOAsyncCalloutCount,
        shared ? &irq->generation : NULL,shared ? 1 : 0,
        pending ? reply : NULL,pending ? &words : NULL);
    if (kr==KERN_SUCCESS || pending || shared) irq->armed=true;
    if (kr==KERN_SUCCESS && pending) {
        /* A malformed reply may follow an acknowledged arm on the driver.
         * Preserve the armed state for retirement and never replay the drain. */
        if (words != EXTGPU_IRQ_WORDS) return error(d,EXTGPU_E_PROTOCOL,0,operation);
        memcpy(pending,reply,sizeof(reply));
    }
    return error(d,native_status(kr),kr,operation);
}
static extgpu_status irq_map_mailbox(extgpu_irq *irq)
{
    extgpu_device *d=irq->device;
    uint64_t address=0, bytes=0;
    const kern_return_t kr=map_memory(d,EXTGPU_MEMORY_IRQ_MAILBOX,&address,&bytes,kIOMapAnywhere);
    if (kr!=KERN_SUCCESS) return error(d,native_status(kr),kr,"IRQ.map_mailbox");
    irq->mailbox_address=address;
    struct openpci_irq_mailbox *mailbox=(void *)(uintptr_t)irq->mailbox_address;
    if (!mailbox || bytes<OPENPCI_IRQ_MAILBOX_BYTES || irq->mailbox_address % _Alignof(struct openpci_irq_mailbox) ||
        __atomic_load_n(&mailbox->generation,__ATOMIC_ACQUIRE)!=irq->generation)
        return error(d,EXTGPU_E_PROTOCOL,0,"IRQ.map_mailbox");
    irq->mailbox=mailbox;
    return EXTGPU_OK;
}

static extgpu_status irq_create(extgpu_device *d, extgpu_irq_callback callback, void *context,
                                extgpu_irq **out, uint32_t *vectors, bool shared)
{
    if (!d || !callback || !out || !vectors) return EXTGPU_E_ARGUMENT;
    *out=NULL; *vectors=0;
    pthread_mutex_lock(&d->lock);
    if (d->cold_attempted) {
        extgpu_status st = error(d, EXTGPU_E_UNSAFE, 0, "Cold.new_IRQ_denied");
        pthread_mutex_unlock(&d->lock);
        return st;
    }
    pthread_mutex_unlock(&d->lock);
    extgpu_irq *irq=calloc(1,sizeof(*irq)); if (!irq) return EXTGPU_E_MEMORY;
    if (pthread_mutex_init(&irq->lock,NULL)) { free(irq); return EXTGPU_E_MEMORY; }
    irq->device=d; irq->callback=callback; irq->context=context;
    irq->shared_delivery=shared;
    irq->queue=dispatch_queue_create("extgpu.irq",DISPATCH_QUEUE_SERIAL);
    irq->cancelled=dispatch_semaphore_create(0);
    irq->port=IONotificationPortCreate(kIOMainPortDefault);
    if (!irq->queue || !irq->cancelled || !irq->port) {
        if (irq->port) IONotificationPortDestroy(irq->port);
        if (irq->queue) dispatch_release(irq->queue);
        if (irq->cancelled) dispatch_release(irq->cancelled);
        pthread_mutex_destroy(&irq->lock); free(irq); return EXTGPU_E_MEMORY;
    }
    irq->source=dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_RECV,IONotificationPortGetMachPort(irq->port),0,irq->queue);
    if (!irq->source) { IONotificationPortDestroy(irq->port); dispatch_release(irq->queue); dispatch_release(irq->cancelled); pthread_mutex_destroy(&irq->lock); free(irq); return EXTGPU_E_MEMORY; }
    dispatch_queue_set_specific(irq->queue,&irq_queue_key,irq,NULL);
    dispatch_set_context(irq->source,irq);
    dispatch_source_set_event_handler_f(irq->source,irq_receive);
    dispatch_source_set_cancel_handler_f(irq->source,irq_cancelled);
    dispatch_resume(irq->source);
    pthread_mutex_lock(&d->lock);
    uint64_t caps=EXTGPU_CAP_IRQ_ASYNC | EXTGPU_CAP_IRQ_RETIRE;
    if (shared) caps |= EXTGPU_CAP_IRQ_SHARED_NOTIFICATION;
    extgpu_status st=ready(d,caps,true);
    if (st==EXTGPU_OK && d->irq) st=EXTGPU_E_BUSY;
    if (st==EXTGPU_OK && shared && d->shared_irq_attempted)
        st=error(d,EXTGPU_E_UNSUPPORTED,0,"IRQ.shared_already_used");
    uint64_t count=0;
    bool setup_sent=st==EXTGPU_OK;
    if (setup_sent) st=rpc(d,EXTGPU_SELECTOR_SETUP_INTERRUPTS,NULL,0,&count,1,"SetupInterrupts");
    if (setup_sent) {
        /* A failed/malformed reply may follow partial physical setup. Keep a
         * cleanup carrier even if local async arming never began. Retire21 is
         * idempotent and can acknowledge the zero-resource case as well. */
        d->irq=irq; irq->generation=d->protocol.generation; *out=irq;
        if (st==EXTGPU_OK) {
            if (!count || count>256) {
                st=error(d,EXTGPU_E_PROTOCOL,0,"SetupInterrupts.vector_count");
                irq->driver_retirement_started=true;
            }
            else {
                irq->vectors=(uint32_t)count; *vectors=(uint32_t)count;
                pthread_mutex_lock(&irq->lock);
                if (shared) {
                    d->shared_irq_attempted=true;
                    st=irq_map_mailbox(irq);
                }
                if (st==EXTGPU_OK) st=irq_arm_locked(irq,NULL);
                pthread_mutex_unlock(&irq->lock);
            }
        } else {
            irq->driver_retirement_started=true;
        }
    }
    if (*out && st!=EXTGPU_OK) {
        /* Setup already acquired driver state. Keep its source, receive port,
         * refcon and device child alive until explicit close drains them. The
         * failing operation/native code remain available in last_error. */
        pthread_mutex_lock(&irq->lock); irq->stopping=true; pthread_mutex_unlock(&irq->lock);
        d->error.status=EXTGPU_E_UNSAFE;
        st=EXTGPU_E_UNSAFE;
    }
    pthread_mutex_unlock(&d->lock);
    if (!*out) {
        dispatch_source_cancel(irq->source); dispatch_semaphore_wait(irq->cancelled,DISPATCH_TIME_FOREVER);
        dispatch_release(irq->source); dispatch_release(irq->queue); dispatch_release(irq->cancelled);
        pthread_mutex_destroy(&irq->lock); free(irq);
    }
    return st;
}
extgpu_status extgpu_irq_create(extgpu_device *d, extgpu_irq_callback callback, void *context,
                                extgpu_irq **out, uint32_t *vectors)
{
    return irq_create(d,callback,context,out,vectors,false);
}
extgpu_status extgpu_irq_create_shared(extgpu_device *d, extgpu_irq_callback callback, void *context,
                                extgpu_irq **out, uint32_t *vectors)
{
    return irq_create(d,callback,context,out,vectors,true);
}
extgpu_status extgpu_irq_set_mask(extgpu_irq *irq, const uint64_t mask[4])
{
    if (!irq || !mask) return EXTGPU_E_ARGUMENT;
    extgpu_device *d=irq->device; pthread_mutex_lock(&d->lock);
    extgpu_status st=ready(d,EXTGPU_CAP_IRQ_ASYNC,true);
    if (irq->generation!=d->protocol.generation) st=EXTGPU_E_STALE;
    if (st==EXTGPU_OK) st=rpc(d,EXTGPU_SELECTOR_SET_IRQ_MASK,mask,4,NULL,0,"SetIRQMask");
    pthread_mutex_unlock(&d->lock); return st;
}
static extgpu_status irq_take_shared(extgpu_irq *irq, uint64_t pending[EXTGPU_IRQ_WORDS])
{
    pthread_mutex_lock(&irq->lock);
    extgpu_status st=irq->stopping || !irq->mailbox ? EXTGPU_E_BUSY : EXTGPU_OK;
    if (st==EXTGPU_OK && pending) openpci_irq_drain_mailbox(irq->mailbox,pending);
    pthread_mutex_unlock(&irq->lock);
    return st;
}

extgpu_status extgpu_irq_drain(extgpu_irq *irq, uint64_t pending[4])
{
    if (!irq || !pending) return EXTGPU_E_ARGUMENT;
    if (irq->shared_delivery) return irq_take_shared(irq,pending);
    extgpu_device *d=irq->device; pthread_mutex_lock(&d->lock);
    extgpu_status st=ready(d,EXTGPU_CAP_IRQ_ASYNC,true);
    if (irq->generation!=d->protocol.generation) st=EXTGPU_E_STALE;
    if (st==EXTGPU_OK) st=rpc(d,EXTGPU_SELECTOR_CHECK_INTERRUPT,NULL,0,pending,4,"CheckInterrupt");
    pthread_mutex_unlock(&d->lock); return st;
}
extgpu_status extgpu_irq_rearm(extgpu_irq *irq)
{
    if (!irq) return EXTGPU_E_ARGUMENT;
    if (irq->shared_delivery) return irq_take_shared(irq,NULL);
    extgpu_device *d=irq->device; pthread_mutex_lock(&d->lock); pthread_mutex_lock(&irq->lock);
    extgpu_status st=ready(d,EXTGPU_CAP_IRQ_ASYNC,true);
    if (irq->stopping || irq->armed) st=EXTGPU_E_BUSY;
    if (irq->generation!=d->protocol.generation) st=EXTGPU_E_STALE;
    if (st==EXTGPU_OK) st=irq_arm_locked(irq,NULL);
    pthread_mutex_unlock(&irq->lock); pthread_mutex_unlock(&d->lock); return st;
}
extgpu_status extgpu_irq_drain_and_rearm(extgpu_irq *irq, uint64_t pending[EXTGPU_IRQ_WORDS])
{
    if (!irq || !pending) return EXTGPU_E_ARGUMENT;
    if (irq->shared_delivery) return irq_take_shared(irq,pending);
    extgpu_device *d=irq->device;
    pthread_mutex_lock(&d->lock);
    pthread_mutex_lock(&irq->lock);
    extgpu_status st=ready(d,EXTGPU_CAP_IRQ_ASYNC,true);
    if (st==EXTGPU_OK && (irq->stopping || irq->armed)) st=error(d,EXTGPU_E_BUSY,0,"IRQ.drain_and_rearm");
    if (st==EXTGPU_OK && irq->generation!=d->protocol.generation)
        st=error(d,EXTGPU_E_STALE,0,"IRQ.drain_and_rearm");
    if (st==EXTGPU_OK) {
        if (d->protocol.capabilities & EXTGPU_CAP_IRQ_DRAIN_AND_REARM) st=irq_arm_locked(irq,pending);
        else {
            st=rpc(d,EXTGPU_SELECTOR_CHECK_INTERRUPT,NULL,0,pending,EXTGPU_IRQ_WORDS,"CheckInterrupt");
            if (st==EXTGPU_OK) st=irq_arm_locked(irq,NULL);
        }
    }
    pthread_mutex_unlock(&irq->lock);
    pthread_mutex_unlock(&d->lock);
    return st;
}
extgpu_status extgpu_irq_close(extgpu_irq **ptr)
{
    if (!ptr || !*ptr) return EXTGPU_E_ARGUMENT;
    extgpu_irq *irq=*ptr;
    /* Cross-IRQ closes can deadlock two serial callback queues waiting for
     * each other's cancellation handlers. All close operations belong on a
     * control thread, outside every SDK IRQ callback queue. */
    if (dispatch_get_specific(&irq_queue_key)!=NULL) return EXTGPU_E_BUSY;
    extgpu_device *d=irq->device; pthread_mutex_lock(&d->lock);
    pthread_mutex_lock(&irq->lock); irq->stopping=true; pthread_mutex_unlock(&irq->lock);
    extgpu_status st=ready(d,EXTGPU_CAP_IRQ_ASYNC | EXTGPU_CAP_IRQ_RETIRE,true);
    if (st==EXTGPU_OK && !irq->driver_retirement_started) {
        uint64_t zero[4]={0}, pending[4]={0};
        st=rpc(d,EXTGPU_SELECTOR_SET_IRQ_MASK,zero,4,NULL,0,"IRQ.close_mask");
        if (st==EXTGPU_OK) st=rpc(d,EXTGPU_SELECTOR_CHECK_INTERRUPT,NULL,0,pending,4,"IRQ.close_drain");
        if (st==EXTGPU_OK) irq->driver_retirement_started=true;
    }
    /* After retirement starts the driver's shared buffer can disappear. Retry
     * retirement directly rather than requiring a second drain of freed state.
     * An unknown IPC reply also preserves this flag and every local resource. */
    if (st==EXTGPU_OK) st=rpc(d,EXTGPU_SELECTOR_RETIRE_INTERRUPTS,NULL,0,NULL,0,"IRQ.close_retire");
    if (st==EXTGPU_OK && irq->mailbox_address) {
        /* stopping was published under irq->lock. Existing readers have
         * drained and later callback calls cannot touch the mapped page. */
        const kern_return_t kr=unmap_memory(d,EXTGPU_MEMORY_IRQ_MAILBOX,irq->mailbox_address);
        if (kr==KERN_SUCCESS) {
            irq->mailbox_address=0;
            irq->mailbox=NULL;
        } else st=error(d,native_status(kr),kr,"IRQ.unmap_mailbox");
    }
    if (st!=EXTGPU_OK) d->error.status=EXTGPU_E_UNSAFE;
    pthread_mutex_unlock(&d->lock);
    if (st!=EXTGPU_OK) return EXTGPU_E_UNSAFE; /* Preserve source/refcon/owner for retry. */
    dispatch_source_cancel(irq->source);
    dispatch_semaphore_wait(irq->cancelled,DISPATCH_TIME_FOREVER);
    /* Cancellation handler runs after every event/callback handler on queue. */
    pthread_mutex_lock(&d->lock); d->irq=NULL; pthread_mutex_unlock(&d->lock);
    dispatch_release(irq->source); dispatch_release(irq->queue); dispatch_release(irq->cancelled);
    pthread_mutex_destroy(&irq->lock); free(irq); *ptr=NULL;
    return EXTGPU_OK;
}

extgpu_status extgpu_surprise_removal_complete(extgpu_device *d, extgpu_buffer **window,
                                               extgpu_irq **irq_ptr, const uint64_t bar_proof[8])
{
    if (!d || !window || !irq_ptr || dispatch_get_specific(&irq_queue_key)) return EXTGPU_E_ARGUMENT;
    pthread_mutex_lock(&d->lock);
    if (d->surprise_released) { pthread_mutex_unlock(&d->lock); return EXTGPU_OK; }
    extgpu_irq *irq = *irq_ptr;
    const bool valid_window = d->vm_dynamic_window ? *window == d->vm_dynamic_window :
        *window == d->buffer_list && d->buffers == (*window ? 1u : 0u);
    if (!d->surprise_requested || !d->broker_bound || d->bar_mask || d->allocated_buffer ||
        d->surprise_draining_irq || irq != d->irq || !valid_window ||
        (d->direct_bar_authorized && !bar_proof)) {
        pthread_mutex_unlock(&d->lock);
        return EXTGPU_E_UNSAFE;
    }
    d->surprise_detaching = true;
    extgpu_status st = confirm_registry_absent(d, d->pci_registry_id);
    if (st == EXTGPU_OK) st = confirm_registry_absent(d, d->surprise_bridge);
    if (st == EXTGPU_OK && irq) {
        pthread_mutex_lock(&irq->lock);
        irq->stopping = true;
        pthread_mutex_unlock(&irq->lock);
        if (irq->mailbox_address) {
            const kern_return_t kr = unmap_detached_memory(d, EXTGPU_MEMORY_IRQ_MAILBOX,
                irq->mailbox_address, OPENPCI_IRQ_MAILBOX_BYTES);
            if (kr == KERN_SUCCESS) {
                irq->mailbox_address = 0;
                irq->mailbox = NULL;
            } else st = error(d, native_status(kr), kr, "Surprise.unmap_irq");
        }
    }
    if (st == EXTGPU_OK) st = surprise_close_escrow(d, bar_proof);
    if (st != EXTGPU_OK) { pthread_mutex_unlock(&d->lock); return st; }
    if (irq) {
        /* The source's last callback can call back into the SDK. */
        d->surprise_draining_irq = true;
        pthread_mutex_unlock(&d->lock);
        dispatch_source_cancel(irq->source);
        dispatch_semaphore_wait(irq->cancelled, DISPATCH_TIME_FOREVER);
        pthread_mutex_lock(&d->lock);
        dispatch_release(irq->source);
        dispatch_release(irq->queue);
        dispatch_release(irq->cancelled);
        pthread_mutex_destroy(&irq->lock);
        free(irq);
        d->irq = *irq_ptr = NULL;
        d->surprise_draining_irq = false;
    }
    while (d->buffer_list) {
        extgpu_buffer *retired = d->buffer_list;
        release_buffer_accounting(retired);
        free(retired);
    }
    d->vm_dynamic_window = *window = NULL;
    d->claimed = false;
    d->protocol_valid = false;
    d->warm_vm_pending = false;
    d->surprise_released = true;
    pthread_mutex_unlock(&d->lock);
    return EXTGPU_OK;
}

#ifdef EXTGPU_TESTING
extgpu_status extgpu_test_set_recovery_transport(extgpu_device *d, const extgpu_test_transport *transport,
    const extgpu_test_recovery_hooks *hooks, void *context)
{
    if (!d || !transport || !transport->scalar || !transport->close || !hooks ||
        !hooks->lookup || !hooks->open || !hooks->drop_original_local) return EXTGPU_E_ARGUMENT;
    pthread_mutex_lock(&d->lock);
    extgpu_status st = !d->test || d->cold_attempted || d->broker_opened ? EXTGPU_E_UNSAFE : EXTGPU_OK;
    if (st == EXTGPU_OK) { d->test_broker = transport; d->test_recovery = hooks; d->test_broker_context = context; }
    pthread_mutex_unlock(&d->lock); return st;
}
extgpu_status extgpu_test_set_registry_ids(extgpu_device *d, uint64_t driver_id, uint64_t pci_id)
{
    if (!d || !driver_id || !pci_id || driver_id == pci_id) return EXTGPU_E_ARGUMENT;
    pthread_mutex_lock(&d->lock);
    extgpu_status st = !d->test || d->cold_attempted ? EXTGPU_E_UNSAFE : EXTGPU_OK;
    if (st == EXTGPU_OK) { d->registry_id = driver_id; d->pci_registry_id = pci_id; }
    pthread_mutex_unlock(&d->lock);
    return st;
}
extgpu_status extgpu_test_set_reset_scope(extgpu_device *d, uint64_t bridge, uint64_t audio)
{
    if (!d || (!bridge != !audio) || (bridge && bridge == audio)) return EXTGPU_E_ARGUMENT;
    pthread_mutex_lock(&d->lock);
    extgpu_status st = !d->test || !d->pci_registry_id ||
        bridge == d->pci_registry_id || audio == d->pci_registry_id ? EXTGPU_E_UNSAFE : EXTGPU_OK;
    if (st == EXTGPU_OK) {
        d->test_reset_bridge = bridge;
        d->test_reset_audio = audio;
    }
    pthread_mutex_unlock(&d->lock);
    return st;
}
extgpu_status extgpu_test_set_provider_snapshot(extgpu_device *d, const extgpu_test_provider_snapshot *snapshot)
{
    if (!d || !snapshot) return EXTGPU_E_ARGUMENT;
    pthread_mutex_lock(&d->lock);
    extgpu_status st = d->test ? EXTGPU_OK : EXTGPU_E_ARGUMENT;
    if (st == EXTGPU_OK) {
        if (d->test_reset_bridge && snapshot->registry_id == d->test_reset_bridge)
            d->test_bridge_provider = *snapshot;
        else {
            d->test_provider = *snapshot;
            d->test_provider_seeded = true;
        }
    }
    pthread_mutex_unlock(&d->lock);
    return st;
}
extgpu_status extgpu_test_open(const extgpu_test_transport *transport, void *context,
                               extgpu_session **session, extgpu_device **device)
{
    if (!transport || !transport->scalar || !transport->map || !transport->unmap || !transport->close || !session || !device) return EXTGPU_E_ARGUMENT;
    *session=NULL; *device=NULL;
    extgpu_status st=extgpu_session_create(session); if (st!=EXTGPU_OK) return st;
    extgpu_device *d=new_device(*session); if (!d) { extgpu_session_destroy(session); return EXTGPU_E_MEMORY; }
    d->test=transport; d->test_context=context;
    st=query(d);
    if (st!=EXTGPU_OK) { pthread_mutex_destroy(&d->lock); free(d); extgpu_session_destroy(session); return st; }
    ++(*session)->devices; *device=d;
    return EXTGPU_OK;
}

extgpu_status extgpu_test_bind_mmio_window(extgpu_device *d, struct openpci_mmio_window *window)
{
    if (!d || !d->test || !window) return EXTGPU_E_ARGUMENT;
    pthread_mutex_lock(&d->lock);
    extgpu_status st = ready(d, EXTGPU_CAP_MMIO_WINDOW, true);
    if (st == EXTGPU_OK && (d->mmio_window_attempted || window->generation != d->protocol.generation ||
                           openpci_mmio_phase(openpci_mmio_state(window)) != OPENPCI_MMIO_READY))
        st = EXTGPU_E_ARGUMENT;
    if (st == EXTGPU_OK) {
        d->mmio_queue = dispatch_queue_create("org.openpci.mmio.test", DISPATCH_QUEUE_SERIAL);
        if (!d->mmio_queue) st = EXTGPU_E_MEMORY;
        else {
            d->mmio_window_attempted = true;
            d->mmio_generation = d->protocol.generation;
            d->mmio_window = window;
            d->mmio_window_word_writes = (d->protocol.capabilities & EXTGPU_CAP_MMIO_WINDOW_WORD_WRITES) != 0;
        }
    }
    pthread_mutex_unlock(&d->lock);
    return st;
}
#endif
