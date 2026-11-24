/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Review regressions against actual SDK logic with private transport injection.
 * No device is opened and no GPU/driver RPC is performed. IRQ close exercises
 * real Darwin notification ports and GCD source cancellation only. */
#include "extgpu_test.h"
#include "fixtures/gpu_teardown.h"
#include <extgpu/extgpu_driver_abi.h>
#include <extgpu/private_mmio_window.h>
#include <IOKit/IOReturn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

typedef struct fixture {
    uint64_t generation, key, bytes, bus;
    uint32_t dma_bits;
    uint16_t pci_command;
    unsigned calls[EXTGPU_SELECTOR_VM_GROUP_PREFLIGHT + 1];
    bool owner, registered, allocated, query_fails_once, fail_query_after_reset;
    bool malformed_claim, malformed_release, oversized_segment, high_bus;
    bool malformed_irq, fail_irq_mask, fail_retire, uncertain_register, registration_rolled_back;
    bool irq_retired;
    unsigned irq_mask_pending_replies, irq_drain_pending_replies, irq_retire_pending_replies;
    int32_t irq_retire_error;
    bool command_read_all_ones, ignore_bus_master_clear, require_early_bus_master_clear;
    unsigned command_read_pending_replies, command_write_pending_replies;
    int32_t claim_error, query_error, release_error, close_error;
    bool original_closed, broker_capable;
    bool warm_capable, warm_unsupported, warm_lost_reply, warm_malformed_reply, warm_closed;
    bool warm_vega_capable, warm_rtx3090_capable, warm_polaris_capable;
    bool group_preflight_capable, group_companion_unreachable;
    bool warm_startup_capable, warm_no_dma_capable;
    bool vm_segments_capable, vm_segments_overlap, vm_segment_read_fails;
    bool vm_dynamic_capable, vm_dynamic_enabled;
    unsigned vm_dynamic_regions;
    bool vega_receipt_capable, vega_bad_receipt_once;
    bool warm_waiting_for_transactions, warm_waiting_for_close, warm_dma_retired;
    bool perst_capable, perst_unsupported;
    bool flr_preflight_capable, flr_advertised, flr_wrong_provider;
    bool bar_restore_capable, bar_restore_wrong_provider;
    kern_return_t bar_restore_native_error;
    bool bar_words_capable, bar_words_short_reply;
    unsigned bar_fill_count;
    uint64_t protocol_minor, window_capabilities;
    struct openpci_mmio_window *window;
    unsigned window_unmaps;
    kern_return_t perst_native_error;
    uint64_t warm_generation;
    uint32_t warm_selector;
    unsigned closes, stale_name_calls, broker_opens, broker_closes;
} fixture;

static int32_t reply(uint64_t *out, uint32_t *count, const uint64_t *values, uint32_t n)
{
    CHECK(count && *count >= n && (!n || out));
    if (n) memcpy(out, values, n * sizeof(*out));
    *count = n; return kIOReturnSuccess;
}
static int32_t call(void *context, uint32_t selector, const uint64_t *in,
                    uint32_t n, uint64_t *out, uint32_t *count)
{
    fixture *f = context;
    CHECK(selector < sizeof f->calls / sizeof f->calls[0]);
    if (f->original_closed) { ++f->stale_name_calls; return kIOReturnIPCError; }
    ++f->calls[selector];
    switch (selector) {
    case EXTGPU_SELECTOR_VM_GROUP_PREFLIGHT:
        CHECK(n == EXTGPU_VM_GROUP_PREFLIGHT_WORDS && in[0] == f->generation);
        CHECK(in[1] == EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA);
        CHECK(in[2] == 9100 && in[3] == 9101 && in[4] == 9102);
        CHECK(!out && *count == 0);
        return f->group_companion_unreachable ? kIOReturnUnsupported : kIOReturnSuccess;
    case EXTGPU_SELECTOR_QUERY_PROTOCOL: {
        CHECK(n == 3 && in[0] == EXTGPU_PROTOCOL_MAJOR && in[2] == EXTGPU_PROTOCOL_FINGERPRINT);
        if (f->query_fails_once) { f->query_fails_once = false; return f->query_error ? f->query_error : kIOReturnIPCError; }
        uint64_t values[8] = {EXTGPU_PROTOCOL_MAJOR, f->protocol_minor ? f->protocol_minor : EXTGPU_PROTOCOL_MINOR, f->generation,
            UINT64_C(0xfff) | (f->broker_capable ? EXTGPU_CAP_COLD_REMOVAL | EXTGPU_CAP_RECOVERY_BROKER : 0) |
            (f->warm_capable ? EXTGPU_CAP_WARM_VM_FLR_RELEASE : 0) |
            (f->warm_vega_capable ? EXTGPU_CAP_WARM_VM_VEGA_RELEASE : 0) |
            (f->warm_rtx3090_capable ? EXTGPU_CAP_WARM_VM_RTX3090_RELEASE : 0) |
            (f->warm_polaris_capable ? EXTGPU_CAP_WARM_VM_POLARIS_RELEASE : 0) |
            (f->group_preflight_capable ? EXTGPU_CAP_VM_GROUP_PREFLIGHT : 0) |
            (f->vega_receipt_capable ? EXTGPU_CAP_VEGA_RELEASE_RECEIPT : 0) |
            (f->warm_startup_capable ? EXTGPU_CAP_WARM_VM_STARTUP_RELEASE : 0) |
            (f->warm_no_dma_capable ? EXTGPU_CAP_WARM_VM_NO_DMA_RELEASE : 0) |
            (f->perst_capable ? EXTGPU_CAP_VEGA_PERST_PROBE : 0) |
            (f->flr_preflight_capable ? EXTGPU_CAP_VM_FLR_PREFLIGHT : 0) |
            (f->bar_restore_capable ? EXTGPU_CAP_RESTORE_VEGA_BARS : 0) |
            (f->bar_words_capable ? EXTGPU_CAP_BAR_WORDS : 0) | f->window_capabilities |
            (f->vm_segments_capable ? EXTGPU_CAP_VM_DMA_SEGMENTS : 0) |
            (f->vm_dynamic_capable ? EXTGPU_CAP_VM_DYNAMIC_DMA : 0),
            EXTGPU_MAX_DMA_REGIONS, EXTGPU_MAX_PREPARED_DMA_BYTES,
            EXTGPU_MAX_RETURNED_SEGMENTS, EXTGPU_PROTOCOL_FINGERPRINT};
        return reply(out, count, values, 8);
    }
    case EXTGPU_SELECTOR_GET_RECOVERY_ENDPOINT: {
        CHECK(n == 1 && in[0] == f->generation);
        uint64_t endpoint[3] = {0xb001, EXTGPU_PROTOCOL_MAJOR, EXTGPU_PROTOCOL_MINOR};
        return reply(out, count, endpoint, 3);
    }
    case EXTGPU_SELECTOR_CLAIM:
        CHECK(!n && !f->owner);
        if (f->claim_error) return f->claim_error;
        f->owner = true; f->dma_bits = 32;
        *count = f->malformed_claim ? 1 : 0; return kIOReturnSuccess;
    case EXTGPU_SELECTOR_RELEASE_OWNER:
        CHECK(n == 1 && in[0] == f->generation && !f->registered && !f->allocated);
        if (!f->owner) return kIOReturnNotOpen;
        f->owner = false; ++f->generation;
        if (f->release_error) { int32_t result = f->release_error; f->release_error = 0; return result; }
        *count = f->malformed_release ? 1 : 0; f->malformed_release = false;
        return kIOReturnSuccess;
    case EXTGPU_SELECTOR_BAR_WORDS: {
        CHECK(f->bar_words_capable && f->owner && n >= 4 && in[0] == 9 && in[1] == 8);
        if (in[2] == EXTGPU_BAR_WORDS_FILL) {
            const unsigned expected = f->bar_fill_count ? f->bar_fill_count : EXTGPU_BAR_WORDS_MAX_FILL;
            CHECK(n == 5 && in[3] == expected && in[4] == UINT64_C(0x1234));
            return reply(out, count, NULL, 0);
        }
        if (in[2] == EXTGPU_BAR_WORDS_WRITE) {
            CHECK(n == 6 && in[3] == 2 && in[4] == 0x1234 && in[5] == 0x5678);
            return reply(out, count, NULL, 0);
        }
        CHECK(in[2] == EXTGPU_BAR_WORDS_READ && in[3] == 2 && n == 4);
        const uint64_t data[] = {0x1122, 0x3344};
        return reply(out, count, data, f->bar_words_short_reply ? 1 : 2);
    }
    case EXTGPU_SELECTOR_RESTORE_VEGA_BARS: {
        CHECK(f->owner && f->bar_restore_capable && n == 2 && in[0] == f->generation && in[1] == 9101);
        CHECK(!f->registered && !f->allocated);
        if (f->bar_restore_native_error) return f->bar_restore_native_error;
        uint64_t values[] = {in[0], f->bar_restore_wrong_provider ? 9102 : 9101};
        return reply(out, count, values, 2);
    }
    case EXTGPU_SELECTOR_VM_FLR_PREFLIGHT: {
        CHECK(f->owner && f->flr_preflight_capable && n == 1 && in[0] == f->generation);
        CHECK(!f->registered && !f->allocated);
        uint64_t values[] = {f->generation, f->flr_wrong_provider ? 9102 : 9101, f->flr_advertised};
        return reply(out, count, values, 3);
    }
    case EXTGPU_SELECTOR_WARM_RELEASE_VM_FLR:
    case EXTGPU_SELECTOR_WARM_RELEASE_VM_RTX3090:
    case EXTGPU_SELECTOR_WARM_RELEASE_VM_POLARIS:
    case EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA: {
        bool group = selector != EXTGPU_SELECTOR_WARM_RELEASE_VM_FLR;
        bool supported;
        switch (selector) {
        case EXTGPU_SELECTOR_WARM_RELEASE_VM_RTX3090: supported = f->warm_rtx3090_capable; break;
        case EXTGPU_SELECTOR_WARM_RELEASE_VM_POLARIS: supported = f->warm_polaris_capable; break;
        case EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA: supported = f->warm_vega_capable; break;
        default: supported = f->warm_capable; break;
        }
        CHECK(supported && n == (group ? 6 : 3) && in[1] == f->key && in[2] == f->bytes);
        if (group) CHECK(in[3] == 9100 && in[4] == 9101 && in[5] == 9102);
        const bool no_dma = !in[2];
        if (no_dma) CHECK(f->warm_no_dma_capable && !in[1] && !f->registered && !f->allocated);
        if (!group && f->warm_unsupported) return kIOReturnUnsupported;
        if (f->warm_waiting_for_transactions) {
            CHECK(!f->warm_closed && f->owner && (no_dma || f->registered || f->registration_rolled_back));
            return kIOReturnNotReady;
        }
        if (f->warm_waiting_for_close) {
            CHECK(!f->warm_closed && f->owner && (f->registered || f->warm_dma_retired));
            f->registered = false;
            f->warm_dma_retired = true;
            return kIOReturnNotReady;
        }
        if (!f->warm_closed) {
            CHECK(f->owner && (no_dma || f->registered || f->warm_dma_retired ||
                  (f->warm_startup_capable && f->registration_rolled_back)) &&
                  in[0] == f->generation);
            f->warm_generation = f->generation++;
            f->warm_selector = selector;
            f->owner = false;
            f->registered = false;
            f->vm_dynamic_regions = 0;
            f->warm_closed = true;
            if (f->warm_lost_reply) return kIOReturnIPCError;
        }
        CHECK(in[0] == f->warm_generation && f->warm_selector == selector);
        uint64_t values[7] = {0};
        memcpy(values, in, n * sizeof(*in));
        values[n] = EXTGPU_WARM_VM_CLOSED;
        if (f->warm_malformed_reply) { values[2]++; f->warm_malformed_reply = false; }
        return reply(out, count, values, n + 1);
    }
    case EXTGPU_SELECTOR_RTX3090_RELEASE_RECEIPT:
    case EXTGPU_SELECTOR_POLARIS_RELEASE_RECEIPT:
    case EXTGPU_SELECTOR_VEGA_RELEASE_RECEIPT: {
        uint32_t method;
        switch (selector) {
        case EXTGPU_SELECTOR_RTX3090_RELEASE_RECEIPT:
            CHECK(f->warm_rtx3090_capable);
            method = EXTGPU_SELECTOR_WARM_RELEASE_VM_RTX3090;
            break;
        case EXTGPU_SELECTOR_POLARIS_RELEASE_RECEIPT:
            CHECK(f->warm_polaris_capable);
            method = EXTGPU_SELECTOR_WARM_RELEASE_VM_POLARIS;
            break;
        default:
            CHECK(f->vega_receipt_capable);
            method = EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA;
            break;
        }
        CHECK(n == EXTGPU_WARM_VEGA_REQUEST_WORDS);
        CHECK(in[1] == f->key && in[2] == f->bytes && in[3] == 9100 && in[4] == 9101 && in[5] == 9102);
        if (!f->warm_closed) return kIOReturnNotReady;
        CHECK(f->warm_selector == method);
        CHECK(in[0] == f->warm_generation && !f->owner && !f->registered);
        uint64_t values[EXTGPU_WARM_VEGA_REPLY_WORDS];
        memcpy(values, in, n * sizeof(*in));
        if (f->vega_bad_receipt_once) { values[4]++; f->vega_bad_receipt_once = false; }
        values[n] = EXTGPU_WARM_VM_CLOSED;
        return reply(out, count, values, n + 1);
    }
    case EXTGPU_SELECTOR_PROBE_VEGA_PERST: {
        CHECK(f->perst_capable && n == 6 && !in[1] && !in[2] && !f->registered && !f->allocated);
        CHECK(in[3] == 9100 && in[4] == 9101 && in[5] == 9102);
        if (!f->warm_closed) {
            CHECK(f->owner && in[0] == f->generation);
            f->warm_generation = f->generation++;
            f->owner = false;
            f->warm_closed = true;
            if (f->warm_lost_reply) return kIOReturnIPCError;
        }
        CHECK(in[0] == f->warm_generation);
        uint64_t values[8] = {0};
        memcpy(values, in, n * sizeof(*in));
        values[6] = EXTGPU_WARM_VM_CLOSED;
        values[7] = f->perst_native_error ? EXTGPU_VEGA_PERST_NATIVE_ERROR | (uint32_t)f->perst_native_error :
            f->perst_unsupported ? EXTGPU_VEGA_PERST_UNSUPPORTED : EXTGPU_VEGA_PERST_RESET;
        return reply(out, count, values, 8);
    }
    case EXTGPU_SELECTOR_RESET_DEVICE:
        CHECK(f->owner && !f->registered && !n);
        ++f->generation; f->dma_bits = 32;
        f->query_fails_once = f->fail_query_after_reset;
        return reply(out, count, NULL, 0);
    case EXTGPU_SELECTOR_SET_DMA_ADDRESS_BITS:
        CHECK(f->owner && !f->registered && n == 1);
        f->dma_bits = (uint32_t)in[0]; return reply(out, count, NULL, 0);
    case EXTGPU_SELECTOR_CONFIG_READ: {
        CHECK(f->owner && n == 2);
        uint64_t value = UINT64_C(0x687f1002);
        if (in[0] == 4 && in[1] == 2) {
            if (f->command_read_pending_replies) {
                --f->command_read_pending_replies;
                return kIOReturnNotReady;
            }
            value = f->command_read_all_ones ? UINT16_MAX : f->pci_command;
        }
        return reply(out, count, &value, 1);
    }
    case EXTGPU_SELECTOR_CONFIG_WRITE:
        CHECK(f->owner && n == 3);
        if (in[0] == 4 && in[1] == 2) {
            if (f->command_write_pending_replies) {
                --f->command_write_pending_replies;
                return kIOReturnNotReady;
            }
            if (!f->ignore_bus_master_clear) f->pci_command = (uint16_t)in[2];
        }
        return reply(out, count, NULL, 0);
    case EXTGPU_SELECTOR_REGISTER_DMA_REGION: {
        CHECK(f->owner && !f->registered && n == 3);
        f->registered = !f->registration_rolled_back; f->key = in[0]; f->bytes = in[2];
        if (f->uncertain_register) return kIOReturnError;
        f->bus = f->high_bus ? UINT64_C(0x100000000) : UINT64_C(0x100000);
        uint64_t values[3] = {1, f->bus, in[2] + (f->oversized_segment ? 4096 : 0)};
        return reply(out, count, values, 3);
    }
    case EXTGPU_SELECTOR_REGISTER_VM_DMA: {
        CHECK(f->vm_segments_capable && f->owner && !f->registered && n == 3 && in[2] == 32768);
        f->registered = true;
        f->key = in[0]; f->bytes = in[2]; f->bus = UINT64_C(0x80000000);
        const uint64_t values[] = {2, f->bus, f->bytes};
        return reply(out, count, values, 3);
    }
    case EXTGPU_SELECTOR_GET_VM_DMA_SEGMENT: {
        CHECK(f->registered && n == 3 && in[0] == f->generation && in[1] == f->key && in[2] < 2);
        if (in[2] && f->vm_segment_read_fails) return kIOReturnStillOpen;
        const uint64_t values[] = {in[2] && !f->vm_segments_overlap ? UINT64_C(0xc0000000) : f->bus, 16384};
        return reply(out, count, values, 2);
    }
    case EXTGPU_SELECTOR_ENABLE_VM_DYNAMIC_DMA:
        CHECK(f->vm_dynamic_capable && f->owner && f->registered &&
              n == 3 && in[0] == f->generation && in[1] == f->key && in[2] == f->bytes);
        f->vm_dynamic_enabled = true;
        return reply(out, count, in, n);
    case EXTGPU_SELECTOR_REGISTER_VM_DYNAMIC_DMA: {
        CHECK(f->vm_dynamic_enabled && f->owner && f->registered && n == 3 && in[0] >= (UINT64_C(1) << 60));
        ++f->vm_dynamic_regions;
        const uint64_t values[] = {1, UINT64_C(0xc0000000) + f->vm_dynamic_regions * 16384, in[2]};
        return reply(out, count, values, 3);
    }
    case EXTGPU_SELECTOR_ALLOCATE_DMA_BUFFER: {
        CHECK(f->owner && !f->allocated && n == 2);
        f->allocated = true;
        uint64_t values[6] = {in[0], 0, 1, UINT64_C(0x100001), in[0], 0x030000};
        return reply(out, count, values, 6); /* Exact size but violates requested alignment. */
    }
    case EXTGPU_SELECTOR_FREE_DMA_BUFFER:
        CHECK(f->owner && f->allocated && !n);
        f->allocated = false; return reply(out, count, NULL, 0);
    case EXTGPU_SELECTOR_UNREGISTER_DMA_REGION:
        if (n == 1 && in[0] >= (UINT64_C(1) << 60)) {
            CHECK(f->vm_dynamic_enabled && f->vm_dynamic_regions);
            --f->vm_dynamic_regions;
            return reply(out, count, NULL, 0);
        }
        CHECK(f->registered && n == 1 && in[0] == f->key);
        if (f->fail_retire) return kIOReturnBusy;
        f->registered = false; return reply(out, count, NULL, 0);
    case EXTGPU_SELECTOR_SETUP_INTERRUPTS: {
        CHECK(f->owner && !n);
        uint64_t vectors = 2; reply(out, count, &vectors, 1);
        if (f->malformed_irq) *count = 0;
        return kIOReturnSuccess;
    }
    case EXTGPU_SELECTOR_RETIRE_INTERRUPTS:
        CHECK(f->owner && !n);
        if (f->require_early_bus_master_clear) CHECK(f->pci_command == 0x402);
        f->irq_retired = true;
        if (f->irq_retire_error) return f->irq_retire_error;
        if (f->irq_retire_pending_replies) {
            --f->irq_retire_pending_replies;
            return kIOReturnNotReady;
        }
        return reply(out, count, NULL, 0);
    case EXTGPU_SELECTOR_SET_IRQ_MASK:
        CHECK(f->owner && n == 4);
        if (f->irq_retired) return kIOReturnNotReady;
        if (f->irq_mask_pending_replies) {
            --f->irq_mask_pending_replies;
            return kIOReturnNotReady;
        }
        if (f->fail_irq_mask) return kIOReturnBusy;
        return reply(out, count, NULL, 0);
    case EXTGPU_SELECTOR_CHECK_INTERRUPT: {
        CHECK(f->owner && !n);
        if (f->irq_retired) return kIOReturnNotReady;
        if (f->irq_drain_pending_replies) {
            --f->irq_drain_pending_replies;
            return kIOReturnNotReady;
        }
        uint64_t pending[4] = {0}; return reply(out, count, pending, 4);
    }
    default: CHECK(false); return kIOReturnUnsupported;
    }
}
static int32_t no_map(void *ctx, uint32_t type, uint64_t *address, uint64_t *size, uint32_t options)
{ (void)ctx; (void)type; (void)address; (void)size; (void)options; CHECK(false); return kIOReturnUnsupported; }
static int32_t no_unmap(void *ctx, uint32_t type, uint64_t address)
{
    fixture *f = ctx;
    CHECK(f->window && type == EXTGPU_MEMORY_MMIO_WINDOW && address == (uintptr_t)f->window);
    ++f->window_unmaps;
    return kIOReturnSuccess;
}
static int32_t closed(void *ctx)
{
    fixture *f = ctx; CHECK(!f->owner && !f->registered && !f->allocated);
    ++f->closes;
    if (f->original_closed) { ++f->stale_name_calls; return kIOReturnSuccess; } // Recycled-name hazard.
    f->original_closed = true; // IOServiceClose consumes the name even on failure.
    return f->close_error ? f->close_error : kIOReturnSuccess;
}
static const extgpu_test_transport transport = {call, no_map, no_unmap, closed};
static void open_fixture(fixture *f, extgpu_session **s, extgpu_device **d)
{ memset(f, 0, sizeof(*f)); f->generation = 71; CHECK(extgpu_test_open(&transport, f, s, d) == EXTGPU_OK); }
static void finish(extgpu_session **s, extgpu_device **d)
{ CHECK(extgpu_device_close(d) == EXTGPU_OK && !*d); CHECK(extgpu_session_destroy(s) == EXTGPU_OK && !*s); }

static void failed_and_uncertain_claims(void)
{
    fixture f; extgpu_session *s = NULL; extgpu_device *d = NULL;
    open_fixture(&f, &s, &d); f.claim_error = kIOReturnNoMemory;
    CHECK(extgpu_device_claim(d) == EXTGPU_E_MEMORY && !f.owner);
    finish(&s, &d); /* A proven preflight error does not poison an unclaimed handle. */

    open_fixture(&f, &s, &d); f.claim_error = kIOReturnIPCError;
    CHECK(extgpu_device_claim(d) == EXTGPU_E_UNSAFE && !f.owner);
    CHECK(extgpu_config_write(d, 4, 2, 2) == EXTGPU_E_UNSAFE);
    CHECK(!f.calls[EXTGPU_SELECTOR_CONFIG_WRITE]);
    CHECK(extgpu_device_close(&d) == EXTGPU_E_BUSY);
    f.query_fails_once = true; f.query_error = kIOReturnNotOpen;
    CHECK(extgpu_device_release(d) == EXTGPU_E_NOT_CLAIMED);
    CHECK(!f.calls[EXTGPU_SELECTOR_RELEASE_OWNER]);
    CHECK(extgpu_device_close(&d) == EXTGPU_E_BUSY); /* Query error cannot establish absence of ownership. */
    CHECK(extgpu_device_release(d) == EXTGPU_E_NOT_CLAIMED);
    finish(&s, &d); /* Fresh generation-checked NOT_OPEN establishes absence, not successful release. */

    open_fixture(&f, &s, &d); f.malformed_claim = true;
    CHECK(extgpu_device_claim(d) == EXTGPU_E_UNSAFE && f.owner);
    CHECK(extgpu_config_write(d, 4, 2, 2) == EXTGPU_E_UNSAFE);
    CHECK(extgpu_device_release(d) == EXTGPU_OK && !f.owner);
    finish(&s, &d);
}
static void epoch_recovery(void)
{
    fixture f; extgpu_session *s = NULL; extgpu_device *d = NULL;
    open_fixture(&f, &s, &d); CHECK(extgpu_device_claim(d) == EXTGPU_OK);
    f.fail_query_after_reset = true;
    CHECK(extgpu_device_reset(d) != EXTGPU_OK && f.generation == 72);
    extgpu_protocol_info protocol = {.major = 999};
    CHECK(extgpu_device_protocol(d, &protocol) == EXTGPU_E_STALE && protocol.major == 999);
    CHECK(extgpu_device_release(d) == EXTGPU_OK && !f.owner);
    finish(&s, &d);

    for (unsigned mode = 0; mode < 2; ++mode) {
        open_fixture(&f, &s, &d); CHECK(extgpu_device_claim(d) == EXTGPU_OK);
        f.malformed_release = mode == 0; f.release_error = mode == 1 ? kIOReturnIPCError : 0;
        CHECK(extgpu_device_release(d) == EXTGPU_E_UNSAFE && !f.owner);
        CHECK(extgpu_device_protocol(d, &protocol) == EXTGPU_E_STALE);
        CHECK(extgpu_device_close(&d) == EXTGPU_E_BUSY);
        CHECK(extgpu_device_release(d) == EXTGPU_E_NOT_CLAIMED);
        finish(&s, &d);
    }
}
static void coverage_and_reservations(void)
{
    uint64_t page = (uint64_t)sysconf(_SC_PAGESIZE);
    void *backing = NULL; CHECK(page && posix_memalign(&backing, (size_t)page, (size_t)page * 2) == 0);
    fixture f; extgpu_session *s = NULL; extgpu_device *d = NULL;
    extgpu_buffer *b = NULL, *overlap = NULL; extgpu_dma_coverage coverage;
    open_fixture(&f, &s, &d); CHECK(extgpu_device_claim(d) == EXTGPU_OK);
    f.oversized_segment = true;
    CHECK(extgpu_dma_register(d, page * 64, backing, page, &b, &coverage) == EXTGPU_E_COVERAGE);
    CHECK(!b && !f.registered && f.calls[EXTGPU_SELECTOR_UNREGISTER_DMA_REGION] == 1);
    f.oversized_segment = false; f.high_bus = true;
    CHECK(extgpu_dma_register(d, page * 64, backing, page, &b, &coverage) == EXTGPU_E_COVERAGE && !b);
    CHECK(extgpu_dma_allocate(d, page, 65536, &b, &coverage) == EXTGPU_E_COVERAGE);
    CHECK(!b && !f.allocated && f.calls[EXTGPU_SELECTOR_FREE_DMA_BUFFER] == 1);
    CHECK(extgpu_device_release(d) == EXTGPU_OK); finish(&s, &d);

    open_fixture(&f, &s, &d); CHECK(extgpu_device_claim(d) == EXTGPU_OK);
    f.high_bus = true;
    CHECK(extgpu_device_set_dma_address_bits(d, 64) == EXTGPU_OK);
    CHECK(extgpu_dma_register(d, page * 64, backing, page, &b, &coverage) == EXTGPU_OK);
    CHECK(coverage.complete && coverage.bus_address == UINT64_C(0x100000000));
    CHECK(extgpu_buffer_unregister(&b) == EXTGPU_OK);
    CHECK(extgpu_device_release(d) == EXTGPU_OK); finish(&s, &d);

    open_fixture(&f, &s, &d); CHECK(extgpu_device_claim(d) == EXTGPU_OK);
    f.uncertain_register = true;
    CHECK(extgpu_dma_register(d, page * 64, backing, page * 2, &b, &coverage) == EXTGPU_E_UNSAFE && b);
    CHECK(!f.calls[EXTGPU_SELECTOR_UNREGISTER_DMA_REGION]);
    CHECK(extgpu_dma_register(d, page * 65, backing, page, &overlap, &coverage) == EXTGPU_E_BUSY && !overlap);
    CHECK(f.calls[EXTGPU_SELECTOR_REGISTER_DMA_REGION] == 1);
    f.fail_retire = true; CHECK(extgpu_buffer_unregister(&b) == EXTGPU_E_UNSAFE && b);
    f.fail_retire = false; CHECK(extgpu_buffer_unregister(&b) == EXTGPU_OK && !b);
    CHECK(extgpu_device_release(d) == EXTGPU_OK); finish(&s, &d);
    free(backing);
}
static void unexpected_irq(extgpu_irq *irq, void *context)
{ (void)irq; (void)context; CHECK(false); }
static void malformed_irq_success(void)
{
    fixture f; extgpu_session *s = NULL; extgpu_device *d = NULL;
    open_fixture(&f, &s, &d); CHECK(extgpu_device_claim(d) == EXTGPU_OK); f.malformed_irq = true;
    extgpu_irq *irq = NULL; uint32_t vectors = 99;
    CHECK(extgpu_irq_create(d, unexpected_irq, NULL, &irq, &vectors) == EXTGPU_E_UNSAFE && irq && !vectors);
    CHECK(extgpu_device_release(d) == EXTGPU_E_BUSY);
    f.fail_irq_mask = true;
    f.irq_retired = true; /* Malformed setup may already have removed the buffer. */
    f.irq_retire_pending_replies = 1;
    CHECK(extgpu_irq_close(&irq) == EXTGPU_E_UNSAFE && irq);
    extgpu_error_info error_info;
    CHECK(extgpu_device_last_error(d, &error_info) == EXTGPU_OK);
    CHECK(error_info.status == EXTGPU_E_UNSAFE && error_info.native_code == kIOReturnNotReady);
    f.fail_irq_mask = false;
    CHECK(f.irq_retired && f.calls[EXTGPU_SELECTOR_RETIRE_INTERRUPTS] == 1);
    CHECK(f.calls[EXTGPU_SELECTOR_SET_IRQ_MASK] == 0);
    const unsigned drains = f.calls[EXTGPU_SELECTOR_CHECK_INTERRUPT];
    CHECK(extgpu_irq_close(&irq) == EXTGPU_OK && !irq);
    CHECK(f.calls[EXTGPU_SELECTOR_RETIRE_INTERRUPTS] == 2);
    CHECK(f.calls[EXTGPU_SELECTOR_CHECK_INTERRUPT] == drains);
    CHECK(extgpu_device_release(d) == EXTGPU_OK); finish(&s, &d);
}
static void consumed_ordinary_close_reference_never_reaches_recycled_name(void)
{
    fixture f; extgpu_session *s = NULL; extgpu_device *d = NULL;
    open_fixture(&f, &s, &d);
    CHECK(extgpu_device_claim(d) == EXTGPU_OK);
    CHECK(extgpu_device_close(&d) == EXTGPU_E_BUSY && f.closes == 0); // Owner must be released first.
    CHECK(extgpu_device_release(d) == EXTGPU_OK);
    f.close_error = kIOReturnIPCError;
    CHECK(extgpu_device_close(&d) == EXTGPU_E_IO && d && f.closes == 1 && f.original_closed);
    CHECK(extgpu_session_destroy(&s) == EXTGPU_E_BUSY && s);
    unsigned calls[32]; memcpy(calls, f.calls, sizeof(calls));
    extgpu_protocol_info protocol = {.major = 999}; extgpu_pci_identity identity = {.vendor_id = 999};
    CHECK(extgpu_device_protocol(d, &protocol) == EXTGPU_E_UNSAFE && protocol.major == 999);
    CHECK(extgpu_device_identity(d, &identity) == EXTGPU_E_UNSAFE && identity.vendor_id == 999);
    CHECK(extgpu_device_claim(d) == EXTGPU_E_UNSAFE);
    CHECK(extgpu_device_release(d) == EXTGPU_E_UNSAFE);
    CHECK(extgpu_device_reset(d) == EXTGPU_E_UNSAFE);
    CHECK(!memcmp(calls, f.calls, sizeof(calls)) && !f.stale_name_calls);
    f.close_error = 0;
    CHECK(extgpu_device_close(&d) == EXTGPU_OK && !d && f.closes == 1 && !f.stale_name_calls);
    CHECK(extgpu_session_destroy(&s) == EXTGPU_OK && !s);
}
static int32_t broker_lookup(void *ctx, uint64_t expected, uint64_t *actual, const char **name)
{ (void)ctx; CHECK(expected == 0xb001); *actual = expected; *name = EXTGPU_RECOVERY_BROKER_CLASS; return kIOReturnSuccess; }
static int32_t broker_open(void *ctx)
{ ++((fixture *)ctx)->broker_opens; return kIOReturnSuccess; }
static int32_t broker_bad_query(void *ctx, uint32_t selector, const uint64_t *input,
    uint32_t input_count, uint64_t *out, uint32_t *out_count)
{
    (void)ctx;
    CHECK(selector == EXTGPU_SELECTOR_QUERY_PROTOCOL && input_count == 3 && input[0] == EXTGPU_PROTOCOL_MAJOR);
    uint64_t values[8] = {EXTGPU_PROTOCOL_MAJOR, EXTGPU_PROTOCOL_MINOR, 0xb001,
        EXTGPU_RECOVERY_BROKER_CAPABILITIES | EXTGPU_CAP_CONFIG_WRITE, EXTGPU_MAX_DMA_REGIONS,
        EXTGPU_MAX_PREPARED_DMA_BYTES, EXTGPU_MAX_RETURNED_SEGMENTS, EXTGPU_PROTOCOL_FINGERPRINT};
    return reply(out, out_count, values, 8); // Broker preflight must fail before PREPARE.
}
static int32_t broker_close(void *ctx)
{ ++((fixture *)ctx)->broker_closes; return kIOReturnSuccess; }
static int32_t no_cold_local_drop(void *ctx)
{ (void)ctx; CHECK(false); return kIOReturnError; }
static void broker_preflight_failure_then_ordinary_close_error_consumes_once(void)
{
    fixture f; extgpu_session *s = NULL; extgpu_device *d = NULL;
    open_fixture(&f, &s, &d); f.broker_capable = true;
    CHECK(extgpu_test_set_registry_ids(d, 5001, 9001) == EXTGPU_OK);
    const extgpu_test_transport broker = {broker_bad_query, NULL, NULL, broker_close};
    const extgpu_test_recovery_hooks hooks = {broker_lookup, broker_open, no_cold_local_drop};
    CHECK(extgpu_test_set_recovery_transport(d, &broker, &hooks, &f) == EXTGPU_OK);
    CHECK(extgpu_device_claim(d) == EXTGPU_OK);
    extgpu_cold_removal_info info;
    CHECK(extgpu_cold_removal_begin(d, &info) == EXTGPU_E_PROTOCOL);
    CHECK(f.broker_opens == 1 && !f.calls[EXTGPU_SELECTOR_PREPARE_RECOVERY_ESCROW] && !f.calls[EXTGPU_SELECTOR_COLD_BEGIN]);
    CHECK(extgpu_device_release(d) == EXTGPU_OK);
    f.close_error = kIOReturnIPCError;
    CHECK(extgpu_device_close(&d) == EXTGPU_E_IO && d && f.closes == 1 && f.broker_closes == 1);
    f.close_error = 0;
    CHECK(extgpu_device_close(&d) == EXTGPU_OK && !d);
    CHECK(f.closes == 1 && f.broker_closes == 1 && !f.stale_name_calls);
    CHECK(extgpu_session_destroy(&s) == EXTGPU_OK && !s);
}
static void warm_window_retirement(void)
{
    void *ram = NULL;
    CHECK(posix_memalign(&ram, 16384, 32768) == 0);
    for (unsigned mode = 0; mode < 5; ++mode) {
        fixture f = {.generation = 71, .warm_capable = mode != 0,
                     .warm_unsupported = mode == 1, .warm_lost_reply = mode == 3,
                     .warm_malformed_reply = mode == 4};
        extgpu_session *s = NULL;
        extgpu_device *d = NULL;
        extgpu_buffer *window = NULL;
        extgpu_dma_coverage coverage;
        CHECK(extgpu_test_open(&transport, &f, &s, &d) == EXTGPU_OK);
        CHECK(extgpu_device_claim(d) == EXTGPU_OK);
        CHECK(extgpu_dma_register(d, 0x40000000, ram, 32768, &window, &coverage) == EXTGPU_OK);
        if (mode <= 1) {
            CHECK(extgpu_warm_release_vm_flr(d, &window) == EXTGPU_E_UNSUPPORTED);
            CHECK(f.calls[EXTGPU_SELECTOR_WARM_RELEASE_VM_FLR] == mode && window && f.registered);
            CHECK(extgpu_config_write(d, 4, 2, 0) == EXTGPU_OK);
            CHECK(extgpu_buffer_unregister(&window) == EXTGPU_OK);
            CHECK(extgpu_device_release(d) == EXTGPU_OK);
        } else if (mode == 2) {
            CHECK(extgpu_warm_release_vm_flr(d, &window) == EXTGPU_OK);
        } else {
            extgpu_buffer *original = window;
            CHECK(extgpu_warm_release_vm_flr(d, &window) == EXTGPU_E_UNSAFE);
            CHECK(window == original && f.warm_closed);
            CHECK(extgpu_buffer_address(window) == NULL);
            CHECK(extgpu_buffer_unregister(&window) == EXTGPU_E_UNSAFE);
            CHECK(extgpu_config_write(d, 4, 2, 6) == EXTGPU_E_UNSAFE);
            CHECK(extgpu_device_protocol(d, &(extgpu_protocol_info){0}) == EXTGPU_E_UNSAFE);
            CHECK(extgpu_device_close(&d) == EXTGPU_E_BUSY);
            CHECK(!f.calls[EXTGPU_SELECTOR_UNREGISTER_DMA_REGION]);
            CHECK(!f.calls[EXTGPU_SELECTOR_CONFIG_WRITE]);
            CHECK(extgpu_warm_release_vm_flr(d, &window) == EXTGPU_OK);
            CHECK(f.calls[EXTGPU_SELECTOR_WARM_RELEASE_VM_FLR] == 2);
        }
        CHECK(!window && !f.owner && !f.registered);
        finish(&s, &d);
    }
    free(ram);
    puts("PASS FLR VM window retirement, unsupported preflight and same-handle acknowledgement replay");
}

static void vm_shutdown_requires_hardware_ack_before_unmap(void)
{
    fixture f = {.generation = 71, .warm_capable = true, .warm_lost_reply = true};
    struct op_gpu_lease lease = {0};
    void *ram = NULL;
    extgpu_dma_coverage coverage;
    CHECK(posix_memalign(&ram, 16384, 16384) == 0);
    CHECK(extgpu_test_open(&transport, &f, &lease.session, &lease.device) == EXTGPU_OK);
    CHECK(extgpu_device_claim(lease.device) == EXTGPU_OK);
    CHECK(extgpu_dma_register(lease.device, 0x40000000, ram, 16384, &lease.window, &coverage) == EXTGPU_OK);
    CHECK(op_gpu_shutdown(&lease) == EXTGPU_E_BUSY && !op_gpu_ram_releasable(&lease));
    CHECK(!f.calls[EXTGPU_SELECTOR_WARM_RELEASE_VM_FLR]);
    lease.stop_acknowledged = true;
    CHECK(op_gpu_shutdown(&lease) == EXTGPU_E_UNSAFE);
    CHECK(lease.window && lease.device && lease.session && !op_gpu_ram_releasable(&lease));
    CHECK(!op_gpu_shutdown_waiting(&lease) && !f.closes);
    unsigned calls = f.calls[EXTGPU_SELECTOR_WARM_RELEASE_VM_FLR];
    f.close_error = kIOReturnIPCError;
    CHECK(op_gpu_shutdown(&lease) == EXTGPU_OK && op_gpu_ram_releasable(&lease));
    CHECK(op_gpu_shutdown(&lease) == EXTGPU_OK && !op_gpu_shutdown_waiting(&lease));
    CHECK(f.calls[EXTGPU_SELECTOR_WARM_RELEASE_VM_FLR] == calls + 1 && f.closes == 1);
    CHECK(!f.stale_name_calls);
    free(ram);
    puts("PASS confirmed VM release finalizes local IPC state after a close error");
}

static void vm_startup_without_dma_ack_requires_scoped_warm_release(void)
{
    void *ram = NULL;
    CHECK(posix_memalign(&ram, 16384, 16384) == 0);
    for (unsigned mode = 0; mode < 3; ++mode) {
        bool vega = mode != 1;
        fixture f = {.generation = 71, .warm_capable = !vega, .warm_vega_capable = vega,
                     .warm_startup_capable = true, .warm_no_dma_capable = true, .warm_lost_reply = true,
                     .oversized_segment = mode == 0, .uncertain_register = mode != 0,
                     .registration_rolled_back = mode == 1};
        struct op_gpu_lease lease = {.release_kind = vega ? OP_GPU_RELEASE_VEGA : OP_GPU_RELEASE_FLR};
        extgpu_dma_coverage coverage;
        uint32_t release = vega ? EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA : EXTGPU_SELECTOR_WARM_RELEASE_VM_FLR;
        CHECK(extgpu_test_open(&transport, &f, &lease.session, &lease.device) == EXTGPU_OK);
        CHECK(extgpu_test_set_registry_ids(lease.device, 9001, 9101) == EXTGPU_OK);
        if (vega) CHECK(extgpu_test_set_reset_scope(lease.device, 9100, 9102) == EXTGPU_OK);
        CHECK(extgpu_device_claim(lease.device) == EXTGPU_OK);
        CHECK(extgpu_vm_ram_register(lease.device, 0x40000000, ram, 16384, &lease.window, &coverage) == EXTGPU_E_UNSAFE);
        extgpu_buffer *original = lease.window;
        CHECK(original && !coverage.complete && coverage.requested_iova == 0x40000000 && coverage.requested_bytes == 16384);
        if (!vega) CHECK(extgpu_warm_release_vm_no_dma(lease.device) == EXTGPU_E_BUSY);
        CHECK(!extgpu_buffer_address(original) && !f.calls[EXTGPU_SELECTOR_UNREGISTER_DMA_REGION]);
        CHECK(extgpu_buffer_unregister(&lease.window) == EXTGPU_E_UNSAFE && lease.window == original);
        CHECK(op_gpu_shutdown(&lease) == EXTGPU_E_BUSY && !f.calls[release]);
        lease.stop_acknowledged = true;
        CHECK(op_gpu_shutdown(&lease) == EXTGPU_E_UNSAFE && f.warm_closed);
        CHECK(lease.window == original && !op_gpu_ram_releasable(&lease));
        CHECK(op_gpu_shutdown(&lease) == EXTGPU_OK && op_gpu_ram_releasable(&lease));
        CHECK(!f.calls[EXTGPU_SELECTOR_UNREGISTER_DMA_REGION] && !f.calls[EXTGPU_SELECTOR_RELEASE_OWNER]);
        CHECK(f.calls[release] == 2 && f.generation == 72 && f.closes == 1);
    }
    free(ram);
    puts("PASS uncertain VM RAM registration retires only through exact scoped release");
}

static void vm_shutdown_clears_bus_master_before_retiring_interrupts(void)
{
    const unsigned pending_replies = 12;
    fixture f = {.generation = 71, .warm_capable = true,
                 .pci_command = 0x406, .command_read_all_ones = true,
                 .ignore_bus_master_clear = true, .require_early_bus_master_clear = true,
                 .irq_retire_pending_replies = pending_replies};
    struct op_gpu_lease lease = {0};
    void *ram = NULL;
    extgpu_dma_coverage coverage;
    uint32_t vectors = 0;
    CHECK(posix_memalign(&ram, 16384, 16384) == 0);
    CHECK(extgpu_test_open(&transport, &f, &lease.session, &lease.device) == EXTGPU_OK);
    CHECK(extgpu_device_claim(lease.device) == EXTGPU_OK);
    CHECK(extgpu_dma_register(lease.device, 0x40000000, ram, 16384, &lease.window, &coverage) == EXTGPU_OK);
    CHECK(extgpu_irq_create(lease.device, unexpected_irq, NULL, &lease.irq, &vectors) == EXTGPU_E_UNSAFE);
    /* Setup succeeded in the driver fixture. The missing Mach connection
     * fails only async arming, leaving normal mask/drain work for close. */
    CHECK(lease.irq && vectors == 2);

    CHECK(op_gpu_shutdown(&lease) == EXTGPU_E_BUSY);
    CHECK(f.calls[EXTGPU_SELECTOR_CONFIG_READ] == 0);
    lease.stop_acknowledged = true;
    CHECK(op_gpu_shutdown(&lease) == EXTGPU_E_UNSAFE);
    CHECK(!f.calls[EXTGPU_SELECTOR_CONFIG_WRITE]);
    CHECK(!op_gpu_shutdown_waiting(&lease));
    f.command_read_all_ones = false;

    f.command_read_pending_replies = pending_replies;
    for (unsigned i = 0; i < pending_replies; ++i) {
        CHECK(op_gpu_shutdown(&lease) == EXTGPU_E_UNSAFE && op_gpu_shutdown_waiting(&lease));
        CHECK(!lease.bus_master_clear_acknowledged && !f.calls[EXTGPU_SELECTOR_CONFIG_WRITE]);
    }
    f.command_write_pending_replies = pending_replies;
    for (unsigned i = 0; i < pending_replies; ++i) {
        CHECK(op_gpu_shutdown(&lease) == EXTGPU_E_UNSAFE && op_gpu_shutdown_waiting(&lease));
        CHECK(!lease.bus_master_clear_acknowledged && f.pci_command == 0x406);
    }
    CHECK(op_gpu_shutdown(&lease) == EXTGPU_E_UNSAFE);
    CHECK(!op_gpu_shutdown_waiting(&lease));
    CHECK(f.pci_command == 0x406 && f.calls[EXTGPU_SELECTOR_CONFIG_WRITE] == pending_replies + 1);
    CHECK(!f.calls[EXTGPU_SELECTOR_RETIRE_INTERRUPTS] && !f.calls[EXTGPU_SELECTOR_WARM_RELEASE_VM_FLR]);
    CHECK(lease.irq && lease.window && !lease.bus_master_clear_acknowledged && !op_gpu_ram_releasable(&lease));

    f.ignore_bus_master_clear = false;
    f.irq_mask_pending_replies = pending_replies;
    f.irq_drain_pending_replies = pending_replies;
    for (unsigned i = 0; i < 2 * pending_replies; ++i) {
        CHECK(op_gpu_shutdown(&lease) == EXTGPU_E_UNSAFE && op_gpu_shutdown_waiting(&lease));
        CHECK(lease.bus_master_clear_acknowledged && lease.irq && lease.window);
        CHECK(!f.calls[EXTGPU_SELECTOR_RETIRE_INTERRUPTS] && !f.calls[EXTGPU_SELECTOR_WARM_RELEASE_VM_FLR]);
    }
    f.irq_retire_error = kIOReturnIPCError;
    CHECK(op_gpu_shutdown(&lease) == EXTGPU_E_UNSAFE);
    CHECK(!op_gpu_shutdown_waiting(&lease));
    CHECK(f.pci_command == 0x402 && f.calls[EXTGPU_SELECTOR_RETIRE_INTERRUPTS] == 1);
    unsigned config_reads = f.calls[EXTGPU_SELECTOR_CONFIG_READ];
    f.irq_retire_error = 0;
    for (unsigned i = 0; i < pending_replies; ++i) {
        CHECK(op_gpu_shutdown(&lease) == EXTGPU_E_UNSAFE);
        CHECK(op_gpu_shutdown_waiting(&lease) && !op_gpu_ram_releasable(&lease));
        CHECK(lease.bus_master_clear_acknowledged && lease.irq && lease.window);
        CHECK(!f.calls[EXTGPU_SELECTOR_WARM_RELEASE_VM_FLR]);
    }
    CHECK(op_gpu_shutdown(&lease) == EXTGPU_OK && op_gpu_ram_releasable(&lease));
    CHECK(f.calls[EXTGPU_SELECTOR_CONFIG_READ] == config_reads &&
          f.calls[EXTGPU_SELECTOR_CONFIG_WRITE] == pending_replies + 2);
    CHECK(f.calls[EXTGPU_SELECTOR_RETIRE_INTERRUPTS] == pending_replies + 2);
    CHECK(f.calls[EXTGPU_SELECTOR_WARM_RELEASE_VM_FLR] == 1);
    free(ram);
    puts("PASS VM shutdown verifies PCI bus mastering is disabled before IRQ retirement");
    puts("PASS VM waits for original command-register and IRQ mask/drain readiness");
    puts("PASS VM shutdown keeps waiting for pending IRQ retirement on the original lease");
}

static void segmented_vm_dma_requires_exact_coverage_and_scoped_release(void)
{
    void *ram = NULL;
    CHECK(posix_memalign(&ram, 16384, 32768) == 0);
    for (unsigned mode = 0; mode < 3; ++mode) {
        fixture f = {.generation = 71, .warm_capable = true, .warm_startup_capable = true,
            .vm_segments_capable = true, .vm_segments_overlap = mode == 1, .vm_segment_read_fails = mode == 2};
        struct op_gpu_lease lease = {.release_kind = OP_GPU_RELEASE_FLR, .stop_acknowledged = true};
        CHECK(extgpu_test_open(&transport, &f, &lease.session, &lease.device) == EXTGPU_OK);
        CHECK(extgpu_device_claim(lease.device) == EXTGPU_OK);
        extgpu_dma_coverage coverage;
        CHECK(extgpu_vm_ram_register_segmented(lease.device, 0x40000000, ram, 32768,
              &lease.window, &coverage) == (mode ? EXTGPU_E_UNSAFE : EXTGPU_OK));
        CHECK(lease.window && f.registered);
        size_t count = 0;
        if (mode) {
            CHECK(!coverage.complete && coverage.requested_bytes == 32768);
            CHECK(extgpu_vm_ram_segments(lease.window, NULL, 0, &count) == EXTGPU_E_UNSAFE);
        } else {
            extgpu_vm_dma_segment segments[2] = {{77, 88, 99}};
            CHECK(coverage.complete && coverage.segment_count == 2 && coverage.covered_bytes == 32768);
            CHECK(extgpu_vm_ram_segments(lease.window, NULL, 0, &count) == EXTGPU_OK && count == 2);
            CHECK(extgpu_vm_ram_segments(lease.window, segments, 1, &count) == EXTGPU_E_CAPACITY);
            CHECK(segments[0].buffer_offset == 77);
            CHECK(extgpu_vm_ram_segments(lease.window, segments, 2, &count) == EXTGPU_OK);
            CHECK(segments[0].buffer_offset == 0 && segments[0].bus_address == 0x80000000 && segments[0].bytes == 16384);
            CHECK(segments[1].buffer_offset == 16384 && segments[1].bus_address == 0xc0000000 && segments[1].bytes == 16384);
        }
        CHECK(extgpu_buffer_unregister(&lease.window) == EXTGPU_E_UNSAFE);
        CHECK(op_gpu_shutdown(&lease) == EXTGPU_OK && op_gpu_ram_releasable(&lease));
        CHECK(!f.calls[EXTGPU_SELECTOR_UNREGISTER_DMA_REGION] && f.calls[EXTGPU_SELECTOR_WARM_RELEASE_VM_FLR] == 1);
    }
    free(ram);
}

static void dynamic_vm_dma_retired_with_initial_window(void)
{
    fixture f = {.generation = 71, .warm_capable = true, .warm_lost_reply = true, .vm_dynamic_capable = true};
    struct op_gpu_lease lease = {.release_kind = OP_GPU_RELEASE_FLR, .stop_acknowledged = true};
    void *ram = NULL;
    CHECK(posix_memalign(&ram, 16384, 49152) == 0);
    const uint64_t key = UINT64_C(1) << 60;
    CHECK(extgpu_test_open(&transport, &f, &lease.session, &lease.device) == EXTGPU_OK);
    CHECK(extgpu_device_claim(lease.device) == EXTGPU_OK);
    extgpu_dma_coverage coverage;
    CHECK(extgpu_vm_dma_map(lease.device, key, ram, 16384, &coverage) == EXTGPU_E_UNSUPPORTED);
    CHECK(extgpu_vm_ram_register(lease.device, 0x40000000, ram, 16384, &lease.window, &coverage) == EXTGPU_OK);
    CHECK(extgpu_vm_dma_enable(lease.device, lease.window) == EXTGPU_OK);
    CHECK(extgpu_vm_dma_map(lease.device, key, (char *)ram + 16384, 16384, &coverage) == EXTGPU_OK);
    CHECK(coverage.complete && coverage.bus_address == 0xc0004000);
    CHECK(extgpu_vm_dma_map(lease.device, key + 16384, (char *)ram + 32768, 16384, &coverage) == EXTGPU_OK);
    CHECK(extgpu_vm_dma_unmap(lease.device, key) == EXTGPU_OK && f.vm_dynamic_regions == 1);
    CHECK(op_gpu_shutdown(&lease) == EXTGPU_E_UNSAFE && !op_gpu_ram_releasable(&lease));
    CHECK(op_gpu_shutdown(&lease) == EXTGPU_OK && op_gpu_ram_releasable(&lease));
    CHECK(!f.vm_dynamic_regions && f.calls[EXTGPU_SELECTOR_UNREGISTER_DMA_REGION] == 1);
    CHECK(f.calls[EXTGPU_SELECTOR_WARM_RELEASE_VM_FLR] == 2);
    free(ram);
}

static void vega_preflight_is_read_only(void)
{
    fixture f = {.generation = 71, .warm_vega_capable = true, .group_preflight_capable = true};
    extgpu_session *session = NULL;
    extgpu_device *device = NULL;
    CHECK(extgpu_test_open(&transport, &f, &session, &device) == EXTGPU_OK);
    CHECK(extgpu_test_set_registry_ids(device, 9001, 9101) == EXTGPU_OK);
    CHECK(extgpu_warm_release_vm_vega_preflight(device) == EXTGPU_E_UNSUPPORTED);
    CHECK(!f.calls[EXTGPU_SELECTOR_VM_GROUP_PREFLIGHT]);
    CHECK(extgpu_test_set_reset_scope(device, 9100, 9102) == EXTGPU_OK);
    f.group_companion_unreachable = true;
    CHECK(extgpu_warm_release_vm_vega_preflight(device) == EXTGPU_E_UNSUPPORTED);
    f.group_companion_unreachable = false;
    CHECK(extgpu_warm_release_vm_vega_preflight(device) == EXTGPU_OK);
    CHECK(f.calls[EXTGPU_SELECTOR_VM_GROUP_PREFLIGHT] == 2);
    CHECK(!f.owner && !f.registered && !f.calls[EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA]);
    CHECK(extgpu_device_claim(device) == EXTGPU_OK);
    f.group_companion_unreachable = true;
    CHECK(extgpu_warm_release_vm_vega_pin(device) == EXTGPU_E_UNSUPPORTED);
    CHECK(f.calls[EXTGPU_SELECTOR_VM_GROUP_PREFLIGHT] == 3);
    CHECK(!f.registered && !f.calls[EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA]);
    CHECK(extgpu_device_release(device) == EXTGPU_OK);
    finish(&session, &device);
}

static void vega_idle_perst_probe_replays_outcome(void)
{
    for (unsigned mode = 0; mode <= 2; ++mode) {
        fixture f = {.generation = 71, .warm_vega_capable = true, .perst_capable = true,
                     .warm_lost_reply = true, .perst_unsupported = mode == 1,
                     .perst_native_error = mode == 2 ? kIOReturnBadArgument : kIOReturnSuccess};
        extgpu_session *session = NULL;
        extgpu_device *device = NULL;
        bool issued = false;
        CHECK(extgpu_test_open(&transport, &f, &session, &device) == EXTGPU_OK);
        CHECK(extgpu_test_set_registry_ids(device, 9001, 9101) == EXTGPU_OK);
        CHECK(extgpu_test_set_reset_scope(device, 9100, 9102) == EXTGPU_OK);
        CHECK(extgpu_device_claim(device) == EXTGPU_OK);
        CHECK(extgpu_probe_vega_perst(device, &issued) == EXTGPU_E_UNSAFE && !issued);
        CHECK(f.warm_closed && f.generation == 72 && !f.owner);
        CHECK(extgpu_device_release(device) == EXTGPU_E_UNSAFE);
        CHECK(extgpu_probe_vega_perst(device, &issued) == EXTGPU_OK);
        CHECK(issued == (mode == 0) && f.generation == 72);
        extgpu_error_info detail = {0};
        CHECK(extgpu_device_last_error(device, &detail) == EXTGPU_OK);
        CHECK(detail.native_code == (mode == 1 ? kIOReturnUnsupported : f.perst_native_error));
        CHECK(f.calls[EXTGPU_SELECTOR_PROBE_VEGA_PERST] == 2);
        CHECK(!f.calls[EXTGPU_SELECTOR_RELEASE_OWNER] && !f.calls[EXTGPU_SELECTOR_REGISTER_DMA_REGION]);
        finish(&session, &device);
    }
    puts("PASS idle Vega PERST probe reports the exact reset outcome on same-scope replay");
}

static void flr_preflight_requires_exact_idle_provider(void)
{
    fixture f = {.generation = 71, .warm_capable = true, .flr_preflight_capable = true};
    extgpu_session *session = NULL;
    extgpu_device *device = NULL;
    CHECK(extgpu_test_open(&transport, &f, &session, &device) == EXTGPU_OK);
    CHECK(extgpu_test_set_registry_ids(device, 9001, 9101) == EXTGPU_OK);
    CHECK(extgpu_warm_release_vm_flr_preflight(device) == EXTGPU_E_NOT_CLAIMED);
    CHECK(!f.calls[EXTGPU_SELECTOR_VM_FLR_PREFLIGHT]);
    CHECK(extgpu_device_claim(device) == EXTGPU_OK);
    CHECK(extgpu_warm_release_vm_flr_preflight(device) == EXTGPU_E_UNSUPPORTED);
    f.flr_advertised = true;
    f.flr_wrong_provider = true;
    CHECK(extgpu_warm_release_vm_flr_preflight(device) == EXTGPU_E_PROTOCOL);
    f.flr_wrong_provider = false;
    CHECK(extgpu_warm_release_vm_flr_preflight(device) == EXTGPU_OK);
    CHECK(!f.calls[EXTGPU_SELECTOR_REGISTER_DMA_REGION] && !f.calls[EXTGPU_SELECTOR_RESET_DEVICE]);
    CHECK(extgpu_device_release(device) == EXTGPU_OK);
    finish(&session, &device);
    puts("PASS FLR eligibility checks the exact provider without registering DMA or resetting");
}

static void bar_restore_requires_an_idle_exact_claim(void)
{
    fixture f = {.generation = 71, .bar_restore_capable = true};
    extgpu_session *session = NULL;
    extgpu_device *device = NULL;
    CHECK(extgpu_test_open(&transport, &f, &session, &device) == EXTGPU_OK);
    CHECK(extgpu_test_set_registry_ids(device, 9001, 9101) == EXTGPU_OK);
    CHECK(extgpu_restore_vega_bars(device) == EXTGPU_E_NOT_CLAIMED);
    CHECK(extgpu_device_claim(device) == EXTGPU_OK);
    f.bar_restore_wrong_provider = true;
    CHECK(extgpu_restore_vega_bars(device) == EXTGPU_E_PROTOCOL);
    f.bar_restore_wrong_provider = false;
    CHECK(extgpu_restore_vega_bars(device) == EXTGPU_OK);
    CHECK(f.calls[EXTGPU_SELECTOR_RESTORE_VEGA_BARS] == 2);
    CHECK(!f.calls[EXTGPU_SELECTOR_CONFIG_WRITE] && !f.calls[EXTGPU_SELECTOR_REGISTER_DMA_REGION]);
    CHECK(extgpu_device_release(device) == EXTGPU_OK);
    finish(&session, &device);
}

static void startup_without_dma_releases_the_original_reset_scope(void)
{
    fixture previous = {.generation = 71, .warm_capable = true};
    extgpu_session *session = NULL;
    extgpu_device *device = NULL;
    CHECK(extgpu_test_open(&transport, &previous, &session, &device) == EXTGPU_OK);
    CHECK(extgpu_device_claim(device) == EXTGPU_OK);
    CHECK(extgpu_warm_release_vm_no_dma(device) == EXTGPU_E_UNSUPPORTED);
    CHECK(!previous.calls[EXTGPU_SELECTOR_WARM_RELEASE_VM_FLR]);
    CHECK(extgpu_device_release(device) == EXTGPU_OK);
    finish(&session, &device);

    fixture flr = {.generation = 71, .warm_capable = true, .warm_no_dma_capable = true};
    CHECK(extgpu_test_open(&transport, &flr, &session, &device) == EXTGPU_OK);
    CHECK(extgpu_device_claim(device) == EXTGPU_OK);
    CHECK(extgpu_warm_release_vm_no_dma(device) == EXTGPU_OK && flr.warm_closed);
    CHECK(flr.calls[EXTGPU_SELECTOR_WARM_RELEASE_VM_FLR] == 1 &&
          !flr.calls[EXTGPU_SELECTOR_REGISTER_DMA_REGION]);
    finish(&session, &device);

    fixture f = {.generation = 71, .warm_vega_capable = true, .warm_no_dma_capable = true,
                 .vega_receipt_capable = true, .bar_restore_capable = true,
                 .bar_restore_native_error = kIOReturnIPCError,
                 .warm_waiting_for_transactions = true, .warm_lost_reply = true,
                 .vega_bad_receipt_once = true};
    struct op_gpu_lease lease = {.release_kind = OP_GPU_RELEASE_VEGA};
    CHECK(extgpu_test_open(&transport, &f, &lease.session, &lease.device) == EXTGPU_OK);
    CHECK(extgpu_test_set_registry_ids(lease.device, 9001, 9101) == EXTGPU_OK);
    CHECK(extgpu_test_set_reset_scope(lease.device, 9100, 9102) == EXTGPU_OK);
    CHECK(extgpu_device_claim(lease.device) == EXTGPU_OK);
    CHECK(op_gpu_preflight_release(&lease, OP_GPU_RELEASE_FLR) == EXTGPU_OK);
    lease.startup_reset_required = true;
    CHECK(extgpu_restore_vega_bars(lease.device) == EXTGPU_E_IO);
    CHECK(op_gpu_shutdown(&lease) == EXTGPU_E_BUSY && !f.calls[EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA]);

    lease.stop_acknowledged = true;
    CHECK(extgpu_test_set_reset_scope(lease.device, 9100, 9199) == EXTGPU_OK);
    CHECK(op_gpu_shutdown(&lease) == EXTGPU_E_UNSAFE && op_gpu_shutdown_waiting(&lease));
    CHECK(!f.calls[EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA] && f.owner);
    CHECK(extgpu_test_set_reset_scope(lease.device, 9100, 9102) == EXTGPU_OK);
    CHECK(op_gpu_shutdown(&lease) == EXTGPU_E_UNSAFE && op_gpu_shutdown_waiting(&lease));
    CHECK(f.owner && !op_gpu_ram_releasable(&lease));
    f.warm_waiting_for_transactions = false;
    CHECK(op_gpu_shutdown(&lease) == EXTGPU_E_UNSAFE && f.warm_closed);
    CHECK(lease.device && !op_gpu_ram_releasable(&lease));
    CHECK(extgpu_config_write(lease.device, 4, 2, 6) == EXTGPU_E_UNSAFE);

    CHECK(extgpu_test_set_reset_scope(lease.device, 0, 0) == EXTGPU_OK);
    CHECK(op_gpu_shutdown(&lease) == EXTGPU_E_UNSAFE && !op_gpu_ram_releasable(&lease));
    CHECK(op_gpu_shutdown(&lease) == EXTGPU_OK && op_gpu_ram_releasable(&lease));
    CHECK(!f.calls[EXTGPU_SELECTOR_REGISTER_DMA_REGION] && !f.calls[EXTGPU_SELECTOR_UNREGISTER_DMA_REGION]);
    CHECK(!f.calls[EXTGPU_SELECTOR_WARM_RELEASE_VM_FLR] && !f.calls[EXTGPU_SELECTOR_RELEASE_OWNER]);
    CHECK(f.calls[EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA] == 2 &&
          f.calls[EXTGPU_SELECTOR_VEGA_RELEASE_RECEIPT] == 2 && f.generation == 72 && f.closes == 1);
    puts("PASS startup failure before DMA uses original reset group and exact closed receipt");
}

static void transaction_drain_wait_retains_the_original_window(void)
{
    fixture f = {.generation = 71, .warm_vega_capable = true, .warm_waiting_for_transactions = true};
    struct op_gpu_lease lease = {.stop_acknowledged = true, .release_kind = OP_GPU_RELEASE_VEGA};
    void *ram = NULL;
    extgpu_dma_coverage coverage;
    CHECK(posix_memalign(&ram, 16384, 16384) == 0);
    CHECK(extgpu_test_open(&transport, &f, &lease.session, &lease.device) == EXTGPU_OK);
    CHECK(extgpu_test_set_registry_ids(lease.device, 9001, 9101) == EXTGPU_OK);
    CHECK(extgpu_test_set_reset_scope(lease.device, 9100, 9102) == EXTGPU_OK);
    CHECK(extgpu_device_claim(lease.device) == EXTGPU_OK);
    CHECK(op_gpu_preflight_release(&lease, OP_GPU_RELEASE_FLR) == EXTGPU_OK);
    CHECK(!f.calls[EXTGPU_SELECTOR_CONFIG_WRITE] && !f.calls[EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA]);
    CHECK(extgpu_test_set_reset_scope(lease.device, 9100, 9199) == EXTGPU_OK);
    CHECK(op_gpu_preflight_release(&lease, OP_GPU_RELEASE_FLR) == EXTGPU_E_UNSAFE);
    CHECK(extgpu_test_set_reset_scope(lease.device, 9100, 9102) == EXTGPU_OK);
    CHECK(extgpu_vm_ram_register(lease.device, 0x40000000, ram, 16384, &lease.window, &coverage) == EXTGPU_OK);
    extgpu_buffer *original = lease.window;

    CHECK(extgpu_test_set_reset_scope(lease.device, 0, 0) == EXTGPU_OK);
    CHECK(op_gpu_shutdown(&lease) == EXTGPU_E_UNSAFE && op_gpu_shutdown_waiting(&lease));
    CHECK(extgpu_test_set_reset_scope(lease.device, 9100, 9199) == EXTGPU_OK);
    CHECK(op_gpu_shutdown(&lease) == EXTGPU_E_UNSAFE && op_gpu_shutdown_waiting(&lease));
    CHECK(lease.window == original && f.owner && f.registered && !op_gpu_ram_releasable(&lease));
    CHECK(!f.calls[EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA] && !f.calls[EXTGPU_SELECTOR_VEGA_RELEASE_RECEIPT]);
    CHECK(extgpu_test_set_reset_scope(lease.device, 9100, 9102) == EXTGPU_OK);

    CHECK(op_gpu_shutdown(&lease) == EXTGPU_E_UNSAFE);
    CHECK(f.owner && f.registered && !f.warm_closed && lease.window && !op_gpu_ram_releasable(&lease));
    CHECK(op_gpu_shutdown_waiting(&lease));
    extgpu_error_info detail;
    CHECK(extgpu_device_last_error(lease.device, &detail) == EXTGPU_OK);
    CHECK(detail.native_code == kIOReturnNotReady);
    CHECK(extgpu_device_release(lease.device) == EXTGPU_E_BUSY);
    CHECK(!f.calls[EXTGPU_SELECTOR_UNREGISTER_DMA_REGION] && !f.calls[EXTGPU_SELECTOR_RESET_DEVICE]);
    f.warm_waiting_for_transactions = false;
    f.warm_waiting_for_close = true;
    CHECK(op_gpu_shutdown(&lease) == EXTGPU_E_UNSAFE && op_gpu_shutdown_waiting(&lease));
    CHECK(f.warm_dma_retired && !f.registered && f.owner && f.generation == 71);
    CHECK(lease.window == original && !op_gpu_ram_releasable(&lease) && !f.closes);
    f.warm_waiting_for_close = false;
    CHECK(op_gpu_shutdown(&lease) == EXTGPU_OK && op_gpu_ram_releasable(&lease));
    CHECK(f.calls[EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA] == 3 && f.warm_closed && f.generation == 72);
    free(ram);
    puts("PASS Vega waits for the PCI scope pinned before DMA, including before the first release request");
    puts("PASS transaction drain defers reset and retires the original window only after exact acknowledgment");
    puts("PASS VM waits for final PCI close after DMA retirement before releasing its backing");
}

static void bar_word_batch_validates_wire_and_preserves_failed_output(void)
{
    fixture f = {.generation = 71, .bar_words_capable = true};
    extgpu_session *session = NULL;
    extgpu_device *device = NULL;
    uint64_t words[2] = {0x1234, 0x5678};
    CHECK(extgpu_test_open(&transport, &f, &session, &device) == EXTGPU_OK);
    CHECK(extgpu_bar_words(device, 9, 8, EXTGPU_BAR_WORDS_FILL, 512, words) == EXTGPU_E_NOT_CLAIMED);
    CHECK(extgpu_device_claim(device) == EXTGPU_OK);
    CHECK(extgpu_bar_words(device, 9, 8, EXTGPU_BAR_WORDS_WRITE, 13, words) == EXTGPU_E_ARGUMENT);
    CHECK(extgpu_bar_words(device, 9, UINT64_MAX - 7, EXTGPU_BAR_WORDS_FILL, 2, words) == EXTGPU_E_ARGUMENT);
    CHECK(f.calls[EXTGPU_SELECTOR_BAR_WORDS] == 0);
    CHECK(extgpu_bar_words(device, 9, 8, EXTGPU_BAR_WORDS_FILL, 512, words) == EXTGPU_OK);
    f.bar_words_short_reply = true;
    CHECK(extgpu_bar_words(device, 9, 8, EXTGPU_BAR_WORDS_READ, 2, words) == EXTGPU_E_PROTOCOL);
    CHECK(words[0] == 0x1234 && words[1] == 0x5678);
    f.bar_words_short_reply = false;
    CHECK(extgpu_bar_words(device, 9, 8, EXTGPU_BAR_WORDS_READ, 2, words) == EXTGPU_OK);
    CHECK(words[0] == 0x1122 && words[1] == 0x3344);
    CHECK(f.calls[EXTGPU_SELECTOR_BAR_WORDS] == 3);
    CHECK(extgpu_device_release(device) == EXTGPU_OK);
    finish(&session, &device);
}

static void shared_word_batches_require_the_complete_capability(void)
{
    fixture f = {.generation = 71, .bar_words_capable = true, .protocol_minor = 21,
                 .window_capabilities = EXTGPU_CAP_MMIO_WINDOW};
    extgpu_session *session = NULL;
    extgpu_device *device = NULL;
    uint64_t words[2] = {0x1234, 0x5678};
    CHECK(extgpu_test_open(&transport, &f, &session, &device) == EXTGPU_OK);
    CHECK(extgpu_device_claim(device) == EXTGPU_OK);
    CHECK(extgpu_bar_words(device, 9, 8, EXTGPU_BAR_WORDS_WRITE, 2, words) == EXTGPU_OK);
    CHECK(f.calls[EXTGPU_SELECTOR_BAR_WORDS] == 1 && f.calls[EXTGPU_SELECTOR_MMIO_WINDOW] == 0);
    CHECK(extgpu_device_release(device) == EXTGPU_OK);
    finish(&session, &device);

    f = (fixture){.generation = 71, .bar_words_capable = true, .protocol_minor = 21,
                  .window_capabilities = EXTGPU_CAP_MMIO_WINDOW | EXTGPU_CAP_MMIO_WINDOW_WORD_WRITES};
    CHECK(extgpu_test_open(&transport, &f, &session, &device) == EXTGPU_E_PROTOCOL);
    f.protocol_minor = 22;
    f.window_capabilities &= ~EXTGPU_CAP_MMIO_WINDOW;
    CHECK(extgpu_test_open(&transport, &f, &session, &device) == EXTGPU_E_PROTOCOL);
    f.window_capabilities |= EXTGPU_CAP_MMIO_WINDOW;
    CHECK(extgpu_test_open(&transport, &f, &session, &device) == EXTGPU_OK);
    finish(&session, &device);
}

struct modeled_mmio_window {
    struct openpci_mmio_window page;
    bool cancel, fill;
    kern_return_t status;
};

static void *service_one_word_batch(void *opaque)
{
    struct modeled_mmio_window *model = opaque;
    struct openpci_mmio_window *page = &model->page;
    const uint64_t request = 8 | OPENPCI_MMIO_REQUEST;
    const uint64_t busy = 8 | OPENPCI_MMIO_BUSY;
    for (unsigned attempt = 0; attempt < 500 && openpci_mmio_state(page) != request; ++attempt)
        usleep(100);
    if (model->cancel) {
        CHECK(openpci_mmio_change(page, request, 8 | OPENPCI_MMIO_ABORTED));
        return NULL;
    }
    CHECK(openpci_mmio_change(page, request, busy));
    CHECK(__atomic_load_n(&page->write, __ATOMIC_RELAXED) == OPENPCI_MMIO_BAR_WORD_WRITE);
    CHECK(__atomic_load_n(&page->index, __ATOMIC_RELAXED) == 9);
    CHECK(__atomic_load_n(&page->offset, __ATOMIC_RELAXED) == 8);
    const unsigned count = model->fill ? 8 : 2;
    CHECK(__atomic_load_n(&page->width, __ATOMIC_RELAXED) == count);
    for (unsigned i = 0; i < count; ++i) {
        const uint64_t expected = model->fill || !i ? 0x1234 : 0x5678;
        CHECK(__atomic_load_n(&page->words[i], __ATOMIC_RELAXED) == expected);
    }
    __atomic_store_n(&page->status, (uint32_t)model->status, __ATOMIC_RELAXED);
    CHECK(openpci_mmio_change(page, busy, 8 | OPENPCI_MMIO_DONE_OFFLINE));
    return NULL;
}

static void shared_word_batches_replay_only_a_canceled_request(void)
{
    const struct {
        bool cancel, fill;
        kern_return_t native_status;
        extgpu_status expected;
    } cases[] = {
        {false, false, kIOReturnSuccess, EXTGPU_OK},
        {false, false, kIOReturnBadArgument, EXTGPU_E_ARGUMENT},
        {true, false, kIOReturnSuccess, EXTGPU_OK},
        {false, true, kIOReturnSuccess, EXTGPU_OK},
        {false, true, kIOReturnBadArgument, EXTGPU_E_ARGUMENT},
        {true, true, kIOReturnSuccess, EXTGPU_OK},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct modeled_mmio_window model = {
            .page = {.state = OPENPCI_MMIO_READY, .generation = 71},
            .cancel = cases[i].cancel, .fill = cases[i].fill, .status = cases[i].native_status,
        };
        fixture f = {.generation = 71, .bar_words_capable = true, .window = &model.page,
                     .bar_fill_count = 8,
                     .window_capabilities = EXTGPU_CAP_MMIO_WINDOW | EXTGPU_CAP_MMIO_WINDOW_WORD_WRITES};
        extgpu_session *session = NULL;
        extgpu_device *device = NULL;
        CHECK(extgpu_test_open(&transport, &f, &session, &device) == EXTGPU_OK);
        CHECK(extgpu_device_claim(device) == EXTGPU_OK);
        CHECK(extgpu_test_bind_mmio_window(device, &model.page) == EXTGPU_OK);
        pthread_t worker;
        CHECK(pthread_create(&worker, NULL, service_one_word_batch, &model) == 0);
        uint64_t words[] = {0x1234, 0x5678};
        const extgpu_bar_words_op op = model.fill ? EXTGPU_BAR_WORDS_FILL : EXTGPU_BAR_WORDS_WRITE;
        CHECK(extgpu_bar_words(device, 9, 8, op, model.fill ? 8 : 2, words) == cases[i].expected);
        CHECK(pthread_join(worker, NULL) == 0);
        CHECK(f.calls[EXTGPU_SELECTOR_BAR_WORDS] == (unsigned)model.cancel);
        CHECK(extgpu_device_release(device) == EXTGPU_OK);
        finish(&session, &device);
        CHECK(f.window_unmaps == 1);
    }
}

static void vega_shutdown_replays_only_original_scope(void)
{
    fixture f = {.generation = 71, .warm_capable = true, .warm_unsupported = true,
                 .warm_vega_capable = true, .warm_lost_reply = true};
    struct op_gpu_lease lease = {.stop_acknowledged = true};
    void *ram = NULL;
    extgpu_dma_coverage coverage;
    CHECK(posix_memalign(&ram, 16384, 16384) == 0);
    CHECK(extgpu_test_open(&transport, &f, &lease.session, &lease.device) == EXTGPU_OK);
    CHECK(extgpu_test_set_registry_ids(lease.device, 9001, 9101) == EXTGPU_OK);
    CHECK(extgpu_device_claim(lease.device) == EXTGPU_OK);
    CHECK(extgpu_dma_register(lease.device, 0x40000000, ram, 16384, &lease.window, &coverage) == EXTGPU_OK);
    CHECK(op_gpu_shutdown(&lease) == EXTGPU_E_UNSUPPORTED);
    CHECK(!f.calls[EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA] && lease.window && lease.release_kind == OP_GPU_RELEASE_FLR);
    lease.release_kind = OP_GPU_RELEASE_VEGA;
    CHECK(extgpu_test_set_reset_scope(lease.device, 9101, 9102) == EXTGPU_E_UNSAFE);
    CHECK(extgpu_test_set_reset_scope(lease.device, 9100, 9102) == EXTGPU_OK);
    CHECK(op_gpu_shutdown(&lease) == EXTGPU_E_UNSAFE && f.warm_closed);
    CHECK(lease.window && !op_gpu_ram_releasable(&lease));
    CHECK(extgpu_test_set_reset_scope(lease.device, 9100, 9199) == EXTGPU_OK);
    CHECK(op_gpu_shutdown(&lease) == EXTGPU_E_UNSAFE);
    CHECK(f.calls[EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA] == 1);
    CHECK(f.calls[EXTGPU_SELECTOR_VEGA_RELEASE_RECEIPT] == 0);
    CHECK(extgpu_warm_release_vm_flr(lease.device, &lease.window) == EXTGPU_E_UNSAFE);
    CHECK(extgpu_test_set_reset_scope(lease.device, 9100, 9102) == EXTGPU_OK);
    CHECK(op_gpu_shutdown(&lease) == EXTGPU_OK && op_gpu_ram_releasable(&lease));
    CHECK(f.calls[EXTGPU_SELECTOR_WARM_RELEASE_VM_FLR] == 1);
    CHECK(f.calls[EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA] == 2 && f.closes == 1);
    free(ram);
    puts("PASS Vega release requires exact bridge scope and replays only its original retirement request");
}

static void vega_shutdown_reads_only_closed_receipt_after_scope_loss(void)
{
    fixture f = {.generation = 71, .warm_vega_capable = true, .vega_receipt_capable = true,
                 .warm_waiting_for_transactions = true, .warm_lost_reply = true, .vega_bad_receipt_once = true};
    struct op_gpu_lease lease = {.stop_acknowledged = true, .release_kind = OP_GPU_RELEASE_VEGA};
    void *ram = NULL;
    extgpu_dma_coverage coverage;
    CHECK(posix_memalign(&ram, 16384, 16384) == 0);
    CHECK(extgpu_test_open(&transport, &f, &lease.session, &lease.device) == EXTGPU_OK);
    CHECK(extgpu_test_set_registry_ids(lease.device, 9001, 9101) == EXTGPU_OK);
    CHECK(extgpu_device_claim(lease.device) == EXTGPU_OK);
    CHECK(extgpu_dma_register(lease.device, 0x40000000, ram, 16384, &lease.window, &coverage) == EXTGPU_OK);
    extgpu_buffer *original = lease.window;
    CHECK(op_gpu_shutdown(&lease) == EXTGPU_E_UNSUPPORTED);
    CHECK(f.calls[EXTGPU_SELECTOR_VEGA_RELEASE_RECEIPT] == 0);
    CHECK(f.calls[EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA] == 0);

    CHECK(extgpu_test_set_reset_scope(lease.device, 9100, 9102) == EXTGPU_OK);
    CHECK(op_gpu_shutdown(&lease) == EXTGPU_E_UNSAFE);
    CHECK(extgpu_test_set_reset_scope(lease.device, 0, 0) == EXTGPU_OK);
    CHECK(op_gpu_shutdown(&lease) == EXTGPU_E_UNSAFE);
    CHECK(op_gpu_shutdown_waiting(&lease));
    CHECK(f.calls[EXTGPU_SELECTOR_VEGA_RELEASE_RECEIPT] == 1);
    CHECK(f.calls[EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA] == 1 && f.generation == 71);
    CHECK(f.owner && f.registered && lease.window == original && !op_gpu_ram_releasable(&lease));
    extgpu_error_info detail;
    CHECK(extgpu_device_last_error(lease.device, &detail) == EXTGPU_OK);
    CHECK(detail.native_code == kIOReturnNotReady);

    f.warm_waiting_for_transactions = false;
    CHECK(extgpu_test_set_reset_scope(lease.device, 9100, 9102) == EXTGPU_OK);
    CHECK(op_gpu_shutdown(&lease) == EXTGPU_E_UNSAFE && f.warm_closed && f.generation == 72);
    CHECK(extgpu_test_set_reset_scope(lease.device, 9100, 9199) == EXTGPU_OK);
    CHECK(op_gpu_shutdown(&lease) == EXTGPU_E_UNSAFE);
    CHECK(!op_gpu_shutdown_waiting(&lease));
    CHECK(lease.window == original && !op_gpu_ram_releasable(&lease));
    CHECK(extgpu_test_set_reset_scope(lease.device, 0, 0) == EXTGPU_OK);
    CHECK(op_gpu_shutdown(&lease) == EXTGPU_OK && op_gpu_ram_releasable(&lease));
    CHECK(f.calls[EXTGPU_SELECTOR_VEGA_RELEASE_RECEIPT] == 3);
    CHECK(f.calls[EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA] == 2 && f.generation == 72 && f.closes == 1);
    CHECK(!f.calls[EXTGPU_SELECTOR_UNREGISTER_DMA_REGION] && !f.calls[EXTGPU_SELECTOR_RESET_DEVICE]);
    free(ram);
    puts("PASS Vega detached topology accepts only the original closed release receipt");
}

static void grouped_release_prefers_flr_then_binds_the_original_pair(enum op_gpu_release_kind fallback)
{
    const bool polaris = fallback == OP_GPU_RELEASE_POLARIS;
    const uint32_t method = polaris ? EXTGPU_SELECTOR_WARM_RELEASE_VM_POLARIS : EXTGPU_SELECTOR_WARM_RELEASE_VM_RTX3090;
    const uint32_t receipt = polaris ? EXTGPU_SELECTOR_POLARIS_RELEASE_RECEIPT : EXTGPU_SELECTOR_RTX3090_RELEASE_RECEIPT;
    extgpu_status (*preflight)(extgpu_device *) = polaris ?
        extgpu_warm_release_vm_polaris_preflight : extgpu_warm_release_vm_rtx3090_preflight;
    extgpu_status (*other_preflight)(extgpu_device *) = polaris ?
        extgpu_warm_release_vm_rtx3090_preflight : extgpu_warm_release_vm_polaris_preflight;
    extgpu_status (*other_release)(extgpu_device *, extgpu_buffer **) = polaris ?
        extgpu_warm_release_vm_rtx3090 : extgpu_warm_release_vm_polaris;
    fixture f = {.generation = 71, .warm_capable = true, .flr_preflight_capable = true,
                 .warm_rtx3090_capable = true, .warm_polaris_capable = true,
                 .warm_lost_reply = true, .vega_bad_receipt_once = true};
    struct op_gpu_lease lease = {.stop_acknowledged = true};
    void *ram = NULL;
    extgpu_dma_coverage coverage;
    CHECK(posix_memalign(&ram, 16384, 16384) == 0);
    CHECK(extgpu_test_open(&transport, &f, &lease.session, &lease.device) == EXTGPU_OK);
    CHECK(extgpu_test_set_registry_ids(lease.device, 9001, 9101) == EXTGPU_OK);
    CHECK(preflight(lease.device) == EXTGPU_E_NOT_CLAIMED);
    CHECK(extgpu_device_claim(lease.device) == EXTGPU_OK);
    CHECK(extgpu_test_set_reset_scope(lease.device, 9100, 9102) == EXTGPU_OK);
    f.flr_advertised = true;
    CHECK(op_gpu_preflight_release(&lease, fallback) == EXTGPU_OK && lease.release_kind == OP_GPU_RELEASE_FLR);
    f.flr_wrong_provider = true;
    CHECK(op_gpu_preflight_release(&lease, fallback) == EXTGPU_E_PROTOCOL && lease.release_kind == OP_GPU_RELEASE_FLR);
    f.flr_wrong_provider = false;
    f.flr_advertised = false;
    CHECK(op_gpu_preflight_release(&lease, OP_GPU_RELEASE_FLR) == EXTGPU_E_UNSUPPORTED);
    CHECK(extgpu_test_set_reset_scope(lease.device, 0, 0) == EXTGPU_OK);
    CHECK(op_gpu_preflight_release(&lease, fallback) == EXTGPU_E_UNSUPPORTED);
    CHECK(extgpu_test_set_reset_scope(lease.device, 9100, 9102) == EXTGPU_OK);
    CHECK(op_gpu_preflight_release(&lease, fallback) == EXTGPU_OK && lease.release_kind == fallback);
    CHECK(other_preflight(lease.device) == EXTGPU_E_UNSAFE);
    CHECK(extgpu_test_set_reset_scope(lease.device, 9100, 9199) == EXTGPU_OK);
    CHECK(preflight(lease.device) == EXTGPU_E_UNSAFE);
    CHECK(!f.calls[method] && !f.calls[EXTGPU_SELECTOR_CONFIG_WRITE]);

    CHECK(extgpu_vm_ram_register(lease.device, 0x40000000, ram, 16384, &lease.window, &coverage) == EXTGPU_OK);
    CHECK(preflight(lease.device) == EXTGPU_E_BUSY);
    extgpu_buffer *original = lease.window;
    CHECK(op_gpu_shutdown(&lease) == EXTGPU_E_UNSAFE && op_gpu_shutdown_waiting(&lease));
    CHECK(lease.window == original && !f.calls[method]);
    CHECK(!f.calls[receipt]);

    CHECK(extgpu_test_set_reset_scope(lease.device, 9100, 9102) == EXTGPU_OK);
    CHECK(op_gpu_shutdown(&lease) == EXTGPU_E_UNSAFE && f.warm_closed);
    CHECK(extgpu_warm_release_vm_flr(lease.device, &lease.window) == EXTGPU_E_UNSAFE);
    CHECK(extgpu_warm_release_vm_vega(lease.device, &lease.window) == EXTGPU_E_UNSAFE);
    CHECK(other_release(lease.device, &lease.window) == EXTGPU_E_UNSAFE);
    CHECK(extgpu_test_set_reset_scope(lease.device, 0, 0) == EXTGPU_OK);
    CHECK(op_gpu_shutdown(&lease) == EXTGPU_E_UNSAFE && lease.window == original);
    CHECK(op_gpu_shutdown(&lease) == EXTGPU_OK && op_gpu_ram_releasable(&lease));
    CHECK(f.calls[method] == 1 && f.generation == 72);
    CHECK(f.calls[receipt] == 2 && f.closes == 1);
    CHECK(!f.calls[EXTGPU_SELECTOR_WARM_RELEASE_VM_FLR] && !f.calls[EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA]);
    free(ram);
    printf("PASS %s prefers FLR and only releases the preflight-pinned PCI pair\n", polaris ? "Polaris" : "RTX 3090");
}

int main(void)
{
    failed_and_uncertain_claims(); epoch_recovery(); coverage_and_reservations(); malformed_irq_success();
    consumed_ordinary_close_reference_never_reaches_recycled_name();
    broker_preflight_failure_then_ordinary_close_error_consumes_once();
    warm_window_retirement();
    vm_shutdown_requires_hardware_ack_before_unmap();
    vm_startup_without_dma_ack_requires_scoped_warm_release();
    segmented_vm_dma_requires_exact_coverage_and_scoped_release();
    dynamic_vm_dma_retired_with_initial_window();
    vm_shutdown_clears_bus_master_before_retiring_interrupts();
    vega_preflight_is_read_only();
    vega_idle_perst_probe_replays_outcome();
    flr_preflight_requires_exact_idle_provider();
    bar_restore_requires_an_idle_exact_claim();
    startup_without_dma_releases_the_original_reset_scope();
    transaction_drain_wait_retains_the_original_window();
    bar_word_batch_validates_wire_and_preserves_failed_output();
    shared_word_batches_require_the_complete_capability();
    shared_word_batches_replay_only_a_canceled_request();
    vega_shutdown_replays_only_original_scope();
    vega_shutdown_reads_only_closed_receipt_after_scope_loss();
    grouped_release_prefers_flr_then_binds_the_original_pair(OP_GPU_RELEASE_RTX3090);
    grouped_release_prefers_flr_then_binds_the_original_pair(OP_GPU_RELEASE_POLARIS);
    puts("PASS SDK claim/epoch recovery, exact coverage, reserved IOVA, malformed IRQ and consumed-close-name regressions; no hardware");
    return 0;
}
