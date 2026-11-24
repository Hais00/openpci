/* SPDX-License-Identifier: MIT
 * Deliberately simulated public SDK for native-probe's command/lifetime tests.
 * This fixture is never linked into the real native probe. No IOKit or GPU. */
#define _POSIX_C_SOURCE 200809L
#include <extgpu/extgpu.h>
#include <stdlib.h>
#include <string.h>

struct extgpu_session { unsigned devices; };
struct extgpu_device { extgpu_session *session; int claimed, children; };
struct extgpu_mapping { extgpu_device *device; };
struct extgpu_buffer { extgpu_device *device; void *backing; uint64_t bytes; int allocated; };
struct extgpu_irq { extgpu_device *device; };
static extgpu_device *owner;
static int failed_retire, failed_unmap, failed_irq_close, failed_release;

static int fault(const char *name)
{
    const char *selection = getenv("NATIVE_PROBE_FIXTURE_FAULT");
    return selection && !strcmp(selection, name);
}
const char *extgpu_status_string(extgpu_status st) { return st == EXTGPU_OK ? "ok" : "fixture error"; }
uint32_t extgpu_sdk_abi_version(void) { return EXTGPU_SDK_ABI_VERSION; }
extgpu_status extgpu_session_create(extgpu_session **out)
{ *out = calloc(1, sizeof(**out)); return *out ? EXTGPU_OK : EXTGPU_E_MEMORY; }
extgpu_status extgpu_session_destroy(extgpu_session **ptr)
{
    if (!ptr || !*ptr) return EXTGPU_E_ARGUMENT;
    if ((*ptr)->devices) return EXTGPU_E_BUSY;
    free(*ptr); *ptr = NULL; return EXTGPU_OK;
}
extgpu_status extgpu_enumerate(extgpu_session *s, extgpu_device_info *items, size_t capacity, size_t *count)
{
    (void)s; *count = 1;
    if (!items && !capacity) return EXTGPU_OK;
    if (!capacity) return EXTGPU_E_CAPACITY;
    items[0] = (extgpu_device_info){.registry_id = 1, .pci_registry_id = 2,
        .vendor_id = 0x1002, .device_id = 0x687f, .registry_name = "SIMULATED service", .host_root = "SIMULATED root"};
    return EXTGPU_OK;
}
extgpu_status extgpu_device_open(extgpu_session *s, uint64_t id, extgpu_device **out)
{
    if (id != 1) return EXTGPU_E_NOT_FOUND;
    *out = calloc(1, sizeof(**out)); if (!*out) return EXTGPU_E_MEMORY;
    (*out)->session = s; ++s->devices; return EXTGPU_OK;
}
extgpu_status extgpu_device_protocol(extgpu_device *d, extgpu_protocol_info *out)
{
    if (!d) return EXTGPU_E_ARGUMENT;
    *out = (extgpu_protocol_info){.major = 1, .minor = 1, .generation = 1,
        .capabilities = 0x7ff, .maximum_dma_regions = 8, .maximum_dma_bytes = 65536,
        .maximum_dma_segments = 1, .abi_fingerprint = 123};
    return EXTGPU_OK;
}
extgpu_status extgpu_device_identity(extgpu_device *d, extgpu_pci_identity *out)
{
    if (!d) return EXTGPU_E_ARGUMENT;
    *out = (extgpu_pci_identity){.bus = 5, .device = 0, .function = 0,
        .vendor_id = 0x1002, .device_id = 0x687f, .class_code = 0x30000};
    return EXTGPU_OK;
}
extgpu_status extgpu_device_last_error(extgpu_device *d, extgpu_error_info *out)
{ (void)d; *out = (extgpu_error_info){.status = EXTGPU_E_UNSAFE, .native_code = -99, .operation = "SIMULATED failure"}; return EXTGPU_OK; }
extgpu_status extgpu_device_claim(extgpu_device *d)
{
    if (!d) return EXTGPU_E_ARGUMENT;
    if (owner && !fault("double-claim")) return EXTGPU_E_BUSY;
    owner = d; d->claimed = 1; return EXTGPU_OK;
}
extgpu_status extgpu_device_release(extgpu_device *d)
{
    if (!d) return EXTGPU_E_ARGUMENT;
    if (!d->claimed) return EXTGPU_E_NOT_CLAIMED;
    if (d->children) return EXTGPU_E_BUSY;
    if (fault("release-once") && !failed_release++) return EXTGPU_E_UNSAFE;
    d->claimed = 0; owner = NULL; return EXTGPU_OK;
}
extgpu_status extgpu_device_close(extgpu_device **ptr)
{
    if (!ptr || !*ptr) return EXTGPU_E_ARGUMENT;
    if ((*ptr)->claimed || (*ptr)->children) return EXTGPU_E_BUSY;
    --(*ptr)->session->devices; free(*ptr); *ptr = NULL; return EXTGPU_OK;
}
extgpu_status extgpu_device_set_dma_address_bits(extgpu_device *d, uint32_t bits)
{ return d && d->claimed && bits >= 32 && bits <= 64 ? EXTGPU_OK : EXTGPU_E_ARGUMENT; }
extgpu_status extgpu_config_read(extgpu_device *d, uint32_t offset, uint32_t width, uint32_t *out)
{ (void)width; if (!d || !d->claimed) return EXTGPU_E_NOT_CLAIMED; *out = offset; return EXTGPU_OK; }
extgpu_status extgpu_bar_get_info(extgpu_device *d, uint32_t bar, extgpu_bar_info *out)
{ if (!d || !d->claimed) return EXTGPU_E_NOT_CLAIMED; *out = (extgpu_bar_info){.bar = bar, .memory_index = bar, .size = 4096}; return EXTGPU_OK; }
extgpu_status extgpu_bar_map(extgpu_device *d, uint32_t bar, extgpu_mapping **out)
{ (void)bar; *out = calloc(1, sizeof(**out)); if (!*out) return EXTGPU_E_MEMORY; (*out)->device = d; ++d->children; return EXTGPU_OK; }
void *extgpu_mapping_address(const extgpu_mapping *m) { return (void *)m; }
uint64_t extgpu_mapping_size(const extgpu_mapping *m) { return m ? 4096 : 0; }
uint64_t extgpu_mapping_generation(const extgpu_mapping *m) { return m ? 1 : 0; }
extgpu_status extgpu_mapping_unmap(extgpu_mapping **ptr)
{
    if (!ptr || !*ptr) return EXTGPU_E_ARGUMENT;
    if (fault("unmap-once") && !failed_unmap++) return EXTGPU_E_UNSAFE;
    --(*ptr)->device->children; free(*ptr); *ptr = NULL; return EXTGPU_OK;
}
extgpu_status extgpu_buffer_coverage(const extgpu_buffer *b, extgpu_dma_coverage *out)
{
    if (!b) return EXTGPU_E_ARGUMENT;
    *out = (extgpu_dma_coverage){.generation = 1, .requested_iova = 0x10000000,
        .requested_bytes = b->bytes, .bus_address = 0x12340000, .covered_bytes = b->bytes,
        .segment_count = 1, .complete = 1}; return EXTGPU_OK;
}
extgpu_status extgpu_dma_register(extgpu_device *d, uint64_t key, void *address, uint64_t bytes,
                                 extgpu_buffer **out, extgpu_dma_coverage *coverage)
{
    (void)key; *out = calloc(1, sizeof(**out)); if (!*out) return EXTGPU_E_MEMORY;
    (*out)->device = d; (*out)->backing = address; (*out)->bytes = bytes; ++d->children;
    (void)extgpu_buffer_coverage(*out, coverage);
    return fault("register-unsafe") ? EXTGPU_E_UNSAFE : EXTGPU_OK;
}
extgpu_status extgpu_dma_allocate(extgpu_device *d, uint64_t bytes, uint64_t alignment,
                                 extgpu_buffer **out, extgpu_dma_coverage *coverage)
{
    (void)alignment; void *backing = calloc(1, (size_t)bytes); if (!backing) return EXTGPU_E_MEMORY;
    extgpu_status st = extgpu_dma_register(d, 0, backing, bytes, out, coverage);
    if (*out) (*out)->allocated = 1; else free(backing); return st;
}
extgpu_status extgpu_buffer_unregister(extgpu_buffer **ptr)
{
    if (!ptr || !*ptr) return EXTGPU_E_ARGUMENT;
    if (fault("retire-once") && !failed_retire++) return EXTGPU_E_UNSAFE;
    if (!(*ptr)->allocated && ((unsigned char *)(*ptr)->backing)[0] != 0xa5) return EXTGPU_E_PROTOCOL;
    if ((*ptr)->allocated) free((*ptr)->backing);
    --(*ptr)->device->children; free(*ptr); *ptr = NULL; return EXTGPU_OK;
}
extgpu_status extgpu_irq_create(extgpu_device *d, extgpu_irq_callback callback, void *context,
                               extgpu_irq **out, uint32_t *vectors)
{
    (void)callback; (void)context;
    *out = calloc(1, sizeof(**out)); if (!*out) return EXTGPU_E_MEMORY;
    (*out)->device = d; ++d->children; *vectors = 1;
    return fault("irq-unsafe") ? EXTGPU_E_UNSAFE : EXTGPU_OK;
}
extgpu_status extgpu_irq_set_mask(extgpu_irq *irq, const uint64_t mask[EXTGPU_IRQ_WORDS])
{ (void)mask; if (!irq) return EXTGPU_E_ARGUMENT; return fault("irq-mask-io") ? EXTGPU_E_IO : EXTGPU_OK; }
extgpu_status extgpu_irq_drain(extgpu_irq *irq, uint64_t pending[EXTGPU_IRQ_WORDS])
{
    if (!irq) return EXTGPU_E_ARGUMENT;
    if (fault("irq-drain-protocol")) return EXTGPU_E_PROTOCOL;
    memset(pending, 0, sizeof(uint64_t) * EXTGPU_IRQ_WORDS); return EXTGPU_OK;
}
extgpu_status extgpu_irq_rearm(extgpu_irq *irq)
{
    if (!irq) return EXTGPU_E_ARGUMENT;
    if (fault("irq-rearm-stale")) return EXTGPU_E_STALE;
    if (fault("irq-rearm-busy")) return EXTGPU_E_BUSY;
    if (fault("irq-rearm-unsafe")) return EXTGPU_E_UNSAFE;
    return EXTGPU_OK;
}
extgpu_status extgpu_irq_close(extgpu_irq **ptr)
{
    if (!ptr || !*ptr) return EXTGPU_E_ARGUMENT;
    if (fault("irq-close-once") && !failed_irq_close++) return EXTGPU_E_UNSAFE;
    --(*ptr)->device->children; free(*ptr); *ptr = NULL; return EXTGPU_OK;
}
