/* SPDX-License-Identifier: MIT */
#ifndef EXTGPU_H
#define EXTGPU_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EXTGPU_SDK_VERSION_MAJOR 1u
#define EXTGPU_SDK_VERSION_MINOR 4u
#define EXTGPU_SDK_ABI_VERSION 1u
#define EXTGPU_IRQ_WORDS 4u
#if defined(__GNUC__)
#define EXTGPU_API __attribute__((visibility("default")))
#else
#define EXTGPU_API
#endif

typedef struct extgpu_session extgpu_session;
typedef struct extgpu_device extgpu_device;
typedef struct extgpu_buffer extgpu_buffer;
typedef struct extgpu_mapping extgpu_mapping;
typedef struct extgpu_irq extgpu_irq;

typedef int32_t extgpu_status;
enum {
    EXTGPU_OK = 0,
    EXTGPU_E_ARGUMENT = -1,
    EXTGPU_E_MEMORY = -2,
    EXTGPU_E_IO = -3,
    EXTGPU_E_NOT_FOUND = -4,
    EXTGPU_E_PROTOCOL = -5,
    EXTGPU_E_UNSUPPORTED = -6,
    EXTGPU_E_BUSY = -7,
    EXTGPU_E_NOT_CLAIMED = -8,
    EXTGPU_E_STALE = -9,
    EXTGPU_E_BOUNDS = -10,
    EXTGPU_E_COVERAGE = -11,
    EXTGPU_E_UNSAFE = -12,
    EXTGPU_E_CAPACITY = -13
};

typedef uint64_t extgpu_capabilities;
#define EXTGPU_CAP_CONFIG_READ (UINT64_C(1)<<0)
/* CONFIG_WRITE exposes the driver's restricted command-register policy only;
 * it does not authorize arbitrary PCI configuration or MSI/BAR relocation. */
#define EXTGPU_CAP_CONFIG_WRITE (UINT64_C(1)<<1)
#define EXTGPU_CAP_BAR_MAPPING (UINT64_C(1)<<2)
#define EXTGPU_CAP_DMA_ALLOCATE (UINT64_C(1)<<3)
#define EXTGPU_CAP_DMA_REGISTER (UINT64_C(1)<<4)
#define EXTGPU_CAP_IRQ_ASYNC (UINT64_C(1)<<5)
#define EXTGPU_CAP_RESET (UINT64_C(1)<<6)
#define EXTGPU_CAP_EXCLUSIVE_CLAIM (UINT64_C(1)<<7)
#define EXTGPU_CAP_GENERATIONS (UINT64_C(1)<<8)
#define EXTGPU_CAP_SAFE_RELEASE (UINT64_C(1)<<9)
#define EXTGPU_CAP_CONFIGURABLE_DMA_MASK (UINT64_C(1)<<10)
#define EXTGPU_CAP_IRQ_RETIRE (UINT64_C(1)<<11)
#define EXTGPU_CAP_COLD_REMOVAL (UINT64_C(1)<<12)
#define EXTGPU_CAP_RECOVERY_BROKER (UINT64_C(1)<<13)
#define EXTGPU_CAP_WARM_VM_FLR_RELEASE (UINT64_C(1)<<14)
#define EXTGPU_CAP_WARM_VM_VEGA_RELEASE (UINT64_C(1)<<15)
#define EXTGPU_CAP_VEGA_PERST_PROBE (UINT64_C(1)<<16)
#define EXTGPU_CAP_VM_FLR_PREFLIGHT (UINT64_C(1)<<17)
#define EXTGPU_CAP_RESTORE_VEGA_BARS (UINT64_C(1)<<18)
#define EXTGPU_CAP_BAR_WORDS (UINT64_C(1)<<19)
#define EXTGPU_CAP_VEGA_RELEASE_RECEIPT (UINT64_C(1)<<20)
#define EXTGPU_CAP_WARM_VM_STARTUP_RELEASE (UINT64_C(1)<<21)
#define EXTGPU_CAP_WARM_VM_RTX3090_RELEASE (UINT64_C(1)<<22)
#define EXTGPU_CAP_WARM_VM_POLARIS_RELEASE (UINT64_C(1)<<23)
#define EXTGPU_CAP_PREFETCHABLE_BAR_WORDS (UINT64_C(1)<<24)
#define EXTGPU_CAP_WARM_VM_NO_DMA_RELEASE (UINT64_C(1)<<25)
#define EXTGPU_CAP_CONFIGURABLE_DMA_BUDGET (UINT64_C(1)<<30)
#define EXTGPU_CAP_VM_DMA_SEGMENTS (UINT64_C(1)<<31)
#define EXTGPU_CAP_VM_DYNAMIC_DMA (UINT64_C(1)<<32)
#define EXTGPU_CAP_DMA_ADDRESS_BITS EXTGPU_CAP_CONFIGURABLE_DMA_MASK

/* All public structure layouts and function signatures in ABI 1 are frozen.
 * Future extensions use new types/functions, never append ABI-1 fields. The
 * runtime ABI function must equal EXTGPU_SDK_ABI_VERSION before dylib use. */

typedef struct extgpu_device_info {
    uint64_t registry_id; /* Exact driver IOService identity; never a BDF alone. */
    uint64_t pci_registry_id;
    uint32_t vendor_id;
    uint32_t device_id;
    char registry_name[128];
    char host_root[128];
} extgpu_device_info;

typedef struct extgpu_pci_identity {
    uint32_t bus, device, function;
    uint32_t vendor_id, device_id, class_code;
} extgpu_pci_identity;

typedef struct extgpu_protocol_info {
    uint32_t major, minor;
    uint64_t generation;
    extgpu_capabilities capabilities;
    uint64_t maximum_dma_regions, maximum_dma_bytes, maximum_dma_segments;
    uint64_t abi_fingerprint;
} extgpu_protocol_info;

typedef struct extgpu_bar_info {
    uint32_t bar;
    uint32_t memory_index;
    uint32_t type;
    uint32_t reserved;
    uint64_t size;
} extgpu_bar_info;

typedef struct extgpu_dma_coverage {
    uint64_t generation;
    uint64_t requested_iova; /* Correlation key, not proof of IOMMU address. */
    uint64_t requested_bytes;
    uint64_t bus_address;    /* Device-visible address returned by PrepareForDMA. */
    uint64_t covered_bytes;
    uint32_t segment_count;
    uint32_t complete;
} extgpu_dma_coverage;

typedef struct extgpu_error_info {
    extgpu_status status;
    int32_t native_code;
    char operation[64];
} extgpu_error_info;

/* SDK 1.3 optional terminal recovery; existing ABI-1 structures stay frozen. */
typedef struct extgpu_cold_removal_info {
    uint64_t generation;
    uint64_t driver_registry_id;
    uint64_t pci_registry_id;
    uint64_t nonce[2];
    uint64_t enrolled_regions;
} extgpu_cold_removal_info;
enum {
    /* Caller attests physical removal of power, not VM pause, BME clear,
     * DEXT disappearance, IPC loss, or disconnecting only the data cable. */
    EXTGPU_COLD_PHYSICAL_POWER_OFF = 1u
};

/* SDK 1.2: requests for the macOS process mapping, not a report of effective
 * CPU/Hypervisor cache attributes. No driver protocol/layout change. */
typedef uint32_t extgpu_bar_cache_policy;
enum {
    EXTGPU_BAR_CACHE_AUTOMATIC = 0, /* Legacy: PF => WC, otherwise descriptor default. */
    EXTGPU_BAR_CACHE_DEFAULT = 1,   /* No cache override, including for PF BARs. */
    EXTGPU_BAR_CACHE_INHIBIT = 2,   /* Request kIOMapInhibitCache. */
    EXTGPU_BAR_CACHE_WRITE_COMBINE = 3 /* Request WC; requires a prefetchable BAR. */
};

EXTGPU_API const char *extgpu_status_string(extgpu_status status);
EXTGPU_API uint32_t extgpu_sdk_abi_version(void);
EXTGPU_API extgpu_status extgpu_session_create(extgpu_session **out_session);
/* Refuses while device handles remain. No implicit hardware cleanup. */
EXTGPU_API extgpu_status extgpu_session_destroy(extgpu_session **session);
/* Enumeration only: no IOServiceOpen, protocol RPC, claim or hardware access.
 * *count always receives the required count. NULL/capacity=0 is count-only. */
EXTGPU_API extgpu_status extgpu_enumerate(extgpu_session *session, extgpu_device_info *items,
                               size_t capacity, size_t *count);
/* Opens exactly registry_id and verifies QueryProtocol BEFORE any claim.
 * No automatic claim. Legacy/unversioned drivers are rejected. */
EXTGPU_API extgpu_status extgpu_device_open(extgpu_session *session, uint64_t registry_id,
                                 extgpu_device **out_device);
/* Returns STALE without changing *out_info when the last epoch could not be
 * validated (for example a failed post-reset query or a completed release). */
EXTGPU_API extgpu_status extgpu_device_protocol(extgpu_device *device, extgpu_protocol_info *out_info);
EXTGPU_API extgpu_status extgpu_device_identity(extgpu_device *device, extgpu_pci_identity *out_identity);
EXTGPU_API extgpu_status extgpu_device_last_error(extgpu_device *device, extgpu_error_info *out_error);
EXTGPU_API extgpu_status extgpu_device_claim(extgpu_device *device);
/* Explicit width 32..64; owner-only before resources. Initial/reset width 32. */
EXTGPU_API extgpu_status extgpu_device_set_dma_address_bits(extgpu_device *device, uint32_t bits);
/* Set the total byte budget for both registered and driver-allocated DMA
 * buffers. Requires the exclusive claim, before its first DMA request.
 * The default is restored at release. Read the effective budget in
 * extgpu_device_protocol().maximum_dma_bytes. Mapping can still fail due
 * to memory availability or the device's address/segment constraints.
 * Retry an uncertain request before attempting any DMA allocation. */
EXTGPU_API extgpu_status extgpu_device_set_dma_budget(extgpu_device *device, uint64_t bytes);
/* Requires no live mapping/buffer/IRQ handles and acknowledged safe-release
 * capability. Refreshes the handshake so a post-reset query failure can be
 * retried here. An uncertain claim admits no owner operations until resolved.
 * If a fresh ReleaseOwner reply proves that uncertain client was never owner,
 * returns NOT_CLAIMED and permits close; this is not a release-success claim.
 * Does not use legacy service-terminate selector 2. */
EXTGPU_API extgpu_status extgpu_device_release(extgpu_device *device);
/* Refuses claimed devices or child resources, preserving all handles. */
EXTGPU_API extgpu_status extgpu_device_close(extgpu_device **device);
/* Refuses with live child resources; revalidates protocol generation on success. */
EXTGPU_API extgpu_status extgpu_device_reset(extgpu_device *device);

/* SDK >=1.4 requires protocol >=1.3/COLD_REMOVAL/RECOVERY_BROKER, certain
 * ownership, registered RAM only, no BAR export history, mappings, allocations
 * or IRQ handles. Opens/negotiates the exact separately registered recovery
 * broker before escrow. Before PREPARE this handle irreversibly stops ordinary
 * I/O/requery/claim/release/reset. Escrow uses a separate private capability;
 * it is never exposed in this public info type. All retries keep the same
 * broker connection/key/capability; no provider/epoch rediscovery or fallback.
 * Lost/malformed replies return UNSAFE and retain every buffer and the owner;
 * retry begin on this SAME handle/key. Output changes only on an exact ACK.
 * Broker snapshot ACK proves original freeze/BME-clear; retirement/finish then
 * use only the broker, retaining the original context. IOUserServer death is
 * not recoverable through this API. No automatic power or teardown action. */
EXTGPU_API extgpu_status extgpu_cold_removal_begin(extgpu_device *device,
                                     extgpu_cold_removal_info *out_info);
/* Exactly PHYSICAL_POWER_OFF is required. Separately verifies that the exact
 * old PCI registry ID captured before open has terminated. Missing DEXT/BDF
 * changes or an unsuccessful registry query never prove PCI termination.
 * After confirmation, unregister uses only the terminal cold-retire protocol.
 */
EXTGPU_API extgpu_status extgpu_cold_removal_confirm(extgpu_device *device, uint32_t flags);
/* Requires confirmation and zero child resources. An exact broker CLOSED ACK
 * permits device_close to drop the old local IPC reference and close the broker;
 * a close error is never interpreted as a hardware retirement ACK.
 * Failed/lost FINISH ACK preserves ownership for finish retry.
 * Any failed unregister retains its buffer and host backing. */
EXTGPU_API extgpu_status extgpu_cold_removal_finish(extgpu_device *device);

EXTGPU_API extgpu_status extgpu_config_read(extgpu_device *device, uint32_t offset,
                                 uint32_t width, uint32_t *out_value);
/* Current driver permits only accesses within PCI command bytes 0x04..0x05,
 * changing MSE/BME bits 0x0006 while preserving every other host-owned bit.
 * BAR placement, MSI routing and PCIe parameters are host-owned; other writes
 * return a real driver error. A capability bit is not unrestricted access. */
EXTGPU_API extgpu_status extgpu_config_write(extgpu_device *device, uint32_t offset,
                                  uint32_t width, uint32_t value);
EXTGPU_API extgpu_status extgpu_bar_get_info(extgpu_device *device, uint32_t bar,
                                  extgpu_bar_info *out_info);
/* On EXTGPU_E_UNSAFE, *out_mapping is retained even though initial mapping
 * validation failed. Keep device/claim alive and explicitly unmap that handle;
 * only a successful unmap clears it. Consult device_last_error for the cause. */
/* Experimental direct BAR owner protocol. Authenticate before claiming.
 * The signer is the VMM process that owns the Hypervisor VM and the single
 * exported host view. A retirement signature is submitted only after every
 * vCPU has stopped and all HVF and host BAR mappings have been removed. */
EXTGPU_API extgpu_status extgpu_direct_bar_challenge(extgpu_device *device,
                                                      uint64_t out_challenge[3]);
EXTGPU_API extgpu_status extgpu_direct_bar_authorize(extgpu_device *device,
                                                      const uint64_t signature[8]);
EXTGPU_API extgpu_status extgpu_direct_bar_retire(extgpu_device *device,
                                                   const uint64_t signature[8]);

EXTGPU_API extgpu_status extgpu_bar_map(extgpu_device *device, uint32_t bar,
                             extgpu_mapping **out_mapping);
/* SDK >=1.2. Legacy bar_map delegates to AUTOMATIC. No automatic fallback.
 * One mapping per BAR; requested native cache bits become sticky on the first
 * export attempt, even after a failed export, unmap or device reset. Requests
 * with different bits return BUSY for this handle's lifetime. This prevents
 * SDK policy switching; it cannot prove absence of aliases outside this SDK.
 * Retained-handle error semantics are identical to extgpu_bar_map above. */
EXTGPU_API extgpu_status extgpu_bar_map_with_cache_policy(extgpu_device *device,
                             uint32_t bar, extgpu_bar_cache_policy policy,
                             extgpu_mapping **out_mapping);
/* Direct process mapping, no QEMU/broker/socket copy. Caller serializes MMIO
 * against unmap/reset; access widths/order remain the hardware driver's duty. */
EXTGPU_API void *extgpu_mapping_address(const extgpu_mapping *mapping);
EXTGPU_API uint64_t extgpu_mapping_size(const extgpu_mapping *mapping);
EXTGPU_API uint64_t extgpu_mapping_generation(const extgpu_mapping *mapping);
EXTGPU_API extgpu_status extgpu_mapping_unmap(extgpu_mapping **mapping);
EXTGPU_API extgpu_status extgpu_mmio_read(extgpu_device *device, uint32_t memory_index,
                               uint64_t offset, uint32_t width, uint64_t *out_value);
EXTGPU_API extgpu_status extgpu_mmio_write(extgpu_device *device, uint32_t memory_index,
                                uint64_t offset, uint32_t width, uint64_t value);
#define EXTGPU_MMIO_GROUPED_STORES_MAX 4u
/* Contiguous, naturally aligned scalar writes to a prefetchable BAR in one
 * shared transport request. The complete span is checked before any write.
 * UNSUPPORTED means no write was issued. Successful groups preserve the
 * original access widths and order, with no atomicity guarantee at the device. */
EXTGPU_API extgpu_status extgpu_mmio_write_grouped(extgpu_device *device, uint32_t memory_index,
    uint64_t offset, uint32_t width, uint32_t count, const uint64_t *values);

typedef uint32_t extgpu_bar_words_op;
enum {
    EXTGPU_BAR_WORDS_READ = 0,
    EXTGPU_BAR_WORDS_WRITE = 1,
    EXTGPU_BAR_WORDS_FILL = 2
};
#define EXTGPU_BAR_WORDS_MAX 12u
#define EXTGPU_BAR_WORDS_MAX_FILL 512u
/* Ordered 64-bit accesses to an owned prefetchable memory BAR.
 * A driver with BAR_WORDS alone accepts BAR0. PREFETCHABLE_BAR_WORDS
 * allows other prefetchable memory BARs. The entire range is validated
 * before access. READ and WRITE take count words. FILL takes one word
 * repeated count times. This is a batch, not an atomic transaction. */
EXTGPU_API extgpu_status extgpu_bar_words(extgpu_device *device, uint32_t memory_index,
                           uint64_t offset, extgpu_bar_words_op op, uint32_t count,
                           uint64_t *words);

/* Host memory must be page aligned and remain mapped/unmodified as required by
 * its GPU users until unregister returns OK. This ABI admits only a single DMA
 * segment with complete coverage. requested_iova is NOT necessarily bus_address.
 * IMPORTANT: on EXTGPU_E_UNSAFE, *out_buffer is retained and must not be freed,
 * unmapped, or ignored. Keep host memory and device alive; retry unregister. */
EXTGPU_API extgpu_status extgpu_dma_register(extgpu_device *device, uint64_t requested_iova,
                                  void *host_address, uint64_t bytes,
                                  extgpu_buffer **out_buffer,
                                  extgpu_dma_coverage *out_coverage);
/* Register the sole VM RAM window on an unused claim before guest access.
 * Once the request is sent, an uncertain reply retains the original window
 * for warm release without ordinary rollback. Only an OK result admits DMA.
 * Use extgpu_warm_release_vm_flr or extgpu_warm_release_vm_vega to retire it. */
EXTGPU_API extgpu_status extgpu_vm_ram_register(extgpu_device *device, uint64_t requested_iova,
                                  void *host_address, uint64_t bytes,
                                  extgpu_buffer **out_buffer,
                                  extgpu_dma_coverage *out_coverage);
/* Opt in to multiple bus segments covering a single guest RAM registration.
 * The entire registration has one handle and one warm-release receipt.
 * out_coverage describes total coverage and the first bus address. */
EXTGPU_API extgpu_status extgpu_vm_ram_register_segmented(extgpu_device *device,
                                  uint64_t requested_iova, void *host_address, uint64_t bytes,
                                  extgpu_buffer **out_buffer, extgpu_dma_coverage *out_coverage);
typedef struct extgpu_vm_dma_segment {
    uint64_t buffer_offset, bus_address, bytes;
} extgpu_vm_dma_segment;
#define EXTGPU_VM_DMA_MAX_SEGMENTS 32u
/* Return segments in guest backing order. NULL and capacity zero query the
 * required count. Insufficient capacity leaves the output array untouched. */
EXTGPU_API extgpu_status extgpu_vm_ram_segments(const extgpu_buffer *buffer,
                                  extgpu_vm_dma_segment *out_segments, size_t capacity,
                                  size_t *out_count);
/* Bind on-demand DMA to a VM's initial RAM window. Window retirement also
 * retires all outstanding on-demand mappings after the physical reset. */
EXTGPU_API extgpu_status extgpu_vm_dma_enable(extgpu_device *device, const extgpu_buffer *window);
/* The caller owns the backing and must keep it resident until unmap or
 * warm release is acknowledged. EXTGPU_E_UNSAFE with nonzero requested_bytes
 * also retains the backing. A successful mapping returns the actual IOVA. */
EXTGPU_API extgpu_status extgpu_vm_dma_map(extgpu_device *device, uint64_t key,
                                  void *host_address, uint64_t bytes, extgpu_dma_coverage *out_coverage);
EXTGPU_API extgpu_status extgpu_vm_dma_unmap(extgpu_device *device, uint64_t key);
/* Driver-owned allocation. Only one allocation per connection in ABI v1. */
EXTGPU_API extgpu_status extgpu_dma_allocate(extgpu_device *device, uint64_t bytes,
                                  uint64_t alignment, extgpu_buffer **out_buffer,
                                  extgpu_dma_coverage *out_coverage);
EXTGPU_API void *extgpu_buffer_address(const extgpu_buffer *buffer);
EXTGPU_API uint64_t extgpu_buffer_size(const extgpu_buffer *buffer);
EXTGPU_API extgpu_status extgpu_buffer_coverage(const extgpu_buffer *buffer,
                                     extgpu_dma_coverage *out_coverage);
/* Caller must first stop all GPU references; SDK does not infer GPU completion.
 * Failed driver retirement/unmap preserves handle, registration and backing. */
EXTGPU_API extgpu_status extgpu_buffer_unregister(extgpu_buffer **buffer);
/* Stop vCPUs and close the SDK IRQ before releasing. Requires a single
 * registered RAM window, with no physical BAR export. The driver performs
 * FLR before native DMA completion. Success releases the device claim and
 * clears *window. An uncertain reply preserves the original owner and RAM.
 * Retry on the same handle/window, with no intervening I/O. Unsupported
 * hardware is refused before the driver arms retirement. The startup release
 * capability also accepts the original uncertain first registration before
 * guest access. The driver verifies the original key and retained DMA state. */
EXTGPU_API extgpu_status extgpu_warm_release_vm_flr(extgpu_device *device, extgpu_buffer **window);
/* Recover the original claim after a startup failure before any DMA request.
 * Uses the preflight-pinned reset group when present and FLR otherwise.
 * Stop vCPUs before this operation. Retry an uncertain reply on the same
 * device. The DEXT verifies an empty resource ledger before it can reset. */
EXTGPU_API extgpu_status extgpu_warm_release_vm_no_dma(extgpu_device *device);
/* Observe the endpoint's advertised FLR bit on a pristine claim before
 * preparing DMA, opening IRQs or changing PCI state. Success describes
 * eligibility. Actual release still requires a verified reset. */
EXTGPU_API extgpu_status extgpu_warm_release_vm_flr_preflight(extgpu_device *device);
/* Pristine claim, no DMA or IRQ. Restore the calibrated Vega board's cleared
 * memory BARs only to addresses assigned by macOS. PCI decoding stays off. */
EXTGPU_API extgpu_status extgpu_restore_vega_bars(extgpu_device *device);
/* Read-only admission against the current Vega bridge and companion driver.
 * Does not claim, reserve the reset domain, touch PCI registers, or prove
 * that the DriverKit reset will succeed. Call before registering VM RAM. */
EXTGPU_API extgpu_status extgpu_warm_release_vm_vega_preflight(extgpu_device *device);
/* On the exclusive claim, remember the exact Vega display and audio functions
 * and bridge before acquiring DMA or IRQ resources. Subsequent warm release
 * waits for that scope if it changes. This does not reserve the audio function. */
EXTGPU_API extgpu_status extgpu_warm_release_vm_vega_pin(extgpu_device *device);
/* Probe platform PERST# on the scoped Vega pair. Requires a pristine claim
 * with no guest DMA or IRQ. An exact reply releases the claim and says if
 * PERST succeeded. DeviceLastError records a declined or failed native reset
 * if the pair was proven idle and the claim could be closed. Unknown pair
 * state retains the same connection for replay. */
EXTGPU_API extgpu_status extgpu_probe_vega_perst(extgpu_device *device, bool *reset_issued);
/* As above, for the audited Vega 1002:687f / 1002:aaf8 sibling pair.
 * The SDK checks the live bridge scope before asking the DEXT to take
 * exclusive ownership of the audio companion and perform a hot reset.
 * If that scope changes after a request, protocol 1.10 can replay only
 * the exact completed receipt from the original user client. */
EXTGPU_API extgpu_status extgpu_warm_release_vm_vega(extgpu_device *device, extgpu_buffer **window);
/* On a pristine exclusive claim, require the audited RTX 3090 10de:2204 /
 * 10de:1aef pair as the entire directly downstream PCI reset domain. This
 * pins the original registry scope for the current claim without resetting. */
EXTGPU_API extgpu_status extgpu_warm_release_vm_rtx3090_preflight(extgpu_device *device);
/* Stop vCPUs and retire IRQ before calling. Revalidates the scope pinned by
 * preflight. The exact CLOSED receipt can be recovered on the same client. */
EXTGPU_API extgpu_status extgpu_warm_release_vm_rtx3090(extgpu_device *device, extgpu_buffer **window);
/* Pin a pristine Polaris display 1002:67df or 1002:6fdf with its 1002:aaf0
 * HDMI audio sibling as the entire directly downstream PCI reset domain. */
EXTGPU_API extgpu_status extgpu_warm_release_vm_polaris_preflight(extgpu_device *device);
/* The pinned pair is revalidated before the scoped PCI hot reset. Only the
 * exact completed release on the original client permits freeing RAM. */
EXTGPU_API extgpu_status extgpu_warm_release_vm_polaris(extgpu_device *device, extgpu_buffer **window);
EXTGPU_API extgpu_status extgpu_dma_probe(extgpu_buffer *buffer, uint64_t offset,
                               uint64_t *out_word);

typedef void (*extgpu_irq_callback)(extgpu_irq *irq, void *context);
/* Callback runs on a private serial queue. It may drain/rearm, but cannot close
 * this IRQ from that queue. Close elsewhere after stopping caller activity.
 * On EXTGPU_E_UNSAFE, *out_irq is retained: setup acquired driver state before
 * vector validation or initial arm failed. Keep callback context/device/claim
 * alive and call irq_close even after create failed. Only successful close
 * clears the handle; device_last_error preserves the failing native code/stage. */
EXTGPU_API extgpu_status extgpu_irq_create(extgpu_device *device, extgpu_irq_callback callback,
                                void *context, extgpu_irq **out_irq,
                                uint32_t *out_vector_count);
/* Requires support for a persistent notification backed by a shared CPU
 * bitmap. At most one shared IRQ per device connection. On UNSUPPORTED, no
 * IRQ was created and the caller can use extgpu_irq_create. Drain from the
 * callback. Rearming a shared handle needs no driver request. */
EXTGPU_API extgpu_status extgpu_irq_create_shared(extgpu_device *device, extgpu_irq_callback callback,
                                void *context, extgpu_irq **out_irq,
                                uint32_t *out_vector_count);
EXTGPU_API extgpu_status extgpu_irq_set_mask(extgpu_irq *irq, const uint64_t mask[EXTGPU_IRQ_WORDS]);
EXTGPU_API extgpu_status extgpu_irq_drain(extgpu_irq *irq, uint64_t pending[EXTGPU_IRQ_WORDS]);
EXTGPU_API extgpu_status extgpu_irq_rearm(extgpu_irq *irq);
/* Drain a delivered notification and install the next callback. Drivers that
 * support the combined operation do both under the same interrupt lock.
 * For a one-shot IRQ, requires a disarmed IRQ. On error, do not replay the
 * combined operation. For a shared IRQ, this drains the CPU bitmap. */
EXTGPU_API extgpu_status extgpu_irq_drain_and_rearm(extgpu_irq *irq, uint64_t pending[EXTGPU_IRQ_WORDS]);
/* Requires acknowledged physical source retirement and local callback drain.
 * A pending/failed retirement preserves the handle and is retryable. Call on
 * a control thread: close from any SDK IRQ callback queue returns BUSY. */
EXTGPU_API extgpu_status extgpu_irq_close(extgpu_irq **irq);

/* Handle destruction and direct pointer access require caller synchronization.
 * Synchronous IOKit RPCs have no SDK timeout/cancellation guarantee. Neither
 * transport success nor DMA coverage proves GPU/rendering initialization. */
#ifdef __cplusplus
}
#endif
#endif
