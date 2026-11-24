// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "DirectBARPublicKey.hpp"
#include <os/log.h>

#include <string.h>
#include <stdlib.h>

extern "C" {
struct ccdigest_info;
const ccdigest_info *ccsha512_di();
int cced25519_verify(const ccdigest_info *, size_t, const void *, const uint8_t[64], const uint8_t[32]);
}

#include <time.h>

#include <DriverKit/OSMetaClass.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSBoolean.h>
#include <DriverKit/OSArray.h>
#include <DriverKit/OSString.h>
#include <DriverKit/OSData.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/IODMACommand.h>
#include <DriverKit/IOUserServer.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOInterruptDispatchSource.h>
#include <DriverKit/IODispatchQueue.h>
#include <PCIDriverKit/IOPCIDevice.h>
#include <PCIDriverKit/IOPCIFamilyDefinitions.h>

#include <extgpu/extgpu.h>
#include <extgpu/extgpu_driver_abi.h>
#include <extgpu/private_mmio_window.h>
#include <extgpu/private_mmio_ring.h>
#include <extgpu/private_irq_mailbox.h>
#include "VFIOUserPCIDriver.h"
#include "VFIOUserPCIDriverUserClient.h"
#include "DMARetirement.hpp"
#include "OwnershipRecovery.hpp"
#include "IRQCancellation.hpp"
#include "MMIOBoundsCache.hpp"
#include "ColdRemovalLedger.hpp"
#include "WarmVMRelease.hpp"
#include "VegaBarAssignments.hpp"
#include "privateRecoveryBrokerSupport.hpp"


constexpr unsigned maxInterruptVectors = EXTGPU_IRQ_BITMAP_WORDS * 64;

// One record owns one prepared DriverKit DMA mapping. An occupied record
// survives failed completion, including a failed rollback during registration.
struct ClientDMARegistration {
    uint64_t key;
    uint64_t bytes;
    IOMemoryDescriptor *memory;
    IODMACommand *command;
    bool fullCoverage;
    bool vmSegmented;
    bool vmDynamic;
    bool completionFailed;
};

struct VFIOUserPCIDriverUserClient_IVars;

/* DriverKit exposes IOLock, not the macOS os_unfair_lock API. */
struct ReadyDriverLock { IOLock *value; };

struct VFIOUserPCIDriver_IVars {
    bool providerClaimed;
    uint32_t providerClaimRefs;
    VFIOUserPCIDriverUserClient *openerClient;
    ReadyDriverLock stateLock;
    bool stopping;
    bool quarantined;
    bool trustedQuiescent;
    bool resetCompanion;
    bool resetCompanionInUse;
    uint64_t generation;
    VFIOUserPCIDriverUserClient_IVars *quarantinedClientState;
    /* One retained descriptor per BAR, owned by this PCI function only. */
    IOMemoryDescriptor *barDescCache[6];
    /* Exclusive provider/generation scope; host BAR placement writes are
     * denied. Reset attempts, claim/release and Stop invalidate this metadata.
     * Unannounced host ReBAR/power transitions are not detected by this cache.
     */
    extgpu_driver::MMIOBoundsCache mmioBoundsCache;
};

struct VFIOUserPCIDriverUserClient_IVars {
    bool authProbeBarIssued;
    bool authProbeMSEEnabled;
    uint16_t authProbeOriginalCommand;
    bool authProbeIssued;
    bool authProbeUsed;
    bool authFullVM;
    bool authBarRetired;
    uint64_t authProbeChallenge[3];

    bool claimed;
    bool claimPristine;
    bool protocolNegotiated;
    uint64_t negotiatedGeneration;
    bool barMappingIssued;
    bool hardwareMutated;
    bool everPreparedDMA;
    bool dmaRequested;
    uint32_t dmaAddressBits;
    bool ownerReleased;
    extgpu_driver::WarmVMKey startupDMARequest;
    extgpu_driver::WarmVMKey dynamicDMAWindow;
    extgpu_driver::WarmVMRelease warmVMRelease;
    VFIOUserPCIDriver *warmAudioOwner; /* explicit retained same-server cofunction */
    /* One bounded frozen ledger per original provider/client. No broker or
     * endpoint-survival guarantee: unfinished state remains quarantined. */
    bool coldPrepared; /* Escrow owns original before freeze; ordinary IO is terminal. */
    bool coldFrozen;
    bool coldStopped;
    bool coldBMEAcknowledged;
    bool coldFinished;
    VFIOUserPCIDriver *coldDriver; /* retained before freeze; no post-Stop PCI use */
    extgpu_driver::ColdRemovalLedger coldLedger;
    ReadyDriverLock operationLock;
    IOBufferMemoryDescriptor *mmioWindowMemory;
    openpci_mmio_window *mmioWindow;
    uint64_t mmioWindowGeneration;
    IOBufferMemoryDescriptor *mmioRingMemory;
    IODispatchQueue *mmioRingQueue;
    openpci_mmio_ring *mmioRing;
    uint64_t mmioRingGeneration, mmioRingConsumed;
    bool mmioRingScheduled;
    uint64_t mmioRingIdleNs, mmioRingFlags, mmioRingIrqDeadlineNs;
    unsigned stateReferences;
    extgpu_driver::MMIOMemoryEnableCheck mmioMemoryEnable;
    ReadyDriverLock irqLock;
    uint32_t irqCancelPending;
    uint32_t irqCancelRemaining; /* irqLock; includes failed/uncertain calls. */
    kern_return_t irqCancelFailure;
    IOBufferMemoryDescriptor *dmaBuffer;
    IODMACommand *dmaCommand;
    uint64_t dmaBufferSize;
    uint64_t dmaFlags;
    IOAddressSegment dmaSegment;
    ClientDMARegistration *clientDMARegions;
    uint64_t activeDMARegionCount;
    uint64_t activeDMABytes;
    uint64_t dmaByteBudget;
    IOAddressSegment vmDMASegments[EXTGPU_VM_DMA_SEGMENT_LIMIT];
    uint32_t vmDMASegmentCount;
    uint64_t dmaCompleteFailureCount;
    uint64_t dmaRetirementFailuresPending;
    bool dmaBufferRetirementFailed;

    bool interruptsSetUp;
    bool irqStateExported;
    uint32_t numInterrupts;
    IOInterruptDispatchSource *interruptSources[maxInterruptVectors];
    extgpu_driver::IRQCancellation irqCancellations[maxInterruptVectors];
    IODispatchQueue *irqQueue;

    // The mapped descriptor exposes two consecutive arrays of 64-bit bitmap
    // words. The driver publishes pending bits and the client owns the mask.
    IOBufferMemoryDescriptor *irqSharedBuffer;
    volatile uint64_t *irqPending;
    volatile uint64_t *irqEnabled;
    OSAction *pendingInterruptNotify;
    IOBufferMemoryDescriptor *irqMailboxMemory;
    openpci_irq_mailbox *irqMailbox;
    uint64_t irqMailboxGeneration;
    bool irqMailboxUsed, irqPersistent;
};

struct VFIOUserPCIDeviceIdentity {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendorID;
    uint16_t deviceID;
    uint32_t classCode;
};

// The caller holds the connection's operation lock. The native call is
// shared by unregister, registration rollback, and broker-backed retirement.
static kern_return_t complete_registration(VFIOUserPCIDriverUserClient_IVars &state,
                                           ClientDMARegistration &registration)
{
    const auto status = registration.command->CompleteDMA(kIODMACommandCompleteDMANoOptions);
    if (status != kIOReturnSuccess) {
        extgpu_driver::record_completion_failure(registration.completionFailed,
            state.dmaRetirementFailuresPending, state.dmaCompleteFailureCount);
        os_log(OS_LOG_DEFAULT, "OpenPCI retained DMA registration=%#llx status=%#x",
               registration.key, status);
    }
    return status;
}

static void release_registration_command(VFIOUserPCIDriverUserClient_IVars &state,
                                         ClientDMARegistration &registration)
{
    OSSafeReleaseNULL(registration.command);
    extgpu_driver::record_completion_success(registration.completionFailed,
                                            state.dmaRetirementFailuresPending);
}

static void erase_completed_registration(VFIOUserPCIDriverUserClient_IVars &state,
                                         ClientDMARegistration &registration)
{
    OSSafeReleaseNULL(registration.memory);
    if (registration.vmSegmented) state.vmDMASegmentCount = 0;
    --state.activeDMARegionCount;
    state.activeDMABytes -= registration.bytes;
    registration = {};
}

static kern_return_t retire_registration(VFIOUserPCIDriverUserClient_IVars &state,
                                        ClientDMARegistration &registration)
{
    const auto status = complete_registration(state, registration);
    if (status != kIOReturnSuccess) return status;
    release_registration_command(state, registration);
    erase_completed_registration(state, registration);
    return kIOReturnSuccess;
}

/* Serializes ownership and BAR cache state for one provider. This is not
 * shared between devices, even if several instances use one IOUserServer.
 */
class ReadyProviderLock {
    IOLock *lock_;
public:
    explicit ReadyProviderLock(ReadyDriverLock *lock) : lock_(lock->value) {
        IOLockLock(lock_);
    }
    ~ReadyProviderLock() { IOLockUnlock(lock_); }
    ReadyProviderLock(const ReadyProviderLock &) = delete;
    ReadyProviderLock &operator=(const ReadyProviderLock &) = delete;
};

/* Exact live service references for the admitted audio companions. The
 * table lock is dropped before taking either provider's ownership lock. */
static ReadyDriverLock resetCompanionLock{};
struct ResetCompanionEntry {
    uint64_t pci_registry;
    VFIOUserPCIDriver *driver;
};
static ResetCompanionEntry resetCompanions[8]{};

static kern_return_t register_reset_companion(VFIOUserPCIDriver *driver, uint64_t registry)
{
    if (!registry) return kIOReturnBadArgument;
    if (!__atomic_load_n(&resetCompanionLock.value, __ATOMIC_ACQUIRE)) {
        IOLock *fresh = IOLockAlloc(), *expected = nullptr;
        if (!fresh) return kIOReturnNoMemory;
        if (!__atomic_compare_exchange_n(&resetCompanionLock.value, &expected, fresh, false,
                                         __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
            IOLockFree(fresh);
    }
    ReadyProviderLock lock(&resetCompanionLock);
    ResetCompanionEntry *free = nullptr;
    for (auto &entry : resetCompanions) {
        if (entry.pci_registry == registry) return kIOReturnExclusiveAccess;
        if (!entry.driver && !free) free = &entry;
    }
    if (!free) return kIOReturnNoSpace;
    driver->retain();
    *free = {registry, driver};
    return kIOReturnSuccess;
}

static void unregister_reset_companion(VFIOUserPCIDriver *driver)
{
    auto *lockValue = __atomic_load_n(&resetCompanionLock.value, __ATOMIC_ACQUIRE);
    if (!lockValue) return;
    VFIOUserPCIDriver *owned = nullptr;
    {
        ReadyProviderLock lock(&resetCompanionLock);
        for (auto &entry : resetCompanions) {
            if (entry.driver != driver) continue;
            owned = entry.driver;
            entry = {};
            break;
        }
    }
    if (owned) owned->release();
}

static VFIOUserPCIDriver *retain_reset_companion(uint64_t registry)
{
    if (!__atomic_load_n(&resetCompanionLock.value, __ATOMIC_ACQUIRE)) return nullptr;
    ReadyProviderLock lock(&resetCompanionLock);
    for (const auto &entry : resetCompanions) {
        if (entry.driver && entry.pci_registry == registry) {
            entry.driver->retain();
            return entry.driver;
        }
    }
    return nullptr;
}

/* Caller owns this provider's stateLock. Returned client mappings hold
 * their own descriptor references; invalidation prevents new stale reuse.
 */
static void ready_clear_bar_cache_locked(VFIOUserPCIDriver *driver)
{
    driver->ivars->mmioBoundsCache.clear();
    for (unsigned i = 0; i < 6; ++i) {
        if (driver->ivars->barDescCache[i] != nullptr) {
            driver->ivars->barDescCache[i]->release();
            driver->ivars->barDescCache[i] = nullptr;
        }
    }
}

/* GetProvider returns a cached, borrowed provider. Do not transfer ownership
 * to callers or let the analyzer assume the default +1 C-function result. */
static IOPCIDevice *vfio_user_get_pci_device(IOService *) DRIVERKIT_RETURNS_NOT_RETAINED;

static IOPCIDevice *vfio_user_get_pci_device(IOService *service)
{
    return service ? OSDynamicCast(IOPCIDevice, service->GetProvider()) : nullptr;
}

/* Discovery must not touch config/aperture registers before explicit Open.
 * PCI registry properties are kernel-published little-endian values. */
static bool ready_pci_property(OSDictionary *properties, const char *key, uint32_t *value)
{
    OSObject *object = properties->getObject(key);
    if (auto *data = OSDynamicCast(OSData, object)) {
        if (data->getLength() < 4) return false;
        const auto *bytes = static_cast<const uint8_t *>(data->getBytesNoCopy());
        if (bytes == nullptr) return false;
        *value = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
                 ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
        return true;
    }
    if (auto *number = OSDynamicCast(OSNumber, object)) {
        const uint64_t n = number->unsigned64BitValue();
        if (n > UINT32_MAX) return false;
        *value = (uint32_t)n;
        return true;
    }
    return false;
}

static kern_return_t vfio_user_read_identity(IOService *service, VFIOUserPCIDeviceIdentity *identity)
{
    auto *pci = vfio_user_get_pci_device(service);
    if (!pci) return kIOReturnUnsupported;
    VFIOUserPCIDeviceIdentity result{};
    auto status = pci->GetBusDeviceFunction(&result.bus, &result.device, &result.function);
    if (status != kIOReturnSuccess) return status;

    OSDictionary *properties = nullptr;
    status = pci->CopyProperties(&properties);
    uint32_t vendor = 0, deviceID = 0, pciClass = 0;
    const bool complete = status == kIOReturnSuccess && properties &&
        ready_pci_property(properties, "vendor-id", &vendor) && vendor <= UINT16_MAX &&
        ready_pci_property(properties, "device-id", &deviceID) && deviceID <= UINT16_MAX &&
        ready_pci_property(properties, "class-code", &pciClass) && pciClass <= 0x00ffffff;
    OSSafeReleaseNULL(properties);
    if (status != kIOReturnSuccess) return status;
    if (!complete) return kIOReturnNotFound;

    result.vendorID = static_cast<uint16_t>(vendor);
    result.deviceID = static_cast<uint16_t>(deviceID);
    result.classCode = pciClass;
    *identity = result;
    return kIOReturnSuccess;
}

/* Common transport: AMD/NVIDIA/Intel display-class PCI functions only,
 * any device/subsystem, with the actual PCI provider explicitly tunneled.
 * Matching registers an idle service. Linux owns vendor initialization.
 */
static kern_return_t ready_validate_external_gpu(IOPCIDevice *pciDevice)
{
    if (pciDevice == nullptr) return kIOReturnBadArgument;
    OSDictionary *properties = nullptr;
    kern_return_t ret = pciDevice->CopyProperties(&properties);
    if (ret != kIOReturnSuccess || properties == nullptr) {
        if (properties != nullptr) properties->release();
        return ret != kIOReturnSuccess ? ret : kIOReturnUnsupported;
    }
    uint32_t vendor = 0, classCode = 0;
    const bool valid = ready_pci_property(properties, "vendor-id", &vendor) &&
                       ready_pci_property(properties, "class-code", &classCode);
    const bool supportedVendor = vendor == 0x1002 || vendor == 0x10de || vendor == 0x8086;
    const bool tunneled = properties->getObject("IOPCITunnelled") == kOSBooleanTrue;
    properties->release();
    if (!valid || !supportedVendor || (classCode & 0x00ff0000) != 0x00030000) {
        os_log(OS_LOG_DEFAULT, "ready-vfio: refusing out-of-scope PCI registry identity vendor=%x class=%x", vendor, classCode);
        return kIOReturnUnsupported;
    }
    if (!tunneled) {
        os_log(OS_LOG_DEFAULT, "ready-vfio: refusing non-tunneled PCI provider");
        return kIOReturnUnsupported;
    }
    return kIOReturnSuccess;
}

static bool ready_is_reset_companion(IOPCIDevice *pciDevice, const VFIOUserPCIDeviceIdentity &id)
{
    if ((id.classCode >> 8) != 0x0403 || id.function != 1)
        return false;
    OSDictionary *properties = nullptr;
    auto status = pciDevice->CopyProperties(&properties);
    bool tunneled = status == kIOReturnSuccess && properties &&
        properties->getObject("IOPCITunnelled") == kOSBooleanTrue;
    if (properties) properties->release();
    return tunneled;
}

static kern_return_t vfio_user_claim_exclusive(VFIOUserPCIDriverUserClient *client,
                                               VFIOUserPCIDriver *driver)
{
    auto &provider = *driver->ivars;
    auto &session = *client->ivars;
    ReadyProviderLock lock(&provider.stateLock);
    if (provider.stopping) return kIOReturnNotAttached;
    if (provider.quarantined) return kIOReturnNotReady;
    if (!session.protocolNegotiated || session.negotiatedGeneration != provider.generation)
        return kIOReturnNotPermitted;
    if (provider.providerClaimed)
        return provider.openerClient == client && session.claimed ?
            kIOReturnSuccess : kIOReturnExclusiveAccess;

    ready_clear_bar_cache_locked(driver);
    auto *pci = vfio_user_get_pci_device(driver);
    if (!pci) return kIOReturnUnsupported;
    auto status = ready_validate_external_gpu(pci);
    if (status == kIOReturnSuccess) status = pci->Open(client, 0);
    if (status != kIOReturnSuccess) return status;

    provider.openerClient = client;
    provider.providerClaimRefs = 1;
    provider.providerClaimed = true;
    session.claimed = true;
    session.claimPristine = true;
    session.startupDMARequest = {};
    session.dynamicDMAWindow = {};
    session.dmaRequested = false;
    session.vmDMASegmentCount = 0;
    session.ownerReleased = false;
    return kIOReturnSuccess;
}

/* The caller holds the operation and provider locks. */
static kern_return_t ready_owner_status_locked(VFIOUserPCIDriverUserClient *client,
                                                VFIOUserPCIDriver *driver, uint64_t generation)
{
    const auto *device = driver->ivars;
    const auto *state = client->ivars;
    if (device->stopping) return kIOReturnNotAttached;
    if (device->quarantined) return kIOReturnNotReady;
    if (!state->protocolNegotiated || state->negotiatedGeneration != generation ||
        device->generation != generation) return kIOReturnNotPermitted;
    if (!state->claimed || !device->providerClaimed || device->openerClient != client)
        return kIOReturnNotOpen;
    return device->providerClaimRefs == 1 ? kIOReturnSuccess : kIOReturnError;
}

/* Serialize Close and publication of release with the provider's Stop. The
 * caller has verified the live original exclusive claim under the same lock. */
static void ready_close_claim_locked(VFIOUserPCIDriverUserClient *client,
                                     VFIOUserPCIDriver *driver, IOPCIDevice *pci,
                                     bool reset_verified)
{
    ready_clear_bar_cache_locked(driver);
    if (auto *ring = client->ivars->mmioRing)
        __atomic_store_n(&ring->active, OPENPCI_RING_RETIRED, __ATOMIC_RELEASE);
    pci->Close(client, 0);
    auto *device = driver->ivars;
    device->providerClaimRefs = 0;
    device->providerClaimed = false;
    device->openerClient = nullptr;
    ++device->generation;
    if (reset_verified) device->trustedQuiescent = true;
    client->ivars->claimed = false;
    client->ivars->dynamicDMAWindow = {};
    client->ivars->dmaByteBudget = EXTGPU_DEFAULT_DMA_BUDGET_BYTES;
    client->ivars->ownerReleased = true;
    client->ivars->protocolNegotiated = false;
}

static kern_return_t vfio_user_release_dma_buffer(VFIOUserPCIDriverUserClient *client)
{
    auto &state = *client->ivars;
    if (!state.dmaCommand) return kIOReturnSuccess;
    const auto status = state.dmaCommand->CompleteDMA(kIODMACommandCompleteDMANoOptions);
    if (status != kIOReturnSuccess) {
        extgpu_driver::record_completion_failure(state.dmaBufferRetirementFailed,
            state.dmaRetirementFailuresPending, state.dmaCompleteFailureCount);
        return status;
    }
    extgpu_driver::record_completion_success(state.dmaBufferRetirementFailed,
        state.dmaRetirementFailuresPending);
    OSSafeReleaseNULL(state.dmaCommand);
    OSSafeReleaseNULL(state.dmaBuffer);
    state.dmaSegment = {};
    state.dmaBufferSize = 0;
    state.dmaFlags = 0;
    return kIOReturnSuccess;
}

static kern_return_t prepare_dma(IOPCIDevice *device, IOMemoryDescriptor *memory,
                                 uint32_t addressBits, uint64_t bytes,
                                 IODMACommand **command, uint64_t *flags,
                                 IOAddressSegment *segments, uint32_t *segmentCount)
{
    IODMACommandSpecification specification{};
    specification.maxAddressBits = addressBits;
    IODMACommand *prepared = nullptr;
    auto status = IODMACommand::Create(device, kIODMACommandCreateNoOptions,
                                       &specification, &prepared);
    if (status != kIOReturnSuccess || !prepared) {
        OSSafeReleaseNULL(prepared);
        return status == kIOReturnSuccess ? kIOReturnNoMemory : status;
    }

    // The generated RPC expects storage for its full 32-entry reply.
    uint32_t count = EXTGPU_VM_DMA_SEGMENT_LIMIT;
    status = prepared->PrepareForDMA(kIODMACommandPrepareForDMANoOptions,
                                     memory, 0, bytes, flags, &count, segments);
    if (status != kIOReturnSuccess) {
        prepared->release();
        return status;
    }
    *command = prepared;
    *segmentCount = count;
    return kIOReturnSuccess;
}

static bool dma_covers_request(const IOAddressSegment &segment, uint32_t count,
                               uint64_t bytes, uint32_t addressBits)
{
    if (count != 1 || segment.length != bytes || segment.address > UINT64_MAX - bytes)
        return false;
    return addressBits == 64 || segment.address + bytes <= (UINT64_C(1) << addressBits);
}

static bool dma_segments_cover_request(const IOAddressSegment *segments, uint32_t count,
                                       uint64_t bytes, uint32_t addressBits)
{
    if (!count || count > EXTGPU_VM_DMA_SEGMENT_LIMIT) return false;
    uint64_t remaining = bytes;
    for (uint32_t i = 0; i < count; ++i) {
        const auto &segment = segments[i];
        if (!segment.length || segment.length > remaining ||
            ((segment.address | segment.length) & UINT64_C(0x3fff)) ||
            !dma_covers_request(segment, 1, segment.length, addressBits)) return false;
        for (uint32_t j = 0; j < i; ++j) {
            const auto &other = segments[j];
            if (segment.address < other.address + other.length &&
                other.address < segment.address + segment.length) return false;
        }
        remaining -= segment.length;
    }
    return !remaining;
}

static uint64_t dma_budget_available(const VFIOUserPCIDriverUserClient_IVars &state)
{
    if (state.activeDMABytes > state.dmaByteBudget) return 0;
    const auto afterRegistered = state.dmaByteBudget - state.activeDMABytes;
    return state.dmaBufferSize > afterRegistered ? 0 : afterRegistered - state.dmaBufferSize;
}

static kern_return_t vfio_user_allocate_dma_buffer(VFIOUserPCIDriverUserClient *client,
                                                   uint64_t bytes, uint64_t alignment)
{
    auto &state = *client->ivars;
    if (state.dynamicDMAWindow.bytes) return kIOReturnNotPermitted;
    if (!bytes) return kIOReturnBadArgument;
    auto *device = vfio_user_get_pci_device(client->GetProvider());
    if (!device) return kIOReturnUnsupported;
    if (state.dmaBuffer) return kIOReturnStillOpen;
    if (bytes > dma_budget_available(state)) return kIOReturnNoSpace;
    if (!alignment) alignment = 16384;
    if (alignment & (alignment - 1)) return kIOReturnBadArgument;

    IOBufferMemoryDescriptor *memory = nullptr;
    auto status = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionOutIn, bytes,
                                                   alignment, &memory);
    if (status != kIOReturnSuccess || !memory) {
        OSSafeReleaseNULL(memory);
        return status == kIOReturnSuccess ? kIOReturnNoMemory : status;
    }

    IOAddressSegment segments[EXTGPU_VM_DMA_SEGMENT_LIMIT]{};
    uint32_t count = 0;
    uint64_t flags = 0;
    IODMACommand *command = nullptr;
    status = memory->SetLength(bytes);
    if (status == kIOReturnSuccess)
        status = prepare_dma(device, memory, state.dmaAddressBits, bytes,
                             &command, &flags, segments, &count);
    if (status != kIOReturnSuccess) {
        memory->release();
        return status;
    }

    // From here until acknowledged completion the session owns both objects.
    state.everPreparedDMA = true;
    state.dmaBufferSize = bytes;
    state.dmaBuffer = memory;
    state.dmaCommand = command;
    if (!dma_covers_request(segments[0], count, bytes, state.dmaAddressBits) ||
        (segments[0].address & (alignment - 1))) {
        const auto completion = vfio_user_release_dma_buffer(client);
        return completion == kIOReturnSuccess ? kIOReturnNotAligned : completion;
    }

    state.dmaFlags = flags;
    state.dmaSegment = segments[0];
    return kIOReturnSuccess;
}

static kern_return_t vfio_user_register_client_dma_region(VFIOUserPCIDriverUserClient *client,
                                                          uint64_t key, uint64_t address,
                                                          uint64_t bytes,
                                                          IOAddressSegment *busSegment, bool segmented, bool dynamic)
{
    auto &state = *client->ivars;
    if (!bytes || !address) return kIOReturnBadArgument;
    auto *device = vfio_user_get_pci_device(client->GetProvider());
    if (!device) return kIOReturnUnsupported;
    if (bytes > dma_budget_available(state) || key > UINT64_MAX - bytes || address > UINT64_MAX - bytes)
        return kIOReturnNoSpace;

    if (!state.clientDMARegions) {
        state.clientDMARegions = IONewZero(ClientDMARegistration, EXTGPU_MAX_DMA_REGIONS);
        if (!state.clientDMARegions) return kIOReturnNoMemory;
    }

    ClientDMARegistration *slot = nullptr;
    for (size_t i = 0; i < EXTGPU_MAX_DMA_REGIONS; ++i) {
        auto &region = state.clientDMARegions[i];
        if (!region.command) {
            if (!slot) slot = &region;
        } else if (key < region.key + region.bytes && region.key < key + bytes) {
            return kIOReturnStillOpen;
        }
    }
    if (!slot) return kIOReturnNoSpace;

    ClientDMARegistration prepared{};
    const IOAddressSegment clientRange{address, bytes};
    auto status = client->CreateMemoryDescriptorFromClient(kIOMemoryDirectionOutIn, 1,
                                                          &clientRange, &prepared.memory);
    if (status == kIOReturnSuccess && !prepared.memory) status = kIOReturnNoMemory;
    IOAddressSegment segments[EXTGPU_VM_DMA_SEGMENT_LIMIT]{};
    uint64_t flags = 0;
    uint32_t count = 0;
    if (status == kIOReturnSuccess)
        status = prepare_dma(device, prepared.memory, state.dmaAddressBits,
                             bytes, &prepared.command, &flags, segments, &count);
    if (status != kIOReturnSuccess) {
        OSSafeReleaseNULL(prepared.memory);
        os_log(OS_LOG_DEFAULT, "OpenPCI DMA prepare key=%#llx bytes=%llu status=%#x",
               key, bytes, status);
        return status;
    }

    // Record native ownership before validating coverage. Failed completion
    // must leave the original registration available for retirement.
    prepared.key = key;
    prepared.bytes = bytes;
    prepared.vmSegmented = segmented;
    prepared.vmDynamic = dynamic;
    prepared.fullCoverage = segmented ? dma_segments_cover_request(segments, count, bytes, state.dmaAddressBits) :
        dma_covers_request(segments[0], count, bytes, state.dmaAddressBits);
    *slot = prepared;
    ++state.activeDMARegionCount;
    state.activeDMABytes += bytes;
    state.everPreparedDMA = true;
    if (prepared.fullCoverage) {
        if (segmented) {
            memcpy(state.vmDMASegments, segments, count * sizeof(segments[0]));
            state.vmDMASegmentCount = count;
        }
        *busSegment = segments[0];
        return kIOReturnSuccess;
    }

    const auto completion = retire_registration(state, *slot);
    if (completion == kIOReturnSuccess) return kIOReturnNotAligned;
    // StillOpen is reserved for an overlap rejected before DMA preparation.
    return dynamic || completion == kIOReturnStillOpen ? kIOReturnError : completion;
}

static ClientDMARegistration *find_dma_region(VFIOUserPCIDriverUserClient_IVars &state, uint64_t key)
{
    if (!state.clientDMARegions) return nullptr;
    for (size_t i = 0; i < EXTGPU_MAX_DMA_REGIONS; ++i) {
        auto &region = state.clientDMARegions[i];
        if (region.command && region.key == key) return &region;
    }
    return nullptr;
}

static kern_return_t vfio_user_unregister_client_dma_region(VFIOUserPCIDriverUserClient *client,
                                                            uint64_t key)
{
    auto *region = find_dma_region(*client->ivars, key);
    if (!region) return kIOReturnNotFound;
    return retire_registration(*client->ivars, *region);
}

static kern_return_t vfio_user_probe_client_dma_region(VFIOUserPCIDriverUserClient *client,
                                                        uint64_t key, uint64_t offset,
                                                        uint64_t *word)
{
    if (offset % sizeof(*word)) return kIOReturnBadArgument;
    const auto *region = find_dma_region(*client->ivars, key);
    if (!region) return kIOReturnNotFound;
    if (region->bytes < sizeof(*word) || offset > region->bytes - sizeof(*word))
        return kIOReturnBadArgument;
    if (region->completionFailed || !region->fullCoverage || !region->memory)
        return kIOReturnNotReady;

    IOMemoryMap *map = nullptr;
    const auto status = region->memory->CreateMapping(0, 0, 0, 0, 0, &map);
    if (status != kIOReturnSuccess || !map) {
        OSSafeReleaseNULL(map);
        return status == kIOReturnSuccess ? kIOReturnError : status;
    }
    const auto length = map->GetLength();
    const auto address = map->GetAddress();
    const bool readable = address && length >= sizeof(*word) && offset <= length - sizeof(*word);
    if (readable)
        *word = *reinterpret_cast<const volatile uint64_t *>(address + offset);
    map->release();
    return readable ? kIOReturnSuccess : kIOReturnBadArgument;
}

static bool valid_config_access(uint64_t offset, uint64_t width)
{
    return (width == 1 || width == 2 || width == 4) &&
        !(offset % width) && offset <= 4096 - width;
}

static kern_return_t vfio_user_config_read(VFIOUserPCIDriverUserClient *client,
                                           VFIOUserPCIDriver *driver, uint64_t offset,
                                           uint64_t width, uint64_t *out)
{
    if (!valid_config_access(offset, width)) return kIOReturnBadArgument;
    ReadyProviderLock lock(&driver->ivars->stateLock);
    auto status = ready_owner_status_locked(client, driver, client->ivars->negotiatedGeneration);
    if (status != kIOReturnSuccess) return status;
    auto *pci = vfio_user_get_pci_device(driver);
    if (!pci) return kIOReturnUnsupported;

    // Present BAR attributes in the aligned PCI header without physical addresses.
    constexpr uint64_t firstBAR = 0x10;
    if (width == 4 && offset >= firstBAR && offset < firstBAR + 6 * 4) {
        uint8_t index = 0, attributes = 0;
        uint64_t size = 0;
        const auto bar = static_cast<uint8_t>((offset - firstBAR) / 4);
        status = pci->GetBARInfo(bar, &index, &size, &attributes);
        *out = status == kIOReturnSuccess && size ? attributes : 0;
    } else if (width == 4) {
        uint32_t value = UINT32_MAX;
        pci->ConfigurationRead32(offset, &value);
        *out = value;
    } else if (width == 2) {
        uint16_t value = UINT16_MAX;
        pci->ConfigurationRead16(offset, &value);
        if (offset == 4 && value == UINT16_MAX) return kIOReturnNotReady;
        *out = value;
    } else {
        uint8_t value = UINT8_MAX;
        pci->ConfigurationRead8(offset, &value);
        *out = value;
    }
    return kIOReturnSuccess;
}

static kern_return_t vfio_user_config_write(VFIOUserPCIDriverUserClient *client,
                                            VFIOUserPCIDriver *driver, uint64_t offset,
                                            uint64_t width, uint64_t value)
{
    if (!valid_config_access(offset, width)) return kIOReturnBadArgument;
    ReadyProviderLock lock(&driver->ivars->stateLock);
    const auto status = ready_owner_status_locked(client, driver, client->ivars->negotiatedGeneration);
    if (status != kIOReturnSuccess) return status;
    auto *pci = vfio_user_get_pci_device(driver);
    if (!pci) return kIOReturnUnsupported;

    constexpr uint64_t commandOffset = 4;
    constexpr uint16_t clientCommandBits = 0x0006; // Memory Space and Bus Master.
    if (offset < commandOffset || offset + width > commandOffset + 2)
        return kIOReturnNotPermitted;
    uint16_t command = UINT16_MAX;
    pci->ConfigurationRead16(commandOffset, &command);
    if (command == UINT16_MAX) return kIOReturnNotReady;

    const auto shift = static_cast<unsigned>(8 * (offset - commandOffset));
    const auto mask = static_cast<uint16_t>((width == 1 ? 0xffu : 0xffffu) << shift);
    const auto requested = static_cast<uint16_t>(value << shift);
    if ((command ^ requested) & mask & ~clientCommandBits) return kIOReturnNotPermitted;

    client->ivars->hardwareMutated = true;
    if (width == 1)
        pci->ConfigurationWrite8(offset, static_cast<uint8_t>(value));
    else
        pci->ConfigurationWrite16(commandOffset, static_cast<uint16_t>(value));
    return kIOReturnSuccess;
}

/* Caller owns the provider stateLock through this check and typed I/O.
 * Cache entries are bound to the current provider and owner generation. */
static kern_return_t ready_validate_mmio(VFIOUserPCIDriver *driver,
                                        VFIOUserPCIDriverUserClient *client,
                                        IOPCIDevice *pci, uint64_t index,
                                        uint64_t offset, uint64_t width)
{
    struct Metadata {
        IOPCIDevice *pci;
        kern_return_t failure;
        extgpu_driver::MMIOBARQuery query_bar(uint8_t bar, extgpu_driver::MMIOBARBounds &value)
        {
            const kern_return_t ret = pci->GetBARInfo(bar, &value.memoryIndex,
                                                       &value.size, &value.type);
            if (ret == kIOReturnSuccess) return extgpu_driver::MMIOBARQuery::found;
            if (ret == kIOReturnNotFound) return extgpu_driver::MMIOBARQuery::absent;
            failure = ret;
            return extgpu_driver::MMIOBARQuery::error;
        }
    } metadata{pci, kIOReturnBadArgument};
    const auto *state = driver->ivars;
    const extgpu_driver::MMIOBoundsContext context{
        reinterpret_cast<uintptr_t>(pci), state->generation,
        client->ivars->negotiatedGeneration, state->stopping,
        !state->quarantined && state->providerClaimed &&
            state->openerClient == client && client->ivars->claimed &&
            client->ivars->protocolNegotiated};
    const auto result = extgpu_driver::validate_mmio_bounds(
        driver->ivars->mmioBoundsCache, context, index, offset, width, metadata);
    switch (result) {
    case extgpu_driver::MMIOBoundsResult::success: return kIOReturnSuccess;
    case extgpu_driver::MMIOBoundsResult::bad_argument: return kIOReturnBadArgument;
    case extgpu_driver::MMIOBoundsResult::stale_generation: return kIOReturnNotPermitted;
    case extgpu_driver::MMIOBoundsResult::unavailable:
        return state->stopping ? kIOReturnNotAttached :
            (state->quarantined ? kIOReturnNotReady : kIOReturnNotOpen);
    case extgpu_driver::MMIOBoundsResult::not_found: return kIOReturnNotFound;
    case extgpu_driver::MMIOBoundsResult::unsupported: return kIOReturnUnsupported;
    case extgpu_driver::MMIOBoundsResult::metadata_error: return metadata.failure;
    }
    return kIOReturnError;
}

static void write_mmio_value(IOPCIDevice *pci, uint8_t index, uint64_t offset,
                             uint64_t width, uint64_t value)
{
    switch (width) {
    case 8: pci->MemoryWrite64(index, offset, value); break;
    case 4: pci->MemoryWrite32(index, offset, static_cast<uint32_t>(value)); break;
    case 2: pci->MemoryWrite16(index, offset, static_cast<uint16_t>(value)); break;
    case 1: pci->MemoryWrite8(index, offset, static_cast<uint8_t>(value)); break;
    }
}

static kern_return_t dispatch_scalar_mmio(VFIOUserPCIDriverUserClient *client,
                                          VFIOUserPCIDriver *driver,
                                          IOUserClientMethodArguments *arguments,
                                          bool read)
{
    auto &session = *client->ivars;
    auto &provider = *driver->ivars;
    // Hold one provider lock through owner validation, BAR checks and
    // the typed access. Provider Stop can't intervene between these steps.
    ReadyProviderLock lock(&provider.stateLock);
    if (!session.protocolNegotiated || session.negotiatedGeneration != provider.generation)
        return kIOReturnNotPermitted;
    const auto owner = ready_owner_status_locked(client, driver, session.negotiatedGeneration);
    if (owner != kIOReturnSuccess) return owner;

    // MMIO reads can have side effects. Preserve the existing claim
    // classification even if a validated owner supplies malformed arguments.
    session.startupDMARequest = {};
    session.claimPristine = false;
    session.hardwareMutated = true;
    const auto *request = arguments->scalarInput;
    auto *reply = read ? arguments->scalarOutput : nullptr;
    if (!request || arguments->scalarInputCount < (read ? 3u : 4u) ||
        (read && (!reply || !arguments->scalarOutputCount)))
        return kIOReturnBadArgument;

    const auto index = request[0], offset = request[1], width = request[2];
    auto *pci = vfio_user_get_pci_device(driver);
    if (!pci) return kIOReturnUnsupported;
    const auto bounds = ready_validate_mmio(driver, client, pci, index, offset, width);
    if (bounds != kIOReturnSuccess) return bounds;
    if (session.mmioMemoryEnable.checkedAtNs &&
        !extgpu_driver::prefetchable_mmio_bar(extgpu_driver::find_mmio_bar(provider.mmioBoundsCache, index)))
        session.mmioMemoryEnable = {};

    const auto bar = static_cast<uint8_t>(index);
    if (reply) {
        // DriverKit's typed readers return void. Initialize each width to PCI's
        // all-ones read value so an absent response cannot expose stack data.
        switch (width) {
        case 8: {
            uint64_t value = UINT64_MAX;
            pci->MemoryRead64(bar, offset, &value);
            *reply = value;
            break;
        }
        case 4: {
            uint32_t value = UINT32_MAX;
            pci->MemoryRead32(bar, offset, &value);
            *reply = value;
            break;
        }
        case 2: {
            uint16_t value = UINT16_MAX;
            pci->MemoryRead16(bar, offset, &value);
            *reply = value;
            break;
        }
        case 1: {
            uint8_t value = UINT8_MAX;
            pci->MemoryRead8(bar, offset, &value);
            *reply = value;
            break;
        }
        }
        arguments->scalarOutputCount = 1;
    } else {
        write_mmio_value(pci, bar, offset, width, request[3]);
    }
    return kIOReturnSuccess;
}

static kern_return_t vfio_user_bar_words(VFIOUserPCIDriverUserClient *client,
                                        VFIOUserPCIDriver *driver, IOUserClientMethodArguments *args);

/* The user-client operation lock is held by ExternalMethod. No task outlives
 * this call. Provider ownership and BAR bounds are rechecked for every I/O.
 */
static kern_return_t service_mmio_window(VFIOUserPCIDriverUserClient *client,
                                         VFIOUserPCIDriver *driver,
                                         IOUserClientMethodArguments *args)
{
    auto &session = *client->ivars;
    auto *window = session.mmioWindow;
    if (!window || args->scalarInputCount != 2 || !args->scalarInput || args->scalarOutputCount ||
        args->scalarInput[0] != session.mmioWindowGeneration ||
        session.mmioWindowGeneration != session.negotiatedGeneration)
        return kIOReturnBadArgument;
    uint64_t request = args->scalarInput[1];
    if (openpci_mmio_phase(request) != OPENPCI_MMIO_START || !openpci_mmio_ticket(request))
        return kIOReturnBadArgument;

    constexpr uint64_t maxWindowNs = OPENPCI_MMIO_WINDOW_SERVICE_NS;
    constexpr uint64_t initialIdleNs = 12000;
    constexpr unsigned maxRequests = 64;
    const auto deadline = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + maxWindowNs;
    for (unsigned i = 0; i < maxRequests; ++i) {
        const auto ticket = openpci_mmio_ticket(request);
        const auto busy = ticket | OPENPCI_MMIO_BUSY;
        if (!openpci_mmio_change(window, request, busy)) break;

        uint64_t fields[] = {
            __atomic_load_n(&window->index, __ATOMIC_RELAXED),
            __atomic_load_n(&window->offset, __ATOMIC_RELAXED),
            __atomic_load_n(&window->width, __ATOMIC_RELAXED),
            __atomic_load_n(&window->input, __ATOMIC_RELAXED),
        };
        uint64_t result = 0;
        const auto operation = __atomic_load_n(&window->write, __ATOMIC_RELAXED);
        IOUserClientMethodArguments mmio{};
        mmio.scalarInput = fields;
        kern_return_t status = kIOReturnBadArgument;
        if (operation == OPENPCI_MMIO_BAR_WORD_WRITE) {
            const auto count = fields[2];
            if (count && count <= EXTGPU_BAR_WORDS_MAX) {
                uint64_t batch[EXTGPU_BAR_WORDS_HEADER + EXTGPU_BAR_WORDS_MAX] = {
                    fields[0], fields[1], EXTGPU_BAR_WORDS_WRITE, count,
                };
                for (uint64_t word = 0; word < count; ++word)
                    batch[EXTGPU_BAR_WORDS_HEADER + word] = __atomic_load_n(&window->words[word], __ATOMIC_RELAXED);
                mmio.scalarInput = batch;
                mmio.scalarInputCount = static_cast<uint32_t>(EXTGPU_BAR_WORDS_HEADER + count);
                status = vfio_user_bar_words(client, driver, &mmio);
            }
        } else if (operation <= OPENPCI_MMIO_SCALAR_WRITE) {
            const bool read = operation == OPENPCI_MMIO_SCALAR_READ;
            mmio.scalarInputCount = read ? 3 : 4;
            mmio.scalarOutput = read ? &result : nullptr;
            mmio.scalarOutputCount = read ? 1 : 0;
            status = dispatch_scalar_mmio(client, driver, &mmio, read);
        }
        __atomic_store_n(&window->output, result, __ATOMIC_RELAXED);
        __atomic_store_n(&window->status, static_cast<uint32_t>(status), __ATOMIC_RELAXED);
        if (!openpci_mmio_change(window, busy, ticket | OPENPCI_MMIO_DONE)) break;
        if (status != kIOReturnSuccess) {
            session.mmioMemoryEnable = {};
            if (operation == OPENPCI_MMIO_BAR_WORD_WRITE) break;
        }

        // Two completed requests demonstrate a burst. Allow the next vCPU
        // exit more time to reach the same window, subject to its existing
        // absolute deadline and generation checks.
        const uint64_t idleNs = i > 0 && status == kIOReturnSuccess ? 2 * initialIdleNs : initialIdleNs;
        const auto idleDeadline = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + idleNs;
        request = (ticket + 8) | OPENPCI_MMIO_REQUEST;
        for (;;) {
            const auto next = openpci_mmio_state(window);
            if (next == request) break;
            const auto now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            if (now >= deadline || now >= idleDeadline || openpci_mmio_ticket(next) != ticket) {
                request = 0;
                break;
            }
            openpci_mmio_pause();
        }
        if (!request || clock_gettime_nsec_np(CLOCK_UPTIME_RAW) >= deadline) break;
    }

    // Closing can race the SDK consuming the last reply and issuing one more
    // request. A canceled request never performed I/O and can use a scalar RPC.
    for (unsigned retry = 0; retry < 4; ++retry) {
        const auto state = openpci_mmio_state(window);
        unsigned closed;
        switch (openpci_mmio_phase(state)) {
        case OPENPCI_MMIO_DONE: closed = OPENPCI_MMIO_DONE_OFFLINE; break;
        case OPENPCI_MMIO_READY: closed = OPENPCI_MMIO_OFFLINE; break;
        case OPENPCI_MMIO_REQUEST: closed = OPENPCI_MMIO_ABORTED; break;
        default: return kIOReturnSuccess;
        }
        if (openpci_mmio_change(window, state, openpci_mmio_ticket(state) | closed)) return kIOReturnSuccess;
    }
    return kIOReturnNotReady;
}

static kern_return_t vfio_user_bar_words(VFIOUserPCIDriverUserClient *client,
                                        VFIOUserPCIDriver *driver, IOUserClientMethodArguments *args)
{
    if (!args->scalarInput || args->scalarInputCount < EXTGPU_BAR_WORDS_HEADER)
        return kIOReturnBadArgument;
    const uint64_t *in = args->scalarInput;
    const uint64_t index = in[0], offset = in[1], op = in[2], count = in[3];
    if (op > EXTGPU_BAR_WORDS_FILL || !count ||
        count > (op == EXTGPU_BAR_WORDS_FILL ? EXTGPU_BAR_WORDS_MAX_FILL : EXTGPU_BAR_WORDS_MAX))
        return kIOReturnBadArgument;
    const uint64_t payload = op == EXTGPU_BAR_WORDS_WRITE ? count : op == EXTGPU_BAR_WORDS_FILL ? 1 : 0;
    const uint64_t output = op == EXTGPU_BAR_WORDS_READ ? count : 0;
    if (args->scalarInputCount != EXTGPU_BAR_WORDS_HEADER + payload ||
        args->scalarOutputCount != output || (output && !args->scalarOutput))
        return kIOReturnBadArgument;

    auto *pci = vfio_user_get_pci_device(driver);
    if (!pci) return kIOReturnNotAttached;
    ReadyProviderLock lock(&driver->ivars->stateLock);
    const auto bounds = ready_validate_mmio(driver, client, pci, index, offset, 8);
    if (bounds != kIOReturnSuccess) return bounds;
    if (!extgpu_driver::prefetchable_bar_contains_words(driver->ivars->mmioBoundsCache,
                                                        index, offset, count))
        return kIOReturnUnsupported;
    auto &memoryEnable = client->ivars->mmioMemoryEnable;
    const auto now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (op != EXTGPU_BAR_WORDS_WRITE || !memoryEnable.valid_at(now)) {
        uint16_t command = UINT16_MAX;
        pci->ConfigurationRead16(4, &command);
        if (!memoryEnable.record(now, command)) return kIOReturnNotReady;
    }

    client->ivars->claimPristine = false;
    client->ivars->hardwareMutated = true;
    for (uint64_t i = 0; i < count; ++i) {
        const uint64_t address = offset + 8u * i;
        if (op == EXTGPU_BAR_WORDS_READ) {
            uint64_t value = UINT64_MAX;
            pci->MemoryRead64(static_cast<uint8_t>(index), address, &value);
            args->scalarOutput[i] = value;
        } else {
            const uint64_t value = in[EXTGPU_BAR_WORDS_HEADER + (op == EXTGPU_BAR_WORDS_FILL ? 0 : i)];
            pci->MemoryWrite64(static_cast<uint8_t>(index), address, value);
        }
    }
    return kIOReturnSuccess;
}

static void release_client_state(VFIOUserPCIDriverUserClient_IVars *state)
{
    if (__atomic_sub_fetch(&state->stateReferences, 1u, __ATOMIC_ACQ_REL)) return;
    IOSafeDeleteNULL(state->clientDMARegions, ClientDMARegistration, EXTGPU_MAX_DMA_REGIONS);
    IOLockFreeZero(state->operationLock.value);
    IOLockFreeZero(state->irqLock.value);
    IOSafeDeleteNULL(state, VFIOUserPCIDriverUserClient_IVars, 1);
}

static kern_return_t dispatch_grouped_stores(VFIOUserPCIDriverUserClient *client,
                                            VFIOUserPCIDriver *driver, openpci_mmio_ring *ring,
                                            uint64_t begin, uint64_t count, uint64_t index,
                                            uint64_t offset, uint64_t width)
{
    if (count < 2 || count > EXTGPU_MMIO_GROUPED_STORES_MAX ||
        !extgpu_driver::valid_mmio_request(index, offset, width) || offset > UINT64_MAX - count * width)
        return kIOReturnBadArgument;
    uint64_t values[EXTGPU_MMIO_GROUPED_STORES_MAX];
    for (uint64_t i = 0; i < count; ++i) {
        const auto *slot = openpci_mmio_ring_slot_at(ring, begin + i);
        if (__atomic_load_n(&slot->operation, __ATOMIC_RELAXED) != OPENPCI_RING_WRITE_GROUPED ||
            __atomic_load_n(&slot->index, __ATOMIC_RELAXED) != index ||
            __atomic_load_n(&slot->offset, __ATOMIC_RELAXED) != offset + i * width ||
            __atomic_load_n(&slot->width, __ATOMIC_RELAXED) != width)
            return kIOReturnBadArgument;
        values[i] = __atomic_load_n(&slot->input, __ATOMIC_RELAXED);
        if (width < 8 && values[i] >= (UINT64_C(1) << (width * 8))) return kIOReturnBadArgument;
    }
    auto &provider = *driver->ivars;
    ReadyProviderLock lock(&provider.stateLock);
    auto *pci = vfio_user_get_pci_device(driver);
    if (!pci) return kIOReturnNotAttached;
    const auto bounds = ready_validate_mmio(driver, client, pci, index, offset, width);
    if (bounds != kIOReturnSuccess) return bounds;
    if (!extgpu_driver::prefetchable_bar_contains_units(provider.mmioBoundsCache,
                                                        index, offset, width, count))
        return kIOReturnUnsupported;

    // These are the same typed writes and owner checks as the scalar path.
    // Keeping one provider lock across the checked group also excludes Stop.
    auto &session = *client->ivars;
    session.startupDMARequest = {};
    session.claimPristine = false;
    session.hardwareMutated = true;
    for (uint64_t i = 0; i < count; ++i)
        write_mmio_value(pci, static_cast<uint8_t>(index), offset + i * width, width, values[i]);
    return kIOReturnSuccess;
}

/* operationLock serializes the consumer cursor with the kick and Stop.
 * The SDK publishes a complete instruction or a single scalar access. */
static kern_return_t drain_mmio_ring(VFIOUserPCIDriverUserClient *client,
                                     VFIOUserPCIDriver *driver, uint64_t end)
{
    auto &state = *client->ivars;
    auto *ring = state.mmioRing;
    if (!ring || !state.protocolNegotiated || !state.claimed ||
        state.mmioRingGeneration != state.negotiatedGeneration ||
        state.coldPrepared || state.coldFrozen ||
        state.warmVMRelease.phase != extgpu_driver::WarmVMRelease::idle)
        return kIOReturnNotPermitted;
    const uint64_t begin = state.mmioRingConsumed;
    const uint64_t count = end - begin;
    if (end <= begin || count > EXTGPU_BAR_WORDS_MAX) return kIOReturnBadArgument;

    const auto *first = openpci_mmio_ring_slot_at(ring, begin);
    const auto operation = __atomic_load_n(&first->operation, __ATOMIC_RELAXED);
    const auto index = __atomic_load_n(&first->index, __ATOMIC_RELAXED);
    const auto offset = __atomic_load_n(&first->offset, __ATOMIC_RELAXED);
    const auto width = __atomic_load_n(&first->width, __ATOMIC_RELAXED);
    uint64_t output[EXTGPU_BAR_WORDS_MAX]{};
    IOUserClientMethodArguments mmio{};
    kern_return_t result = kIOReturnBadArgument;
    if (operation == OPENPCI_RING_WRITE_GROUPED) {
        result = dispatch_grouped_stores(client, driver, ring, begin, count, index, offset, width);
    } else if (operation <= OPENPCI_RING_WRITE && count == 1) {
        const bool read = operation == OPENPCI_RING_READ;
        uint64_t input[] = {index, offset, width, __atomic_load_n(&first->input, __ATOMIC_RELAXED)};
        mmio.scalarInput = input;
        mmio.scalarInputCount = read ? 3 : 4;
        mmio.scalarOutput = read ? output : nullptr;
        mmio.scalarOutputCount = read ? 1 : 0;
        result = dispatch_scalar_mmio(client, driver, &mmio, read);
    } else if ((operation == OPENPCI_RING_READ_WORDS || operation == OPENPCI_RING_WRITE_WORDS) &&
               width == 8 && offset <= UINT64_MAX - count * 8) {
        const bool read = operation == OPENPCI_RING_READ_WORDS;
        uint64_t input[EXTGPU_BAR_WORDS_HEADER + EXTGPU_BAR_WORDS_MAX] = {
            index, offset, read ? EXTGPU_BAR_WORDS_READ : EXTGPU_BAR_WORDS_WRITE, count,
        };
        for (uint64_t i = 0; i < count; ++i) {
            const auto *slot = openpci_mmio_ring_slot_at(ring, begin + i);
            if (__atomic_load_n(&slot->operation, __ATOMIC_RELAXED) != operation ||
                __atomic_load_n(&slot->index, __ATOMIC_RELAXED) != index ||
                __atomic_load_n(&slot->width, __ATOMIC_RELAXED) != 8 ||
                __atomic_load_n(&slot->offset, __ATOMIC_RELAXED) != offset + 8 * i)
                goto complete;
            input[EXTGPU_BAR_WORDS_HEADER + i] = __atomic_load_n(&slot->input, __ATOMIC_RELAXED);
        }
        mmio.scalarInput = input;
        mmio.scalarInputCount = static_cast<uint32_t>(EXTGPU_BAR_WORDS_HEADER + (read ? 0 : count));
        mmio.scalarOutput = read ? output : nullptr;
        mmio.scalarOutputCount = read ? static_cast<uint32_t>(count) : 0;
        result = vfio_user_bar_words(client, driver, &mmio);
    }
complete:
    for (uint32_t i = 0; i < mmio.scalarOutputCount; ++i)
        __atomic_store_n(&openpci_mmio_ring_slot_at(ring, begin + i)->output, output[i], __ATOMIC_RELAXED);
    if (result != kIOReturnSuccess) state.mmioMemoryEnable = {};
    state.mmioRingConsumed = end;
    __atomic_store_n(&ring->status, static_cast<uint32_t>(result), __ATOMIC_RELAXED);
    __atomic_store_n(&ring->completed, end, __ATOMIC_RELEASE);
    return kIOReturnSuccess;
}

static void schedule_mmio_ring(VFIOUserPCIDriverUserClient *client, VFIOUserPCIDriver *driver,
                               VFIOUserPCIDriverUserClient_IVars *state, openpci_mmio_ring *ring,
                               uint64_t idleNs, bool adaptive, bool irqArmed);

static void run_mmio_ring(VFIOUserPCIDriverUserClient *client, VFIOUserPCIDriver *driver,
                          VFIOUserPCIDriverUserClient_IVars *state, openpci_mmio_ring *ring,
                          uint64_t idleNs, bool adaptive, bool irqArmed)
{
    auto now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    const auto deadline = now + (idleNs ? 2000000 : OPENPCI_MMIO_WINDOW_SERVICE_NS);
    const unsigned maxRequests = idleNs ? 1024 : 64;
    // A longer grace period requires evidence of another request. Keep
    // the setup cost of an isolated command bounded even with long bursts.
    const uint64_t firstIdle = idleNs / 6 > 12000 ? idleNs / 6 : 12000;
    uint64_t idleDeadline = now + (irqArmed ? OPENPCI_MMIO_RING_IRQ_FIRST_NS : firstIdle);
    uint64_t lastCompletion = now;
    uint64_t consumed = __atomic_load_n(&ring->completed, __ATOMIC_ACQUIRE);
    for (unsigned requests = 0; __atomic_load_n(&ring->active, __ATOMIC_ACQUIRE) != OPENPCI_RING_RETIRED;) {
        const auto end = __atomic_load_n(&ring->published, __ATOMIC_ACQUIRE);
        if (end == consumed && now < idleDeadline && now < deadline) {
            openpci_mmio_pause();
            now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            continue;
        }
        ReadyProviderLock operation(&state->operationLock);
        if (state->mmioRing != ring ||
            __atomic_load_n(&ring->active, __ATOMIC_ACQUIRE) == OPENPCI_RING_RETIRED)
            break;
        consumed = state->mmioRingConsumed;
        const auto next = __atomic_load_n(&ring->published, __ATOMIC_ACQUIRE);
        if (next != consumed) {
            const uint64_t gap = now - lastCompletion;
            const auto status = drain_mmio_ring(client, driver, next);
            if (status != kIOReturnSuccess) {
                __atomic_store_n(&ring->active, OPENPCI_RING_RETIRED, __ATOMIC_RELEASE);
                break;
            }
            if (irqArmed && requests == 0) __atomic_fetch_add(&ring->irqHits, 1, __ATOMIC_RELAXED);
            consumed = next;
            now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            lastCompletion = now;
            idleDeadline = now + (adaptive ? openpci_mmio_ring_adaptive_idle(idleNs, gap)
                                           : (idleNs ? idleNs : 24000));
            if (++requests < maxRequests && now < deadline) continue;
        } else if (now < deadline && state->mmioRingIrqDeadlineNs > idleDeadline &&
                   now < state->mmioRingIrqDeadlineNs) {
            // A notification came through while this consumer was warm.
            // Reuse it within the same absolute service budget.
            idleDeadline = state->mmioRingIrqDeadlineNs;
            __atomic_fetch_add(&ring->irqExtensions, 1, __ATOMIC_RELAXED);
            continue;
        }
        if (openpci_mmio_ring_disarm(ring, consumed)) state->mmioRingScheduled = false;
        else schedule_mmio_ring(client, driver, state, ring, idleNs, adaptive, false);
        return;
    }
    ReadyProviderLock operation(&state->operationLock);
    state->mmioRingScheduled = false;
}

static void schedule_mmio_ring(VFIOUserPCIDriverUserClient *client, VFIOUserPCIDriver *driver,
                               VFIOUserPCIDriverUserClient_IVars *state, openpci_mmio_ring *ring,
                               uint64_t idleNs, bool adaptive, bool irqArmed)
{
    state->mmioRingScheduled = true;
    auto *memory = state->mmioRingMemory;
    __atomic_fetch_add(&state->stateReferences, 1u, __ATOMIC_RELAXED);
    client->retain();
    driver->retain();
    memory->retain();
    state->mmioRingQueue->DispatchAsync(^{
        run_mmio_ring(client, driver, state, ring, idleNs, adaptive, irqArmed);
        memory->release();
        driver->release();
        client->release();
        release_client_state(state);
    });
}

static kern_return_t kick_mmio_ring(VFIOUserPCIDriverUserClient *client,
                                    VFIOUserPCIDriver *driver, IOUserClientMethodArguments *args)
{
    auto *state = client->ivars;
    auto *ring = state->mmioRing;
    if (!ring || !args->scalarInput || args->scalarInputCount < 2 || args->scalarInputCount > 4 ||
        args->scalarOutputCount ||
        args->scalarInput[0] != state->mmioRingGeneration ||
        args->scalarInput[1] != __atomic_load_n(&ring->published, __ATOMIC_ACQUIRE))
        return kIOReturnBadArgument;
    const uint64_t idleUs = args->scalarInputCount >= 3 ? args->scalarInput[2] : 0;
    const uint64_t flags = args->scalarInputCount == 4 ? args->scalarInput[3] : 0;
    if (flags == OPENPCI_MMIO_RING_EVENT_FLAG) {
        if (idleUs || state->mmioRingScheduled ||
            __atomic_load_n(&ring->active, __ATOMIC_ACQUIRE) != OPENPCI_RING_ACTIVE)
            return kIOReturnBadArgument;
        state->mmioRingIdleNs = state->mmioRingIrqDeadlineNs = 0;
        __atomic_store_n(&state->mmioRingFlags, flags, __ATOMIC_RELEASE);
        const auto result = drain_mmio_ring(client, driver, args->scalarInput[1]);
        // The SDK holds its producer lock until this synchronous notification
        // returns. No producer can publish another batch during disarm.
        __atomic_store_n(&ring->active, result == kIOReturnSuccess ? OPENPCI_RING_IDLE : OPENPCI_RING_RETIRED,
                         __ATOMIC_RELEASE);
        return result;
    }
    if (flags & ~(OPENPCI_MMIO_RING_ADAPTIVE_FLAG | OPENPCI_MMIO_RING_IRQ_ARM_FLAG))
        return kIOReturnBadArgument;
    if (args->scalarInputCount >= 3 &&
        (idleUs < OPENPCI_MMIO_RING_POLL_MIN_US || idleUs > OPENPCI_MMIO_RING_POLL_MAX_US))
        return kIOReturnBadArgument;
    // The first operation runs on the IOKit caller's path. The serial DEXT
    // queue can then consume later arrivals while the VMM runs.
    // The previous consumer can complete a published batch just before the
    // producer's active exchange. In that case a kick only arms the queue.
    const auto result = args->scalarInput[1] == state->mmioRingConsumed
        ? kIOReturnSuccess : drain_mmio_ring(client, driver, args->scalarInput[1]);
    if (result == kIOReturnSuccess) {
        state->mmioRingIdleNs = idleUs * 1000;
        if (!(flags & OPENPCI_MMIO_RING_IRQ_ARM_FLAG)) state->mmioRingIrqDeadlineNs = 0;
        __atomic_store_n(&state->mmioRingFlags, flags, __ATOMIC_RELEASE);
        if (!state->mmioRingScheduled)
            schedule_mmio_ring(client, driver, state, ring, state->mmioRingIdleNs,
                               flags & OPENPCI_MMIO_RING_ADAPTIVE_FLAG, false);
    }
    return result;
}

static void arm_mmio_for_shared_irq(VFIOUserPCIDriverUserClient *client,
                                   VFIOUserPCIDriverUserClient_IVars *state)
{
    if (!(__atomic_load_n(&state->mmioRingFlags, __ATOMIC_ACQUIRE) & OPENPCI_MMIO_RING_IRQ_ARM_FLAG))
        return;
    // The caller holds irqLock. Ordinary selectors take operationLock first.
    // An unsuccessful try leaves IRQ delivery on its current path.
    auto *lock = state->operationLock.value;
    if (!IOLockTryLock(lock)) return;
    const uint64_t flags = __atomic_load_n(&state->mmioRingFlags, __ATOMIC_RELAXED);
    auto *ring = state->mmioRing;
    if (ring && (flags & OPENPCI_MMIO_RING_IRQ_ARM_FLAG) && state->mmioRingIdleNs &&
        state->claimed && state->protocolNegotiated &&
        state->mmioRingGeneration == state->negotiatedGeneration &&
        !state->coldPrepared && !state->coldFrozen &&
        state->warmVMRelease.phase == extgpu_driver::WarmVMRelease::idle) {
        state->mmioRingIrqDeadlineNs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + OPENPCI_MMIO_RING_IRQ_FIRST_NS;
        if (!state->mmioRingScheduled &&
            __atomic_load_n(&ring->active, __ATOMIC_ACQUIRE) == OPENPCI_RING_IDLE) {
            auto *driver = OSDynamicCast(VFIOUserPCIDriver, client->GetProvider());
            if (driver && openpci_mmio_ring_arm_idle(ring)) {
                __atomic_fetch_add(&ring->irqArms, 1, __ATOMIC_RELAXED);
                schedule_mmio_ring(client, driver, state, ring, state->mmioRingIdleNs,
                                   flags & OPENPCI_MMIO_RING_ADAPTIVE_FLAG, true);
            }
        }
    }
    IOLockUnlock(lock);
}

kern_return_t IMPL(VFIOUserPCIDriver, Start)
{
    auto *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) return kIOReturnUnsupported;
    auto status = Start(provider, SUPERDISPATCH);
    if (status != kIOReturnSuccess) return status;

    VFIOUserPCIDeviceIdentity identity{};
    status = vfio_user_read_identity(this, &identity);
    if (status != kIOReturnSuccess) return status;
    const bool companion = ready_is_reset_companion(pci, identity);
    if (!companion) {
        status = ready_validate_external_gpu(pci);
        if (status != kIOReturnSuccess) return status;
    }

    ivars = IONewZero(VFIOUserPCIDriver_IVars, 1);
    if (!ivars) return kIOReturnNoMemory;
    ivars->stateLock.value = IOLockAlloc();
    const auto publish = [&]() -> kern_return_t {
        if (!ivars->stateLock.value) return kIOReturnNoMemory;
        auto result = GetRegistryEntryID(&ivars->generation);
        if (result != kIOReturnSuccess) return result;
        if (!ivars->generation) return kIOReturnError;

        if (companion) {
            uint64_t registryID = 0;
            result = pci->GetRegistryEntryID(&registryID);
            if (result != kIOReturnSuccess) return result;
            result = register_reset_companion(this, registryID);
            if (result != kIOReturnSuccess) return result;
            ivars->resetCompanion = true;
        }
        return RegisterService();
    };
    status = publish();
    if (status != kIOReturnSuccess) {
        if (ivars->resetCompanion) unregister_reset_companion(this);
        IOLockFreeZero(ivars->stateLock.value);
        IOSafeDeleteNULL(ivars, VFIOUserPCIDriver_IVars, 1);
    }
    return status;
}

kern_return_t IMPL(VFIOUserPCIDriver, NewUserClient)
{
    if (!connection) return kIOReturnBadArgument;
    *connection = nullptr;
    if (type || !ivars || ivars->resetCompanion) return kIOReturnUnsupported;

    IOService *service = nullptr;
    const auto status = Create(this, "OpenPCIUserClientProperties", &service);
    auto *session = status == kIOReturnSuccess ? OSDynamicCast(IOUserClient, service) : nullptr;
    if (!session) {
        OSSafeReleaseNULL(service);
        return status == kIOReturnSuccess ? kIOReturnUnsupported : status;
    }
    *connection = session;
    return kIOReturnSuccess;
}

kern_return_t
IMPL(VFIOUserPCIDriver, Stop)
{
    if (ivars && ivars->resetCompanion) unregister_reset_companion(this);
    if (ivars != nullptr) {
        ReadyProviderLock stateLock(&ivars->stateLock);
        ivars->stopping = true;
        if (ivars->providerClaimed || ivars->quarantinedClientState != nullptr) {
            ivars->quarantined = true;
            os_log(OS_LOG_DEFAULT, "extgpu: provider Stop while owner/resources exist; state retained");
        }
        ready_clear_bar_cache_locked(this);
        /* Do not delete state here: a userclient Stop or in-flight RPC can still
         * reference it. DriverKit process/removal behavior requires real tests.
         */
    }
    return Stop(provider, SUPERDISPATCH);
}

kern_return_t IMPL(VFIOUserPCIDriverUserClient, Start)
{
    if (!OSDynamicCast(VFIOUserPCIDriver, provider)) return kIOReturnUnsupported;

    const auto status = Start(provider, SUPERDISPATCH);
    if (status != kIOReturnSuccess) return status;
    ivars = IONewZero(VFIOUserPCIDriverUserClient_IVars, 1);
    if (!ivars) return kIOReturnNoMemory;

    ivars->stateReferences = 1;
    ivars->dmaAddressBits = 32;
    ivars->dmaByteBudget = EXTGPU_DEFAULT_DMA_BUDGET_BYTES;
    ivars->operationLock.value = IOLockAlloc();
    ivars->irqLock.value = IOLockAlloc();
    if (ivars->operationLock.value && ivars->irqLock.value) return kIOReturnSuccess;

    IOLockFreeZero(ivars->irqLock.value);
    IOLockFreeZero(ivars->operationLock.value);
    IOSafeDeleteNULL(ivars, VFIOUserPCIDriverUserClient_IVars, 1);
    return kIOReturnNoMemory;
}

static void vfio_user_release_all_interrupts(VFIOUserPCIDriverUserClient *);

kern_return_t
IMPL(VFIOUserPCIDriverUserClient, Stop)
{
    auto *state = ivars;
    if (state == nullptr) return Stop(provider, SUPERDISPATCH);
    bool preserve = false;
    bool frozenStop = false;
    {
        ReadyProviderLock operation(&state->operationLock);
        state->mmioWindow = nullptr;
        OSSafeReleaseNULL(state->mmioWindowMemory);
        if (state->mmioRing)
            __atomic_store_n(&state->mmioRing->active, OPENPCI_RING_RETIRED, __ATOMIC_RELEASE);
        state->mmioRing = nullptr;
        OSSafeReleaseNULL(state->mmioRingMemory);
        OSSafeReleaseNULL(state->mmioRingQueue);
        if (state->irqMailboxMemory) {
            ReadyProviderLock irq(&state->irqLock);
            if (state->irqPersistent) OSSafeReleaseNULL(state->pendingInterruptNotify);
            state->irqPersistent = false;
            state->irqMailbox = nullptr;
            OSSafeReleaseNULL(state->irqMailboxMemory);
        }
        if (state->coldPrepared || state->coldFrozen) {
            /* Stop/deactivation is not physical-off evidence. Keep the old
             * endpoint's recovery state/lock reachable, with no automatic
             * CompleteDMA, IRQ operation, PCI access or reference release. */
            state->coldStopped = true;
            state->coldLedger.note_provider_stop();
            auto *driver = state->coldDriver;
            if (driver != nullptr && driver->ivars != nullptr) {
                ReadyProviderLock device(&driver->ivars->stateLock);
                driver->ivars->quarantined = true;
                driver->ivars->quarantinedClientState = state;
            }
            frozenStop = true;
        } else {
            preserve = state->claimed && !state->ownerReleased;
            if (preserve) {
                // DriverKit can invoke Stop after the PCI provider's access
                // state has already been torn down. Retain the unacknowledged
                // owner without issuing Close, config access or completion.
                auto *driver = OSDynamicCast(VFIOUserPCIDriver, provider);
                if (driver && driver->ivars) {
                    ReadyProviderLock device(&driver->ivars->stateLock);
                    driver->ivars->quarantined = true;
                    driver->ivars->quarantinedClientState = state;
                    ready_clear_bar_cache_locked(driver);
                    if (driver->ivars->openerClient == this) {
                        driver->ivars->providerClaimed = false;
                        driver->ivars->providerClaimRefs = 0;
                        driver->ivars->openerClient = nullptr;
                    }
                    driver->retain();
                }
                os_log(OS_LOG_DEFAULT, "extgpu: Stop retained unacknowledged owner without PCI access");
            } else {
                vfio_user_release_all_interrupts(this);
            }
            if (!preserve && __atomic_load_n(&state->irqCancelPending, __ATOMIC_ACQUIRE) != 0) {
                /* Cancellation still references state. Keep it rather than risk UAF. */
                preserve = true;
            }
            ivars = nullptr;
        }
    }
    if (frozenStop) return Stop(provider, SUPERDISPATCH);
    if (!preserve) release_client_state(state);
    return Stop(provider, SUPERDISPATCH);
}

/* irqLock is held and every created source has either drained or never existed. */
static void ready_clear_irq_resources_locked(VFIOUserPCIDriverUserClient_IVars *state)
{
    OSAction *pending = __atomic_exchange_n(&state->pendingInterruptNotify, nullptr, __ATOMIC_ACQ_REL);
    if (pending != nullptr) pending->release();
    state->irqPersistent = false;
    state->irqPending = nullptr;
    state->irqEnabled = nullptr;
    /* Legacy clients may hold this exported descriptor for the connection's
     * lifetime. Keep its identity stable and refuse retirement/recreation.
     */
    if (!state->irqStateExported) OSSafeReleaseNULL(state->irqSharedBuffer);
    OSSafeReleaseNULL(state->irqQueue);
}

/* Called exactly once only after a successful Cancel return AND completion.
 * irqCancelPending stays positive until the last state access and lock release,
 * so Stop cannot delete this state while a completion is still using it.
 */
static void ready_retire_irq_source(VFIOUserPCIDriverUserClient *client,
                                    VFIOUserPCIDriverUserClient_IVars *state,
                                    uint32_t index)
{
    IOInterruptDispatchSource *source;
    {
        ReadyProviderLock irqLock(&state->irqLock);
        source = state->interruptSources[index];
        state->interruptSources[index] = nullptr;
    }
    if (source != nullptr) source->release();
    {
        ReadyProviderLock irqLock(&state->irqLock);
        if (state->irqCancelRemaining != 0) --state->irqCancelRemaining;
        if (state->irqCancelRemaining == 0 && state->irqCancelFailure == kIOReturnSuccess) {
            ready_clear_irq_resources_locked(state);
        }
    }
    __atomic_sub_fetch(&state->irqCancelPending, 1u, __ATOMIC_ACQ_REL);
    client->release();
}

static void
vfio_user_release_all_interrupts(VFIOUserPCIDriverUserClient *client)
{
    if (client == nullptr || client->ivars == nullptr) return;
    auto *state = client->ivars;
    IOInterruptDispatchSource *toCancel[maxInterruptVectors] = {};
    {
        ReadyProviderLock irqLock(&state->irqLock);
        state->interruptsSetUp = false;
        state->numInterrupts = 0;
        OSAction *pending = __atomic_exchange_n(&state->pendingInterruptNotify, nullptr, __ATOMIC_ACQ_REL);
        if (pending != nullptr) pending->release();
        state->irqPersistent = false;
        /* Stage every source before calling Cancel: completion may run inline.
         * Failed calls keep their original source in this reachable slot and
         * their pending client/state reference. Never retry an ambiguous Cancel.
         */
        for (uint32_t i = 0; i < maxInterruptVectors; ++i) {
            auto *source = state->interruptSources[i];
            if (source == nullptr || !extgpu_driver::begin_irq_cancel(state->irqCancellations[i])) continue;
            toCancel[i] = source;
            ++state->irqCancelRemaining;
            __atomic_add_fetch(&state->irqCancelPending, 1u, __ATOMIC_ACQ_REL);
            client->retain();
        }
        if (state->irqCancelRemaining == 0 && state->irqCancelFailure == kIOReturnSuccess) {
            ready_clear_irq_resources_locked(state);
        }
    }
    for (uint32_t i = 0; i < maxInterruptVectors; ++i) {
        IOInterruptDispatchSource *source = toCancel[i];
        if (source == nullptr) continue;
        kern_return_t cancel = source->Cancel(^{
            /* A racing successful return may retire the primary reference
             * while this completion is still publishing its event. Keep state
             * live until the completion's final access, including the loser.
             */
            __atomic_add_fetch(&state->irqCancelPending, 1u, __ATOMIC_ACQ_REL);
            if (extgpu_driver::record_irq_cancel_completion(state->irqCancellations[i])) {
                ready_retire_irq_source(client, state, i);
            }
            __atomic_sub_fetch(&state->irqCancelPending, 1u, __ATOMIC_ACQ_REL);
        });
        if (cancel != kIOReturnSuccess) {
            {
                ReadyProviderLock irqLock(&state->irqLock);
                state->irqCancelFailure = cancel;
            }
            os_log(OS_LOG_DEFAULT, "extgpu: IRQ cancellation failed; source and owner retained for recovery: %#x", cancel);
        }
        if (extgpu_driver::record_irq_cancel_return(state->irqCancellations[i], cancel == kIOReturnSuccess)) {
            ready_retire_irq_source(client, state, i);
        }
    }
}

static kern_return_t ready_retire_interrupts(VFIOUserPCIDriverUserClient *client)
{
    vfio_user_release_all_interrupts(client);
    auto *state = client->ivars;
    ReadyProviderLock irqLock(&state->irqLock);
    if (state->irqCancelFailure != kIOReturnSuccess) return state->irqCancelFailure;
    if (__atomic_load_n(&state->irqCancelPending, __ATOMIC_ACQUIRE) != 0 ||
        state->irqCancelRemaining != 0 || state->interruptsSetUp ||
        state->irqQueue != nullptr) return kIOReturnNotReady;
    if (state->irqStateExported) return kIOReturnUnsupported;
    if (state->irqSharedBuffer != nullptr) return kIOReturnNotReady;
    for (uint32_t i = 0; i < maxInterruptVectors; ++i) {
        if (state->interruptSources[i] != nullptr) return kIOReturnNotReady;
    }
    return kIOReturnSuccess;
}

static uint32_t requested_interrupt_count(IOPCIDevice *pci, bool *msix)
{
    uint64_t offset = 0;
    uint16_t control = 0;
    *msix = pci->FindPCICapability(kIOPCICapabilityIDMSIX, 0, &offset) == kIOReturnSuccess;
    if (*msix) {
        pci->ConfigurationRead16(offset + 2, &control);
        const unsigned advertised = (control & 0x7ffu) + 1;
        return advertised > maxInterruptVectors ? maxInterruptVectors : advertised;
    }
    if (pci->FindPCICapability(kIOPCICapabilityIDMSI, 0, &offset) != kIOReturnSuccess)
        return 1;
    pci->ConfigurationRead16(offset + 2, &control);
    return 1u << ((control >> 1) & 7);
}

static kern_return_t ready_owner_status(VFIOUserPCIDriverUserClient *client)
{
    if (client == nullptr || client->ivars == nullptr || !client->ivars->protocolNegotiated) return kIOReturnNotPermitted;
    auto *driver = OSDynamicCast(VFIOUserPCIDriver, client->GetProvider());
    if (driver == nullptr || driver->ivars == nullptr) return kIOReturnNotAttached;
    ReadyProviderLock lock(&driver->ivars->stateLock);
    return ready_owner_status_locked(client, driver, client->ivars->negotiatedGeneration);
}

static kern_return_t ready_cold_disable_bus_master(IOPCIDevice *pci)
{
    if (!pci) return kIOReturnNotAttached;
    uint16_t command = UINT16_MAX;
    pci->ConfigurationRead16(0x04, &command);
    /* Public ConfigurationRead16 writes -1 on error. Do not use that value
     * as an RMW operand, and never mint an ACK from an error readback. */
    if (command == UINT16_MAX) return kIOReturnNotAttached;
    if (!(command & 0x04)) return kIOReturnSuccess;
    pci->ConfigurationWrite16(0x04, command & ~UINT16_C(0x04));
    command = UINT16_MAX;
    pci->ConfigurationRead16(0x04, &command);
    if (command == UINT16_MAX) return kIOReturnNotAttached;
    return (command & 0x04) ? kIOReturnError : kIOReturnSuccess;
}

static extgpu_driver::ColdRemovalKey ready_cold_key(const uint64_t *in)
{
    return {in[0], in[1], in[2], {in[3], in[4]}};
}

static void ready_cold_write_key(uint64_t *out,
                                  const extgpu_driver::ColdRemovalKey &key)
{
    out[0] = key.generation;
    out[1] = key.serviceID;
    out[2] = key.pciID;
    out[3] = key.nonce[0];
    out[4] = key.nonce[1];
}

static kern_return_t ready_cold_result(extgpu_driver::ColdRemovalResult result,
                                       int32_t native = 0)
{
    using Result = extgpu_driver::ColdRemovalResult;
    switch (result) {
    case Result::success: return kIOReturnSuccess;
    case Result::bad_argument: return kIOReturnBadArgument;
    case Result::stale_key: return kIOReturnNotPermitted;
    case Result::capacity: return kIOReturnNoSpace;
    case Result::busy: return kIOReturnBusy;
    case Result::not_found: return kIOReturnNotFound;
    case Result::completion_unknown: return native ? native : kIOReturnError;
    case Result::closed: return kIOReturnNotPermitted;
    case Result::unavailable: return kIOReturnNotReady;
    }
    return kIOReturnError;
}

/* Metadata only. Caller holds operationLock; never starts IRQ cancellation. */
static bool ready_cold_irq_empty(VFIOUserPCIDriverUserClient_IVars *state)
{
    ReadyProviderLock irq(&state->irqLock);
    if (state->irqStateExported || state->interruptsSetUp || state->numInterrupts ||
        state->irqQueue || state->irqSharedBuffer || state->irqPending ||
        state->irqEnabled || state->pendingInterruptNotify ||
        state->irqCancelRemaining || state->irqCancelFailure != kIOReturnSuccess ||
        __atomic_load_n(&state->irqCancelPending, __ATOMIC_ACQUIRE) != 0) return false;
    for (uint32_t i = 0; i < maxInterruptVectors; ++i) {
        if (state->interruptSources[i] != nullptr) return false;
    }
    return true;
}

static bool ready_cold_regions_empty(VFIOUserPCIDriverUserClient_IVars *state)
{
    if (state->activeDMARegionCount || state->activeDMABytes ||
        state->dmaRetirementFailuresPending || state->dmaBuffer || state->dmaCommand ||
        state->dmaBufferSize || state->dmaBufferRetirementFailed) return false;
    if (state->clientDMARegions) {
        for (uint32_t i = 0; i < EXTGPU_MAX_DMA_REGIONS; ++i) {
            auto &region = state->clientDMARegions[i];
            if (region.command || region.memory) return false;
        }
    }
    return true;
}

/* Called only for a matching frozen BEGIN. A failed BME transaction may be
 * retried while still live; after either Stop marker there is no PCI access. */
static kern_return_t ready_cold_begin_reply(VFIOUserPCIDriverUserClient *client,
                                             IOUserClientMethodArguments *arguments)
{
    auto *state = client->ivars;
    auto key = ready_cold_key(arguments->scalarInput);
    if (!state->coldFrozen ||
        !extgpu_driver::same_cold_removal_key(key, state->coldLedger.key)) return kIOReturnNotPermitted;
    if (state->coldLedger.closed()) return kIOReturnNotPermitted;
    if (!state->coldBMEAcknowledged) {
        auto *driver = state->coldDriver;
        if (!driver || !driver->ivars) return kIOReturnNotAttached;
        ReadyProviderLock device(&driver->ivars->stateLock);
        if (state->coldStopped || driver->ivars->stopping) return kIOReturnNotAttached;
        if (!state->claimed || !driver->ivars->providerClaimed ||
            driver->ivars->openerClient != client ||
            driver->ivars->generation != key.generation) return kIOReturnNotPermitted;
        kern_return_t ret = ready_cold_disable_bus_master(vfio_user_get_pci_device(driver));
        if (ret != kIOReturnSuccess) return ret;
        state->coldBMEAcknowledged = true;
    }
    ready_cold_write_key(arguments->scalarOutput, state->coldLedger.key);
    arguments->scalarOutput[5] = state->coldLedger.count;
    arguments->scalarOutput[6] = EXTGPU_COLD_STATE_FROZEN;
    arguments->scalarOutputCount = EXTGPU_COLD_BEGIN_OUTPUT_COUNT;
    return kIOReturnSuccess;
}

static kern_return_t ready_cold_prepare(VFIOUserPCIDriverUserClient *client,
                                      VFIOUserPCIDriver *driver,
                                      IOUserClientMethodArguments *arguments)
{
    auto *state = client->ivars;
    const auto key = ready_cold_key(arguments->scalarInput);
    if (!extgpu_driver::valid_cold_removal_key(key)) return kIOReturnBadArgument;
    if (state->barMappingIssued || state->dmaBuffer || state->dmaCommand ||
        state->dmaBufferSize || state->dmaBufferRetirementFailed ||
        state->dmaRetirementFailuresPending || !ready_cold_irq_empty(state)) return kIOReturnUnsupported;
    if (state->activeDMARegionCount > EXTGPU_MAX_DMA_REGIONS) return kIOReturnNoSpace;

    uint64_t serviceID = 0, pciID = 0;
    ReadyProviderLock device(&driver->ivars->stateLock);
    if (driver->ivars->stopping || driver->ivars->quarantined) return kIOReturnNotAttached;
    if (!state->claimed || !state->protocolNegotiated ||
        !driver->ivars->providerClaimed || driver->ivars->openerClient != client ||
        state->negotiatedGeneration != driver->ivars->generation ||
        (driver->ivars->quarantinedClientState && driver->ivars->quarantinedClientState != state))
        return kIOReturnNotPermitted;
    auto *pci = vfio_user_get_pci_device(driver);
    if (!pci) return kIOReturnNotAttached;
    kern_return_t ret = driver->GetRegistryEntryID(&serviceID);
    if (ret != kIOReturnSuccess) return ret;
    ret = pci->GetRegistryEntryID(&pciID);
    if (ret != kIOReturnSuccess) return ret;
    extgpu_driver::ColdRemovalIdentity identity{driver->ivars->generation, serviceID, pciID};
    if (key.generation != identity.generation || key.serviceID != serviceID || key.pciID != pciID)
        return kIOReturnNotPermitted;

    const uint32_t capacity = (uint32_t)state->activeDMARegionCount;
    auto *entries = capacity ? IONewZero(extgpu_driver::ColdRemovalEntry, capacity) : nullptr;
    if (capacity && !entries) return kIOReturnNoMemory;
    uint32_t count = 0;
    uint64_t bytes = 0;
    bool valid = true;
    for (uint32_t i = 0; state->clientDMARegions && i < EXTGPU_MAX_DMA_REGIONS; ++i) {
        auto &region = state->clientDMARegions[i];
        if (!region.command && !region.memory) continue;
        if (count >= capacity || !region.command || !region.memory || !region.fullCoverage ||
            region.completionFailed || !region.bytes || region.bytes > UINT64_MAX - bytes) {
            valid = false;
            break;
        }
        entries[count++].map = {region.key, region.bytes, reinterpret_cast<uintptr_t>(&region)};
        bytes += region.bytes;
    }
    if (!valid || count != capacity || state->activeDMABytes != bytes) {
        IOSafeDeleteNULL(entries, extgpu_driver::ColdRemovalEntry, capacity);
        return kIOReturnUnsupported;
    }
    extgpu_driver::ColdRemovalLedger prepared{};
    auto armed = prepared.arm(key, identity, entries, capacity, count);
    if (armed != extgpu_driver::ColdRemovalResult::success) {
        IOSafeDeleteNULL(entries, extgpu_driver::ColdRemovalEntry, capacity);
        return ready_cold_result(armed);
    }
    uint64_t brokerID = 0;
    ret = ready_recovery_reserve(client, arguments->scalarInput, arguments->scalarInput + 5, count, &brokerID);
    if (ret != kIOReturnSuccess) {
        IOSafeDeleteNULL(entries, extgpu_driver::ColdRemovalEntry, capacity);
        return ret;
    }
    /* No fallible operation follows reservation. The broker already retains
     * this exact original object BEFORE ordinary IO becomes irreversible.
     * Ledger.arm above is metadata only, not a BME or hardware acknowledgement. */
    state->coldLedger = prepared;
    state->coldPrepared = true;
    state->claimPristine = false;
    state->hardwareMutated = true;
    state->coldDriver = driver;
    driver->retain();
    driver->ivars->quarantined = true;
    driver->ivars->quarantinedClientState = state;
    ready_recovery_publish(client);
    ready_cold_write_key(arguments->scalarOutput, key);
    arguments->scalarOutput[5] = brokerID;
    arguments->scalarOutputCount = EXTGPU_RECOVERY_PREPARE_OUTPUT_COUNT;
    return kIOReturnSuccess;
}

/* Original endpoint only. BIND proves possession of the private capability
 * through the already-open independent broker. Never retry PCI after Stop. */
static kern_return_t ready_cold_begin(VFIOUserPCIDriverUserClient *client,
                                      IOUserClientMethodArguments *arguments)
{
    auto *state = client->ivars;
    if (!state->coldPrepared || !ready_recovery_is_bound(client, arguments->scalarInput)) return kIOReturnNotReady;
    if (!extgpu_driver::same_cold_removal_key(ready_cold_key(arguments->scalarInput), state->coldLedger.key))
        return kIOReturnNotPermitted;
    if (state->coldFrozen) return ready_cold_begin_reply(client, arguments);
    auto *driver = state->coldDriver;
    if (!driver || !driver->ivars || state->coldStopped) return kIOReturnNotAttached;
    ReadyProviderLock device(&driver->ivars->stateLock);
    if (driver->ivars->stopping) return kIOReturnNotAttached;
    if (!state->claimed || !driver->ivars->providerClaimed || driver->ivars->openerClient != client ||
        driver->ivars->generation != state->coldLedger.key.generation) return kIOReturnNotPermitted;
    state->coldFrozen = true; // Irreversible BEFORE BME read/write/readback.
    ready_clear_bar_cache_locked(driver);
    const kern_return_t ret = ready_cold_disable_bus_master(vfio_user_get_pci_device(driver));
    if (ret != kIOReturnSuccess) return ret;
    state->coldBMEAcknowledged = true;
    ready_cold_write_key(arguments->scalarOutput, state->coldLedger.key);
    arguments->scalarOutput[5] = state->coldLedger.count;
    arguments->scalarOutput[6] = EXTGPU_COLD_STATE_FROZEN;
    arguments->scalarOutputCount = EXTGPU_COLD_BEGIN_OUTPUT_COUNT;
    return kIOReturnSuccess;
}

static ClientDMARegistration *ready_cold_region(
    VFIOUserPCIDriverUserClient_IVars *state, const extgpu_driver::ColdRemovalEntry &entry)
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(state->clientDMARegions);
    if (!base || entry.map.token < base ||
        entry.map.token - base >= sizeof(ClientDMARegistration) * EXTGPU_MAX_DMA_REGIONS ||
        (entry.map.token - base) % sizeof(ClientDMARegistration)) return nullptr;
    auto *region = reinterpret_cast<ClientDMARegistration *>(entry.map.token);
    if (!region->command || !region->memory || !region->fullCoverage ||
        region->key != entry.map.iova || region->bytes != entry.map.bytes) return nullptr;
    return region;
}

static kern_return_t ready_cold_retire(VFIOUserPCIDriverUserClient *client,
                                       IOUserClientMethodArguments *arguments)
{
    auto *state = client->ivars;
    if (!state->coldFrozen || !state->coldBMEAcknowledged) return kIOReturnNotReady;
    const auto key = ready_cold_key(arguments->scalarInput);
    struct Operations {
        VFIOUserPCIDriverUserClient *client;
        int32_t complete(extgpu_driver::ColdRemovalEntry &entry) {
            auto *region = ready_cold_region(client->ivars, entry);
            return region ? complete_registration(*client->ivars, *region) : kIOReturnBadArgument;
        }
        void release_command(extgpu_driver::ColdRemovalEntry &entry) {
            auto *region = reinterpret_cast<ClientDMARegistration *>(entry.map.token);
            release_registration_command(*client->ivars, *region);
        }
        void release_descriptor(extgpu_driver::ColdRemovalEntry &entry) {
            auto *region = reinterpret_cast<ClientDMARegistration *>(entry.map.token);
            erase_completed_registration(*client->ivars, *region);
        }
    } operations{client};
    /* Flags3 are explicit caller attestations, not Stop-derived evidence.
     * This path uses retained command objects only: no provider or PCI access. */
    const auto reply = state->coldLedger.retire(key, arguments->scalarInput[5],
        arguments->scalarInput[6], arguments->scalarInput[7], operations);
    const kern_return_t ret = ready_cold_result(reply.result, reply.nativeStatus);
    if (ret != kIOReturnSuccess) return ret;
    ready_cold_write_key(arguments->scalarOutput, key);
    arguments->scalarOutput[5] = arguments->scalarInput[5];
    arguments->scalarOutput[6] = arguments->scalarInput[6];
    arguments->scalarOutput[7] = 0;
    arguments->scalarOutput[8] = EXTGPU_COLD_STATE_SUCCESS_TOMBSTONE;
    arguments->scalarOutput[9] = reply.commitSequence;
    arguments->scalarOutputCount = EXTGPU_COLD_RETIRE_OUTPUT_COUNT;
    return kIOReturnSuccess;
}

static kern_return_t ready_cold_finish(VFIOUserPCIDriverUserClient *client,
                                       IOUserClientMethodArguments *arguments)
{
    auto *state = client->ivars;
    if (!state->coldFrozen || !state->coldBMEAcknowledged || !state->coldDriver ||
        !state->coldDriver->ivars) return kIOReturnNotReady;
    if (!ready_cold_irq_empty(state) || !ready_cold_regions_empty(state)) return kIOReturnStillOpen;
    const auto key = ready_cold_key(arguments->scalarInput);
    auto *driver = state->coldDriver;
    ReadyProviderLock device(&driver->ivars->stateLock);
    if ((driver->ivars->openerClient && driver->ivars->openerClient != client) ||
        (driver->ivars->quarantinedClientState && driver->ivars->quarantinedClientState != state))
        return kIOReturnNotPermitted;
    const auto reply = state->coldLedger.finish(key, arguments->scalarInput[5]);
    const kern_return_t ret = ready_cold_result(reply.result, reply.nativeStatus);
    if (ret != kIOReturnSuccess) return ret;
    state->coldFinished = true;
    state->ownerReleased = true;
    state->claimed = false;
    state->protocolNegotiated = false;
    driver->ivars->quarantined = true; /* Never a warm-handoff/trusted-idle claim. */
    driver->ivars->quarantinedClientState = state;
    if (driver->ivars->openerClient == client) {
        driver->ivars->openerClient = nullptr;
        driver->ivars->providerClaimed = false;
        driver->ivars->providerClaimRefs = 0;
    }
    ready_cold_write_key(arguments->scalarOutput, key);
    arguments->scalarOutput[5] = EXTGPU_COLD_STATE_CLOSED;
    arguments->scalarOutput[6] = reply.commitSequence;
    arguments->scalarOutputCount = EXTGPU_COLD_FINISH_OUTPUT_COUNT;
    return kIOReturnSuccess;
}

/* Called by the independent broker with a strong per-call original reference.
 * This function is LOCAL C++, never dispatches an RPC through the stopped PCI
 * endpoint, and never gets a provider or retries BME/configuration access. */
kern_return_t ready_recovery_local_call(VFIOUserPCIDriverUserClient *client, uint64_t selector,
    const uint64_t *input, uint32_t inputCount, uint64_t *output, uint32_t *outputCount)
{
    if (!client || !client->ivars || !input || !output || !outputCount) return kIOReturnBadArgument;
    uint32_t expectedIn = 0, expectedOut = 0;
    if (selector == EXTGPU_SELECTOR_COLD_BEGIN) { expectedIn = 5; expectedOut = 7; }
    else if (selector == EXTGPU_SELECTOR_COLD_RETIRE) { expectedIn = 8; expectedOut = 10; }
    else if (selector == EXTGPU_SELECTOR_COLD_FINISH) { expectedIn = 6; expectedOut = 7; }
    else return kIOReturnUnsupported;
    if (inputCount != expectedIn || *outputCount != expectedOut) return kIOReturnBadArgument;
    auto *state = client->ivars;
    ReadyProviderLock operation(&state->operationLock);
    if (!state->coldPrepared || !state->coldFrozen || !state->coldBMEAcknowledged) return kIOReturnNotReady;
    if (!extgpu_driver::same_cold_removal_key(ready_cold_key(input), state->coldLedger.key))
        return kIOReturnNotPermitted;
    IOUserClientMethodArguments arguments = {};
    arguments.scalarInput = const_cast<uint64_t *>(input);
    arguments.scalarInputCount = inputCount;
    arguments.scalarOutput = output;
    arguments.scalarOutputCount = *outputCount;
    kern_return_t ret;
    if (selector == EXTGPU_SELECTOR_COLD_BEGIN) {
        if (state->coldLedger.closed()) return kIOReturnNotPermitted;
        ready_cold_write_key(output, state->coldLedger.key);
        output[5] = state->coldLedger.count; output[6] = EXTGPU_COLD_STATE_FROZEN;
        arguments.scalarOutputCount = 7; ret = kIOReturnSuccess;
    } else if (selector == EXTGPU_SELECTOR_COLD_RETIRE) ret = ready_cold_retire(client, &arguments);
    else ret = ready_cold_finish(client, &arguments);
    if (ret == kIOReturnSuccess) *outputCount = arguments.scalarOutputCount;
    return ret;
}

/* OSObject::free is LOCALONLY in the used public IIG. No completion or PCI
 * operation is allowed here. Successful cold sessions reclaim metadata only
 * at final object release, preserving lost-reply tombstones until that point. */
void VFIOUserPCIDriverUserClient::free()
{
    auto *state = ivars;
    if (state && (state->coldPrepared || state->coldFrozen)) {
        auto *driver = state->coldDriver;
        bool dispose = false;
        {
            ReadyProviderLock operation(&state->operationLock);
            state->coldStopped = true;
            dispose = state->coldFinished && state->ownerReleased && state->coldLedger.closed() &&
                !state->coldLedger.inFlight && state->coldLedger.remaining() == 0 &&
                ready_cold_regions_empty(state) && ready_cold_irq_empty(state) && driver && driver->ivars;
            if (driver && driver->ivars) {
                ReadyProviderLock device(&driver->ivars->stateLock);
                driver->ivars->quarantined = true;
                if (dispose) {
                    if (driver->ivars->quarantinedClientState == state)
                        driver->ivars->quarantinedClientState = nullptr;
                } else {
                    driver->ivars->quarantinedClientState = state;
                }
                /* Final object free must not leave a dangling raw client
                 * pointer. Unfinished obligations remain in quarantined state,
                 * not as a usable claim; no hardware Close is issued here. */
                if (driver->ivars->openerClient == this) {
                    driver->ivars->openerClient = nullptr;
                    driver->ivars->providerClaimed = false;
                    driver->ivars->providerClaimRefs = 0;
                }
            }
            ivars = nullptr;
        }
        if (dispose) {
            const uint32_t capacity = state->coldLedger.capacity;
            OSSafeReleaseNULL(state->irqMailboxMemory);
            IOSafeDeleteNULL(state->coldLedger.entries, extgpu_driver::ColdRemovalEntry, capacity);
            IOSafeDeleteNULL(state->clientDMARegions, ClientDMARegistration, EXTGPU_MAX_DMA_REGIONS);
            IOLockFreeZero(state->operationLock.value);
            IOLockFreeZero(state->irqLock.value);
            IOSafeDeleteNULL(state, VFIOUserPCIDriverUserClient_IVars, 1);
            driver->release();
        }
        /* Otherwise the one quarantined original owner retains its bounded
         * ledger (<=4096 entries) and prepared resources; no false cleanup ACK. */
    }
    IOUserClient::free();
}

static kern_return_t vfio_user_reset_device(VFIOUserPCIDriverUserClient *client,
                                             VFIOUserPCIDriver *driver)
{
    auto &session = *client->ivars;
    auto &device = *driver->ivars;
    ReadyProviderLock lock(&device.stateLock);
    auto status = ready_owner_status_locked(client, driver, session.negotiatedGeneration);
    if (status != kIOReturnSuccess) return status;
    auto *pci = vfio_user_get_pci_device(driver);
    if (!pci) return kIOReturnUnsupported;
    if (session.barMappingIssued || session.irqStateExported || session.dmaBuffer ||
        session.activeDMARegionCount || session.interruptsSetUp ||
        __atomic_load_n(&session.irqCancelPending, __ATOMIC_ACQUIRE))
        return kIOReturnStillOpen;

    ready_clear_bar_cache_locked(driver);
    status = pci->Reset(kIOPCIDeviceResetTypeFunctionReset, kIOPCIDeviceResetOptionNone);
    if (status != kIOReturnSuccess) return status;

    ++device.generation;
    device.trustedQuiescent = true;
    session.hardwareMutated = false;
    session.everPreparedDMA = false;
    session.dmaAddressBits = 32;
    session.dmaCompleteFailureCount = 0;
    return kIOReturnSuccess;
}

static kern_return_t add_irq_vector(VFIOUserPCIDriverUserClient *client, IOPCIDevice *pci,
                                     uint32_t index)
{
    auto &state = *client->ivars;
    IOInterruptDispatchSource *source = nullptr;
    auto status = IOInterruptDispatchSource::Create(pci, index, state.irqQueue, &source);
    if (status != kIOReturnSuccess || !source) {
        OSSafeReleaseNULL(source);
        return status == kIOReturnSuccess ? kIOReturnNoMemory : status;
    }
    {
        ReadyProviderLock lock(&state.irqLock);
        state.interruptSources[index] = source;
    }

    OSAction *handler = nullptr;
    status = client->CreateActionDeliverVector(sizeof(index), &handler);
    if (status == kIOReturnSuccess) {
        auto *vector = handler ? static_cast<uint32_t *>(handler->GetReference()) : nullptr;
        if (vector) {
            *vector = index;
            status = source->SetHandler(handler);
        } else {
            status = kIOReturnNoMemory;
        }
    }
    OSSafeReleaseNULL(handler);
    return status;
}

static kern_return_t vfio_user_setup_interrupts(VFIOUserPCIDriverUserClient *client,
                                                 VFIOUserPCIDriver *driver)
{
    auto &state = *client->ivars;
    {
        ReadyProviderLock lock(&state.irqLock);
        if (state.irqCancelFailure != kIOReturnSuccess) return state.irqCancelFailure;
        if (state.irqStateExported) return kIOReturnUnsupported;
        if (__atomic_load_n(&state.irqCancelPending, __ATOMIC_ACQUIRE)) return kIOReturnNotReady;
        if (state.irqQueue || state.irqSharedBuffer || state.interruptsSetUp)
            return kIOReturnStillOpen;
        for (unsigned i = 0; i < maxInterruptVectors; ++i) {
            if (state.interruptSources[i]) return kIOReturnStillOpen;
            state.irqCancellations[i].state = 0;
        }
    }
    auto *pci = vfio_user_get_pci_device(driver);
    if (!pci) return kIOReturnUnsupported;

    constexpr uint64_t pageSize = 16384;
    IOBufferMemoryDescriptor *memory = nullptr;
    IODispatchQueue *queue = nullptr;
    IOAddressSegment range{};
    auto status = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionOutIn,
                                                   pageSize, pageSize, &memory);
    if (status == kIOReturnSuccess)
        status = memory ? memory->GetAddressRange(&range) : kIOReturnNoMemory;
    if (status == kIOReturnSuccess)
        status = IODispatchQueue::Create("OpenPCIInterrupts", 0, 0, &queue);
    if (status != kIOReturnSuccess || !queue || !range.address ||
        range.length < 2 * EXTGPU_IRQ_BITMAP_WORDS * sizeof(uint64_t)) {
        OSSafeReleaseNULL(queue);
        OSSafeReleaseNULL(memory);
        return status == kIOReturnSuccess ? kIOReturnNoMemory : status;
    }

    // The client maps the first two arrays in this page as pending and enabled bits.
    auto *words = reinterpret_cast<volatile uint64_t *>(range.address);
    for (unsigned i = 0; i < EXTGPU_IRQ_BITMAP_WORDS; ++i) {
        words[i] = 0;
        words[i + EXTGPU_IRQ_BITMAP_WORDS] = UINT64_MAX;
    }
    state.irqQueue = queue;
    state.irqSharedBuffer = memory;
    state.irqPending = words;
    state.irqEnabled = words + EXTGPU_IRQ_BITMAP_WORDS;

    const auto configure = [&]() -> kern_return_t {
        ReadyProviderLock lock(&driver->ivars->stateLock);
        auto result = ready_owner_status_locked(client, driver, state.negotiatedGeneration);
        if (result != kIOReturnSuccess) return result;

        bool msix = false;
        auto count = requested_interrupt_count(pci, &msix);
        result = pci->ConfigureInterrupts(
            msix ? kIOInterruptTypePCIMessagedX : kIOInterruptTypePCIMessaged, 1, count, 0);
        if (result != kIOReturnSuccess && msix) {
            count = 1;
            result = pci->ConfigureInterrupts(kIOInterruptTypePCIMessaged, 1, count, 0);
        }
        if (result != kIOReturnSuccess) return result;

        for (uint32_t index = 0; index < count; ++index) {
            result = add_irq_vector(client, pci, index);
            if (result != kIOReturnSuccess) return result;
        }
        {
            ReadyProviderLock irqLock(&state.irqLock);
            state.numInterrupts = count;
            state.interruptsSetUp = true;
        }
        for (uint32_t index = 0; index < count; ++index) {
            result = state.interruptSources[index]->SetEnable(true);
            if (result != kIOReturnSuccess) return result;
        }
        return kIOReturnSuccess;
    };
    status = configure();
    if (status == kIOReturnSuccess) return status;

    os_log(OS_LOG_DEFAULT, "OpenPCI IRQ setup failed: %#x", status);
    vfio_user_release_all_interrupts(client);
    ReadyProviderLock lock(&state.irqLock);
    return state.irqCancelFailure == kIOReturnSuccess ? status : state.irqCancelFailure;
}

static kern_return_t ready_release_owner(VFIOUserPCIDriverUserClient *client,
                                          VFIOUserPCIDriver *driver, uint64_t expectedGeneration)
{
    auto *state = client->ivars;
    if (expectedGeneration != state->negotiatedGeneration) return kIOReturnNotPermitted;
    /* There is no verified public API here to revoke an exported physical BAR
     * view in another process. Never permit a second owner while it may exist.
     */
    if (state->barMappingIssued || state->irqStateExported) return kIOReturnUnsupported;
    if (state->dmaBuffer || state->activeDMARegionCount) return kIOReturnStillOpen;
    const bool pristine = state->claimPristine && !state->hardwareMutated &&
        !state->everPreparedDMA && !state->interruptsSetUp && !state->irqSharedBuffer &&
        !state->dmaRetirementFailuresPending && state->irqCancelFailure == kIOReturnSuccess &&
        __atomic_load_n(&state->irqCancelPending, __ATOMIC_ACQUIRE) == 0;
    if (!pristine) {
        const auto ret = ready_retire_interrupts(client);
        if (ret != kIOReturnSuccess) return ret;
    }

    ReadyProviderLock provider(&driver->ivars->stateLock);
    auto ret = ready_owner_status_locked(client, driver, expectedGeneration);
    if (ret != kIOReturnSuccess) return ret;
    auto *pci = vfio_user_get_pci_device(driver);
    if (!pci) return kIOReturnNotAttached;
    if (!pristine) {
        ret = ready_cold_disable_bus_master(pci);
        if (ret != kIOReturnSuccess) return ret;
        ready_clear_bar_cache_locked(driver);
        if (!driver->ivars->trustedQuiescent || state->everPreparedDMA || state->hardwareMutated) {
            ret = pci->Reset(kIOPCIDeviceResetTypeFunctionReset, kIOPCIDeviceResetOptionNone);
            if (ret != kIOReturnSuccess) return ret; /* Failed attempts also invalidate metadata. */
        }
    }
    /* A pristine close leaves the prior quiescence status untouched. */
    ready_close_claim_locked(client, driver, pci, !pristine);
    return kIOReturnSuccess;
}

static bool ready_vm_window_matches(const VFIOUserPCIDriverUserClient_IVars *state,
                                    const extgpu_driver::WarmVMKey &key, bool require_full_coverage)
{
    if (state->dmaBuffer || state->dmaCommand ||
        (state->barMappingIssued && !state->authBarRetired) || state->irqStateExported ||
        !state->clientDMARegions)
        return false;
    const bool dynamic = state->dynamicDMAWindow.matches(key);
    if (!dynamic && (state->activeDMARegionCount != 1 || state->activeDMABytes != key.bytes)) return false;
    bool primary = false;
    for (unsigned i = 0; i < EXTGPU_MAX_DMA_REGIONS; ++i) {
        const auto &region = state->clientDMARegions[i];
        if (!region.command) continue;
        if (dynamic && region.vmDynamic) continue;
        if (primary || region.key != key.registration || region.bytes != key.bytes || !region.memory ||
            (require_full_coverage && !region.fullCoverage)) return false;
        primary = true;
    }
    return primary;
}

static bool ready_hypervisor_client_entitled(VFIOUserPCIDriverUserClient *client)
{
    OSDictionary *entitlements = nullptr;
    if (client->CopyClientEntitlements(&entitlements) != kIOReturnSuccess || !entitlements) {
        if (entitlements) entitlements->release();
        return false;
    }
    auto *access = OSDynamicCast(OSArray, entitlements->getObject("com.apple.developer.driverkit.userclient-access"));
    bool allowed = false;
    if (access && entitlements->getObject("com.apple.security.hypervisor") == kOSBooleanTrue) {
        for (unsigned i = 0; i < access->getCount(); ++i) {
            auto *name = OSDynamicCast(OSString, access->getObject(i));
            if (name && name->isEqualTo(EXTGPU_DRIVER_BUNDLE_ID)) {
                allowed = true;
                break;
            }
        }
    }
    entitlements->release();
    return allowed;
}

static bool ready_reset_member(VFIOUserPCIDriver *driver, extgpu_driver::ResetFunction *out)
{
    auto *pci = vfio_user_get_pci_device(driver);
    VFIOUserPCIDeviceIdentity identity;
    uint64_t registry = 0;
    if (!pci || pci->GetRegistryEntryID(&registry) != kIOReturnSuccess || !registry ||
        vfio_user_read_identity(driver, &identity) != kIOReturnSuccess)
        return false;
    *out = {registry, identity.vendorID, identity.deviceID, identity.classCode,
            identity.bus, identity.device, identity.function};
    return true;
}

static bool ready_post_reset_quiet(IOPCIDevice *pci, uint64_t id, uint32_t expected_pci_id)
{
    uint64_t actual = 0, cap = 0;
    uint32_t pci_id = UINT32_MAX;
    uint16_t command = UINT16_MAX, status = UINT16_MAX;
    if (!pci || pci->GetRegistryEntryID(&actual) != kIOReturnSuccess || actual != id ||
        pci->FindPCICapability(kIOPCICapabilityIDPCIExpress, 0, &cap) != kIOReturnSuccess ||
        cap < 0x40 || cap > 0xf4 || cap % 4)
        return false;
    pci->ConfigurationRead32(0x00, &pci_id);
    pci->ConfigurationRead16(0x04, &command);
    pci->ConfigurationRead16(static_cast<uint32_t>(cap + 0x0a), &status);
    return pci_id == expected_pci_id && command != UINT16_MAX && status != UINT16_MAX &&
        !(command & 4) && !(status & (1u << 5));
}

/* The trusted host checks bridge membership just before sending this RPC.
 * DriverKit exposes provider identity but no bridge-child iterator. Here we
 * additionally require the exact, live same-server pair, both functions
 * exclusively held, and exact registry IDs before and after reset. */
static const extgpu_driver::ResetPair *ready_warm_reset_pair(uint64_t selector)
{
    switch (selector) {
    case EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA: return &extgpu_driver::VegaResetPair;
    case EXTGPU_SELECTOR_WARM_RELEASE_VM_RTX3090: return &extgpu_driver::RTX3090ResetPair;
    case EXTGPU_SELECTOR_WARM_RELEASE_VM_POLARIS: return &extgpu_driver::PolarisResetPair;
    default: return nullptr;
    }
}

static kern_return_t ready_group_preflight(VFIOUserPCIDriverUserClient *client,
                                          VFIOUserPCIDriver *gpu,
                                          const IOUserClientMethodArguments *args)
{
    if (!args->scalarInput || args->scalarInputCount != EXTGPU_VM_GROUP_PREFLIGHT_WORDS ||
        args->scalarOutputCount != 0)
        return kIOReturnBadArgument;
    const auto *in = args->scalarInput;
    const extgpu_driver::PCIResetScope scope{in[2], in[3], in[4]};
    const auto *pair = ready_warm_reset_pair(in[1]);
    if (!scope.valid_ids() || !pair) return kIOReturnBadArgument;
    if (!ready_hypervisor_client_entitled(client)) return kIOReturnNotPermitted;

    auto *audio = retain_reset_companion(scope.audio);
    if (!audio) return kIOReturnUnsupported;
    kern_return_t result = kIOReturnNotAttached;
    if (audio != gpu && audio->ivars) {
        ReadyProviderLock display(&gpu->ivars->stateLock);
        ReadyProviderLock companion(&audio->ivars->stateLock);
        if (!gpu->ivars->stopping && !audio->ivars->stopping) {
            if (gpu->ivars->quarantined || gpu->ivars->generation != in[0] ||
                (gpu->ivars->providerClaimed && gpu->ivars->openerClient != client) ||
                audio->ivars->resetCompanionInUse) {
                result = kIOReturnNotReady;
            } else {
                extgpu_driver::ResetFunction displayIdentity{}, audioIdentity{};
                result = audio->ivars->resetCompanion &&
                    ready_reset_member(gpu, &displayIdentity) && ready_reset_member(audio, &audioIdentity) &&
                    scope.is_pair(displayIdentity, audioIdentity, *pair) ? kIOReturnSuccess : kIOReturnUnsupported;
            }
        }
    }
    audio->release();
    return result;
}

static kern_return_t ready_begin_group_warm_release(VFIOUserPCIDriverUserClient *client,
                                                   VFIOUserPCIDriver *gpu,
                                                   const extgpu_driver::WarmVMKey &key,
                                                   const extgpu_driver::PCIResetScope &scope,
                                                   uint32_t selector)
{
    using Ledger = extgpu_driver::WarmVMRelease;
    const auto *pair = ready_warm_reset_pair(
        selector == EXTGPU_SELECTOR_PROBE_VEGA_PERST ? EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA : selector);
    const bool entitled = ready_hypervisor_client_entitled(client);
    if (!scope.valid_ids() || !entitled || !pair) {
        os_log(OS_LOG_DEFAULT, "extgpu: WarmVM diagnostic scope_or_entitlement ids=%d entitled=%d",
               scope.valid_ids(), entitled);
        return kIOReturnUnsupported;
    }
    auto *audio = retain_reset_companion(scope.audio);
    if (!audio) {
        os_log(OS_LOG_DEFAULT, "extgpu: WarmVM diagnostic companion_not_found");
        return kIOReturnUnsupported;
    }
    struct UnattachedAudio {
        VFIOUserPCIDriver *audio;
        ~UnattachedAudio() { if (audio) audio->release(); }
    } retained{audio};
    if (!audio->ivars || audio == gpu) {
        os_log(OS_LOG_DEFAULT, "extgpu: WarmVM diagnostic companion_state_invalid");
        return kIOReturnUnsupported;
    }

    auto *state = client->ivars;
    ReadyProviderLock display(&gpu->ivars->stateLock);
    ReadyProviderLock companion(&audio->ivars->stateLock);
    if (gpu->ivars->stopping || gpu->ivars->quarantined || !state->claimed ||
        !state->protocolNegotiated || state->negotiatedGeneration != key.generation ||
        gpu->ivars->generation != key.generation || !gpu->ivars->providerClaimed ||
        gpu->ivars->openerClient != client || !audio->ivars->resetCompanion ||
        audio->ivars->stopping || audio->ivars->resetCompanionInUse)
        return kIOReturnNotPermitted;
    extgpu_driver::ResetFunction g{}, a{};
    const bool gpuMember = ready_reset_member(gpu, &g);
    const bool audioMember = ready_reset_member(audio, &a);
    const bool pairMatches = gpuMember && audioMember && scope.is_pair(g, a, *pair);
    if (!pairMatches) {
        os_log(OS_LOG_DEFAULT,
               "extgpu: WarmVM diagnostic reset_pair_invalid gpu=%d audio=%d pair=%d",
               gpuMember, audioMember, pairMatches);
        return kIOReturnUnsupported;
    }
    auto *ap = vfio_user_get_pci_device(audio);
    auto &ledger = state->warmVMRelease;
    if (!ap || !ledger.begin(key, scope.display, scope, selector))
        return kIOReturnError;

    /* Freeze ordinary access before opening/programming the sibling.
     * Even an ambiguous Open result retains both original references. */
    state->warmAudioOwner = audio;
    retained.audio = nullptr;
    audio->ivars->resetCompanionInUse = true;
    if (ap->Open(audio, 0) != kIOReturnSuccess) return kIOReturnError;
    ready_clear_bar_cache_locked(gpu);
    ledger.pci_identifier = g.device << 16 | g.vendor;
    ledger.companion_pci_identifier = pair->audio_id();
    return ledger.advance(Ledger::quiescing) ? kIOReturnSuccess : kIOReturnError;
}

static kern_return_t ready_flr_advertised(IOPCIDevice *pci, bool *advertised)
{
    uint64_t cap = 0;
    const auto ret = pci->FindPCICapability(kIOPCICapabilityIDPCIExpress, 0, &cap);
    if (ret != kIOReturnSuccess) return ret;
    if (cap < 0x40 || cap > 0xf4 || cap % 4) return kIOReturnError;
    uint32_t features = UINT32_MAX;
    pci->ConfigurationRead32(static_cast<uint32_t>(cap + 4), &features);
    if (features == UINT32_MAX) return kIOReturnNotAttached;
    *advertised = (features & (UINT32_C(1) << 28)) != 0;
    return kIOReturnSuccess;
}

static kern_return_t ready_vm_flr_preflight(VFIOUserPCIDriverUserClient *client,
                                           VFIOUserPCIDriver *driver,
                                           IOUserClientMethodArguments *arguments)
{
    if (!arguments->scalarInput || arguments->scalarInputCount != 1 || !arguments->scalarOutput ||
        arguments->scalarOutputCount != EXTGPU_FLR_PREFLIGHT_REPLY_WORDS)
        return kIOReturnBadArgument;
    auto *state = client->ivars;
    if (!state->claimPristine || state->hardwareMutated || state->everPreparedDMA || state->barMappingIssued ||
        !ready_cold_irq_empty(state) || !ready_cold_regions_empty(state)) return kIOReturnNotPermitted;
    ReadyProviderLock provider(&driver->ivars->stateLock);
    const uint64_t generation = arguments->scalarInput[0];
    if (driver->ivars->stopping || driver->ivars->quarantined || !driver->ivars->providerClaimed ||
        driver->ivars->openerClient != client || generation != driver->ivars->generation)
        return kIOReturnNotReady;
    IOPCIDevice *pci = vfio_user_get_pci_device(driver);
    uint64_t registry = 0;
    uint16_t command = UINT16_MAX;
    if (!pci || pci->GetRegistryEntryID(&registry) != kIOReturnSuccess || !registry)
        return kIOReturnNotAttached;
    pci->ConfigurationRead16(4, &command);
    if (command == UINT16_MAX || (command & 4)) return kIOReturnNotReady;
    bool advertised = false;
    const auto ret = ready_flr_advertised(pci, &advertised);
    if (ret != kIOReturnSuccess) return ret;
    const uint64_t reply[] = {generation, registry, advertised};
    memcpy(arguments->scalarOutput, reply, sizeof reply);
    return kIOReturnSuccess;
}

static kern_return_t ready_restore_vega_bars(VFIOUserPCIDriverUserClient *client,
                                            VFIOUserPCIDriver *driver,
                                            IOUserClientMethodArguments *args)
{
    using Assignments = extgpu_driver::VegaBarAssignments;
    if (!args->scalarInput || args->scalarInputCount != EXTGPU_RESTORE_VEGA_BARS_WORDS ||
        !args->scalarOutput || args->scalarOutputCount != EXTGPU_RESTORE_VEGA_BARS_WORDS)
        return kIOReturnBadArgument;
    auto *state = client->ivars;
    if (!state->claimPristine || state->hardwareMutated || state->everPreparedDMA || state->barMappingIssued ||
        !ready_cold_irq_empty(state) || !ready_cold_regions_empty(state) || !ready_hypervisor_client_entitled(client))
        return kIOReturnNotPermitted;
    ReadyProviderLock lock(&driver->ivars->stateLock);
    if (driver->ivars->stopping || driver->ivars->quarantined || !driver->ivars->providerClaimed ||
        driver->ivars->openerClient != client || driver->ivars->generation != args->scalarInput[0])
        return kIOReturnNotReady;

    auto *pci = vfio_user_get_pci_device(driver);
    extgpu_driver::ResetFunction identity{};
    if (!pci || !ready_reset_member(driver, &identity) || identity.registry != args->scalarInput[1] ||
        identity.vendor != 0x1002 || identity.device != 0x687f || identity.class_code != 0x030000 ||
        identity.function != 0 || ready_validate_external_gpu(pci) != kIOReturnSuccess)
        return kIOReturnUnsupported;
    uint32_t revision = UINT32_MAX, subsystem = UINT32_MAX;
    pci->ConfigurationRead32(8, &revision);
    pci->ConfigurationRead32(0x2c, &subsystem);
    if (revision != 0x030000c1u || subsystem != 0xe37f1da2u) return kIOReturnUnsupported;
    uint16_t command = UINT16_MAX;
    pci->ConfigurationRead16(4, &command);
    if ((command & 7u) || !ready_post_reset_quiet(pci, identity.registry, 0x687f1002u))
        return kIOReturnNotReady;

    OSDictionary *props = nullptr;
    const auto ret = pci->CopyProperties(&props);
    if (ret != kIOReturnSuccess || !props) {
        if (props) props->release();
        return kIOReturnNotAttached;
    }
    auto *data = OSDynamicCast(OSData, props->getObject("assigned-addresses"));
    Assignments assignments{};
    uint32_t bdf = uint32_t(identity.bus) << 16 | uint32_t(identity.slot) << 11 | uint32_t(identity.function) << 8;
    const bool valid = data && assignments.parse(static_cast<const uint8_t *>(data->getBytesNoCopy()),
                                                  data->getLength(), bdf);
    props->release();
    if (!valid) return kIOReturnUnsupported;

    uint32_t expected[3]{}, before[3]{};
    for (unsigned i = 0; i < 3; ++i) {
        const unsigned reg = Assignments::registers[i];
        const uint8_t flags = i < 2 ? 0xc : 0;
        uint8_t memory = 0, type = 0;
        uint64_t size = 0;
        if (pci->GetBARInfo((reg - 0x10) / 4, &memory, &size, &type) != kIOReturnSuccess ||
            size != assignments.ranges[i].size || type != flags)
            return kIOReturnUnsupported;
        expected[i] = assignments.ranges[i].address | flags;
        pci->ConfigurationRead32(reg, &before[i]);
        if (before[i] != expected[i] && before[i] != flags) return kIOReturnUnsupported;
        if (i < 2) {
            uint32_t upper = UINT32_MAX;
            pci->ConfigurationRead32(reg + 4, &upper);
            if (upper) return kIOReturnUnsupported;
        }
    }

    bool complete = true;
    for (unsigned i = 0; i < 3; ++i) {
        if (before[i] == expected[i]) continue;
        const unsigned reg = Assignments::registers[i];
        uint16_t check_command = UINT16_MAX;
        pci->ConfigurationRead16(4, &check_command);
        if (check_command != command) return kIOReturnNotReady;
        state->hardwareMutated = true;
        pci->ConfigurationWrite32(reg, expected[i]);
        uint32_t check = UINT32_MAX;
        pci->ConfigurationRead32(reg, &check);
        if (check != expected[i] && check != before[i]) return kIOReturnError;
        complete = complete && check == expected[i];
    }
    uint16_t final_command = UINT16_MAX;
    pci->ConfigurationRead16(4, &final_command);
    if (final_command != command || !ready_post_reset_quiet(pci, identity.registry, 0x687f1002u))
        return kIOReturnError;
    // Restoring only the kernel's existing assignments with decode off
    // changes no GPU workload state and leaves a safely releasable claim.
    state->hardwareMutated = false;
    ready_clear_bar_cache_locked(driver);
    if (!complete) return kIOReturnUnsupported;
    memcpy(args->scalarOutput, args->scalarInput, EXTGPU_RESTORE_VEGA_BARS_WORDS * sizeof(uint64_t));
    return kIOReturnSuccess;
}

static kern_return_t ready_begin_flr_warm_release(VFIOUserPCIDriverUserClient *client,
                                                VFIOUserPCIDriver *driver, IOPCIDevice *pci,
                                                const extgpu_driver::WarmVMKey &key)
{
    using Ledger = extgpu_driver::WarmVMRelease;
    auto *state = client->ivars;
    auto &ledger = state->warmVMRelease;
    ReadyProviderLock provider(&driver->ivars->stateLock);
    if (driver->ivars->stopping || driver->ivars->quarantined ||
        !state->claimed || !state->protocolNegotiated ||
        state->negotiatedGeneration != key.generation ||
        driver->ivars->generation != key.generation ||
        !driver->ivars->providerClaimed || driver->ivars->openerClient != client)
        return kIOReturnNotPermitted;

    bool advertised = false;
    const auto eligibility = ready_flr_advertised(pci, &advertised);
    if (eligibility != kIOReturnSuccess) return eligibility;
    if (!advertised) return kIOReturnUnsupported;

    uint32_t pci_id = UINT32_MAX;
    uint64_t registry = 0;
    pci->ConfigurationRead32(0x00, &pci_id);
    if (pci_id == UINT32_MAX || !(pci_id & 0xffff) ||
        pci->GetRegistryEntryID(&registry) != kIOReturnSuccess || !registry)
        return kIOReturnNotAttached;
    if (!ledger.begin(key, registry)) return kIOReturnError;
    ready_clear_bar_cache_locked(driver);
    ledger.pci_identifier = pci_id;
    return ledger.advance(Ledger::quiescing) ? kIOReturnSuccess : kIOReturnError;
}

static kern_return_t ready_disable_original_master(IOPCIDevice *pci, uint64_t expected_registry,
                                                    uint32_t expected_pci_id)
{
    uint64_t actual_registry = 0;
    if (!pci || pci->GetRegistryEntryID(&actual_registry) != kIOReturnSuccess ||
        actual_registry != expected_registry)
        return kIOReturnNotAttached;

    uint32_t identifier = UINT32_MAX;
    pci->ConfigurationRead32(0x00, &identifier);
    if (identifier == UINT32_MAX) return kIOReturnNotReady;
    if (identifier != expected_pci_id) return kIOReturnNotPermitted;
    return ready_cold_disable_bus_master(pci) == kIOReturnSuccess ? kIOReturnSuccess : kIOReturnNotReady;
}

static kern_return_t ready_quiesce_warm_owners(VFIOUserPCIDriverUserClient *client,
                                               VFIOUserPCIDriver *driver, IOPCIDevice *pci)
{
    using Ledger = extgpu_driver::WarmVMRelease;
    auto *state = client->ivars;
    auto &ledger = state->warmVMRelease;
    ReadyProviderLock provider(&driver->ivars->stateLock);
    if (driver->ivars->stopping || driver->ivars->quarantined) return kIOReturnNotAttached;
    const auto owner = ready_owner_status_locked(client, driver, ledger.key.generation);
    if (owner != kIOReturnSuccess) return owner;

    auto result = ready_disable_original_master(pci, ledger.pci_registry, ledger.pci_identifier);
    if (result != kIOReturnSuccess) return result;
    if (ledger.selector != EXTGPU_SELECTOR_WARM_RELEASE_VM_FLR) {
        auto *audio = state->warmAudioOwner;
        if (!audio || !audio->ivars) return kIOReturnNotAttached;
        ReadyProviderLock companion(&audio->ivars->stateLock);
        if (audio->ivars->stopping || !audio->ivars->resetCompanionInUse) return kIOReturnNotAttached;
        result = ready_disable_original_master(vfio_user_get_pci_device(audio), ledger.group_scope.audio,
                                               ledger.companion_pci_identifier);
        if (result != kIOReturnSuccess) return result;
    }
    return ledger.advance(Ledger::draining) ? kIOReturnSuccess : kIOReturnError;
}

static kern_return_t ready_issue_drained_reset(VFIOUserPCIDriverUserClient *client,
                                               VFIOUserPCIDriver *driver, IOPCIDevice *pci)
{
    using Ledger = extgpu_driver::WarmVMRelease;
    auto *state = client->ivars;
    auto &ledger = state->warmVMRelease;
    const bool perst_probe = ledger.selector == EXTGPU_SELECTOR_PROBE_VEGA_PERST;
    ReadyProviderLock provider(&driver->ivars->stateLock);
    if (driver->ivars->stopping || driver->ivars->quarantined || !driver->ivars->providerClaimed ||
        driver->ivars->openerClient != client || driver->ivars->generation != ledger.key.generation)
        return kIOReturnNotAttached;

    const auto reset_when_quiet = [&](IOOptionBits kind) -> kern_return_t {
        if (!ledger.arm_reset(ready_post_reset_quiet(pci, ledger.pci_registry, ledger.pci_identifier)))
            return kIOReturnNotReady;
        const auto result = pci->Reset(kind, kIOPCIDeviceResetOptionNone);
        if (perst_probe) {
            os_log(OS_LOG_DEFAULT, "extgpu: scoped idle Vega PERST probe returned %#x", result);
            if (result == kIOReturnSuccess) ledger.perst_outcome = EXTGPU_VEGA_PERST_RESET;
            else if (result == kIOReturnUnsupported) ledger.perst_outcome = EXTGPU_VEGA_PERST_UNSUPPORTED;
            else ledger.perst_outcome = EXTGPU_VEGA_PERST_NATIVE_ERROR | static_cast<uint32_t>(result);
        } else if (result != kIOReturnSuccess) {
            os_log(OS_LOG_DEFAULT, "extgpu: VM reset returned %#x; keeping original DMA lease", result);
            return kIOReturnError;
        }
        return ledger.advance(Ledger::reset_returned) ? kIOReturnSuccess : kIOReturnError;
    };

    if (ledger.selector == EXTGPU_SELECTOR_WARM_RELEASE_VM_FLR)
        return reset_when_quiet(kIOPCIDeviceResetTypeFunctionReset);
    auto *audio = state->warmAudioOwner;
    if (!audio || !audio->ivars) return kIOReturnNotAttached;
    ReadyProviderLock companion(&audio->ivars->stateLock);
    if (audio->ivars->stopping || !audio->ivars->resetCompanionInUse)
        return kIOReturnNotAttached;
    auto *audio_pci = vfio_user_get_pci_device(audio);
    if (!ready_post_reset_quiet(audio_pci, ledger.group_scope.audio, ledger.companion_pci_identifier))
        return kIOReturnNotReady;
    return reset_when_quiet(perst_probe ? kIOPCIDeviceResetTypeWarmReset : kIOPCIDeviceResetTypeHotReset);
}

static kern_return_t ready_warm_release_vm(VFIOUserPCIDriverUserClient *client,
                                          IOUserClientMethodArguments *arguments, uint32_t selector)
{
    using Ledger = extgpu_driver::WarmVMRelease;
    const bool group = selector != EXTGPU_SELECTOR_WARM_RELEASE_VM_FLR;
    const bool perst_probe = selector == EXTGPU_SELECTOR_PROBE_VEGA_PERST;
    const unsigned request_words = group ? EXTGPU_WARM_VEGA_REQUEST_WORDS : EXTGPU_WARM_VM_REQUEST_WORDS;
    const unsigned reply_words = perst_probe ? EXTGPU_VEGA_PERST_REPLY_WORDS :
        group ? EXTGPU_WARM_VEGA_REPLY_WORDS : EXTGPU_WARM_VM_REPLY_WORDS;
    if (!arguments->scalarInput || arguments->scalarInputCount != request_words ||
        !arguments->scalarOutput || arguments->scalarOutputCount != reply_words)
        return kIOReturnBadArgument;
    auto *state = client->ivars;
    auto &ledger = state->warmVMRelease;
    const auto *in = arguments->scalarInput;
    const extgpu_driver::WarmVMKey key = {in[0], in[1], in[2]};
    const bool no_dma = !perst_probe && !key.bytes;
    const extgpu_driver::PCIResetScope scope = group ?
        extgpu_driver::PCIResetScope{in[3], in[4], in[5]} : extgpu_driver::PCIResetScope{};
    if ((group && !scope.valid_ids()) || (!key.bytes && key.registration) ||
        (perst_probe && key.bytes))
        return kIOReturnBadArgument;
    if (ledger.phase != Ledger::idle && (ledger.selector != selector ||
        !ledger.key.matches(key) || !ledger.group_scope.matches(scope)))
        return kIOReturnNotPermitted;
    if (ledger.phase == Ledger::preparing || ledger.phase == Ledger::armed)
        return kIOReturnError; // Preparation or reset outcome is unproven.

    if (ledger.phase != Ledger::closed) {
        auto *driver = OSDynamicCast(VFIOUserPCIDriver, client->GetProvider());
        if (!driver || !driver->ivars) return kIOReturnNotAttached;
        IOPCIDevice *pci = vfio_user_get_pci_device(driver);
        if (!pci) return kIOReturnNotAttached;

        if (ledger.phase == Ledger::idle) {
            const bool startup = !perst_probe && state->startupDMARequest.matches(key) && key.bytes &&
                !state->hardwareMutated && !state->barMappingIssued && !state->irqStateExported;
            const bool eligible = perst_probe ?
                state->claimPristine && !state->hardwareMutated && !state->everPreparedDMA &&
                    !state->barMappingIssued && ready_cold_regions_empty(state) : no_dma ?
                !state->dmaRequested && !state->everPreparedDMA && !state->barMappingIssued &&
                    ready_cold_regions_empty(state) :
                (startup && ready_cold_regions_empty(state)) || ready_vm_window_matches(state, key, !startup);
            if (state->coldPrepared || state->coldFrozen || !eligible ||
                (state->dmaRetirementFailuresPending && !startup && !state->dynamicDMAWindow.matches(key))) {
                os_log(OS_LOG_DEFAULT,
                       "extgpu: WarmVM diagnostic ineligible startup=%d eligible=%d bar=%d retired=%d dma_regions=%llu dma_pending=%llu",
                       startup, eligible, state->barMappingIssued, state->authBarRetired,
                       static_cast<unsigned long long>(state->activeDMARegionCount),
                       static_cast<unsigned long long>(state->dmaRetirementFailuresPending));
                return kIOReturnUnsupported;
            }
            if (!ready_cold_irq_empty(state)) return kIOReturnNotReady;
            const auto ret = group ? ready_begin_group_warm_release(client, driver, key, scope, selector) :
                                    ready_begin_flr_warm_release(client, driver, pci, key);
            if (ret != kIOReturnSuccess) return ret;
        }
        if (ledger.phase == Ledger::quiescing) {
            const auto ret = ready_quiesce_warm_owners(client, driver, pci);
            if (ret != kIOReturnSuccess) return ret;
        }
        if (ledger.phase == Ledger::draining) {
            const auto ret = ready_issue_drained_reset(client, driver, pci);
            if (ret != kIOReturnSuccess) return ret;
        }
        if (ledger.phase == Ledger::reset_returned || ledger.phase == Ledger::reset_verified) {
            ReadyProviderLock provider(&driver->ivars->stateLock);
            const auto owner = ready_owner_status_locked(client, driver, key.generation);
            if (owner != kIOReturnSuccess) return owner;
            if (!ready_post_reset_quiet(pci, ledger.pci_registry, ledger.pci_identifier))
                return kIOReturnNotReady;
            auto complete = [&] {
                if (ledger.phase == Ledger::reset_returned && !ledger.advance(Ledger::reset_verified))
                    return kIOReturnError;
                if (!key.bytes || (state->startupDMARequest.matches(key) && ready_cold_regions_empty(state)))
                    return kIOReturnSuccess;
                if (state->dynamicDMAWindow.matches(key)) {
                    for (unsigned i = 0; i < EXTGPU_MAX_DMA_REGIONS; ++i) {
                        auto &region = state->clientDMARegions[i];
                        if (region.command && region.vmDynamic && retire_registration(*state, region) != kIOReturnSuccess)
                            return kIOReturnError;
                    }
                }
                return vfio_user_unregister_client_dma_region(client, key.registration);
            };
            kern_return_t ret;
            if (group) {
                auto *audio = state->warmAudioOwner;
                if (!audio || !audio->ivars) return kIOReturnNotAttached;
                ReadyProviderLock companion(&audio->ivars->stateLock);
                if (audio->ivars->stopping || !audio->ivars->resetCompanionInUse)
                    return kIOReturnNotAttached;
                if (!ready_post_reset_quiet(vfio_user_get_pci_device(audio), scope.audio,
                                            ledger.companion_pci_identifier))
                    return kIOReturnNotReady;
                ret = complete();
            } else ret = complete();
            if (ret != kIOReturnSuccess) return kIOReturnError;
            if (!ledger.advance(Ledger::dma_retired)) return kIOReturnError;
        }
        if (ledger.phase != Ledger::dma_retired || !ready_cold_regions_empty(state))
            return kIOReturnError;
        {
            ReadyProviderLock provider(&driver->ivars->stateLock);
            const auto owner = ready_owner_status_locked(client, driver, key.generation);
            if (owner != kIOReturnSuccess) return owner;
            if (!ready_post_reset_quiet(pci, ledger.pci_registry, ledger.pci_identifier))
                return kIOReturnNotReady;
            if (group) {
                auto *audio = state->warmAudioOwner;
                if (!audio || !audio->ivars) return kIOReturnNotAttached;
                {
                    ReadyProviderLock companion(&audio->ivars->stateLock);
                    auto *ap = vfio_user_get_pci_device(audio);
                    if (audio->ivars->stopping || !audio->ivars->resetCompanionInUse)
                        return kIOReturnNotAttached;
                    if (!ready_post_reset_quiet(ap, scope.audio, ledger.companion_pci_identifier))
                        return kIOReturnNotReady;
                    ap->Close(audio, 0);
                    audio->ivars->resetCompanionInUse = false;
                    state->warmAudioOwner = nullptr;
                }
                audio->release();
            }
            ready_close_claim_locked(client, driver, pci,
                                     !perst_probe || ledger.perst_outcome == EXTGPU_VEGA_PERST_RESET);
            if (!ledger.advance(Ledger::closed)) return kIOReturnError;
        }
    }
    memcpy(arguments->scalarOutput, in, request_words * sizeof(*in));
    arguments->scalarOutput[request_words] = EXTGPU_WARM_VM_CLOSED;
    if (perst_probe) arguments->scalarOutput[request_words + 1] = ledger.perst_outcome;
    arguments->scalarOutputCount = reply_words;
    return kIOReturnSuccess;
}

static kern_return_t ready_group_release_receipt(const extgpu_driver::WarmVMRelease &ledger,
                                                IOUserClientMethodArguments *args, uint32_t method)
{
    if (!args->scalarInput || args->scalarInputCount != EXTGPU_WARM_VEGA_REQUEST_WORDS ||
        !args->scalarOutput || args->scalarOutputCount != EXTGPU_WARM_VEGA_REPLY_WORDS)
        return kIOReturnBadArgument;
    const auto *in = args->scalarInput;
    const extgpu_driver::WarmVMKey key = {in[0], in[1], in[2]};
    const extgpu_driver::PCIResetScope scope = {in[3], in[4], in[5]};
    if (ledger.phase != extgpu_driver::WarmVMRelease::closed) return kIOReturnNotReady;
    if (!ledger.has_group_release_receipt(key, scope, method)) return kIOReturnNotPermitted;
    memcpy(args->scalarOutput, in, EXTGPU_WARM_VEGA_REQUEST_WORDS * sizeof(*in));
    args->scalarOutput[EXTGPU_WARM_VEGA_REQUEST_WORDS] = EXTGPU_WARM_VM_CLOSED;
    return kIOReturnSuccess;
}

kern_return_t
VFIOUserPCIDriverUserClient::ExternalMethod(uint64_t selector,
                                            IOUserClientMethodArguments *arguments,
                                            const IOUserClientMethodDispatch *,
                                            OSObject *, void *)
{
    if (!arguments) return kIOReturnBadArgument;

    if (ivars == nullptr) return kIOReturnNotAttached;
    ReadyProviderLock operation(&ivars->operationLock);
    if (selector != EXTGPU_SELECTOR_MMIO_READ && selector != EXTGPU_SELECTOR_MMIO_WRITE &&
        selector != EXTGPU_SELECTOR_MMIO_WINDOW && selector != EXTGPU_SELECTOR_BAR_WORDS &&
        selector != EXTGPU_SELECTOR_MMIO_RING_KICK)
        ivars->mmioMemoryEnable = {};
    if (selector == EXTGPU_SELECTOR_VEGA_RELEASE_RECEIPT)
        return ready_group_release_receipt(ivars->warmVMRelease, arguments, EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA);
    if (selector == EXTGPU_SELECTOR_RTX3090_RELEASE_RECEIPT)
        return ready_group_release_receipt(ivars->warmVMRelease, arguments, EXTGPU_SELECTOR_WARM_RELEASE_VM_RTX3090);
    if (selector == EXTGPU_SELECTOR_POLARIS_RELEASE_RECEIPT)
        return ready_group_release_receipt(ivars->warmVMRelease, arguments, EXTGPU_SELECTOR_WARM_RELEASE_VM_POLARIS);
    if (selector == EXTGPU_SELECTOR_WARM_RELEASE_VM_FLR || selector == EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA ||
        selector == EXTGPU_SELECTOR_WARM_RELEASE_VM_RTX3090 || selector == EXTGPU_SELECTOR_WARM_RELEASE_VM_POLARIS ||
        selector == EXTGPU_SELECTOR_PROBE_VEGA_PERST)
        return ready_warm_release_vm(this, arguments, static_cast<uint32_t>(selector));
    if (ivars->warmVMRelease.phase != extgpu_driver::WarmVMRelease::idle)
        return kIOReturnNotPermitted;
    if (selector == EXTGPU_SELECTOR_COLD_RETIRE || selector == EXTGPU_SELECTOR_COLD_FINISH)
        return kIOReturnNotPermitted; // Bound independent broker only.
    if (selector == EXTGPU_SELECTOR_COLD_BEGIN) {
        if (!arguments->scalarInput || arguments->scalarInputCount != EXTGPU_COLD_BEGIN_INPUT_COUNT ||
            !arguments->scalarOutput || arguments->scalarOutputCount != EXTGPU_COLD_BEGIN_OUTPUT_COUNT)
            return kIOReturnBadArgument;
        return ready_cold_begin(this, arguments);
    }
    if (selector == EXTGPU_SELECTOR_PREPARE_RECOVERY_ESCROW && ivars->coldPrepared) {
        if (!arguments->scalarInput || arguments->scalarInputCount != EXTGPU_RECOVERY_ESCROW_INPUT_COUNT ||
            !arguments->scalarOutput || arguments->scalarOutputCount != EXTGPU_RECOVERY_PREPARE_OUTPUT_COUNT)
            return kIOReturnBadArgument;
        uint64_t brokerID = 0;
        kern_return_t ret = ready_recovery_prepared_reply(this, arguments->scalarInput, arguments->scalarInput + 5, &brokerID);
        if (ret != kIOReturnSuccess) return ret;
        ready_cold_write_key(arguments->scalarOutput, ivars->coldLedger.key);
        arguments->scalarOutput[5] = brokerID;
        arguments->scalarOutputCount = EXTGPU_RECOVERY_PREPARE_OUTPUT_COUNT;
        return kIOReturnSuccess;
    }
    if (ivars->coldPrepared || ivars->coldFrozen) return kIOReturnNotPermitted;
    IOService *driverService = GetProvider();
    if (driverService == nullptr) {
        return kIOReturnNotAttached;
    }

    auto *ownedDriver = OSDynamicCast(VFIOUserPCIDriver, driverService);
    if (ownedDriver == nullptr || ownedDriver->ivars == nullptr) return kIOReturnNotAttached;
    if (selector == 0x7e04) {
        if (!ready_hypervisor_client_entitled(this) || !ivars->claimed ||
            !ivars->authProbeUsed || ivars->authFullVM || !ivars->authProbeBarIssued ||
            !ivars->authProbeMSEEnabled || arguments->scalarInputCount != 0 ||
            !arguments->scalarOutput || arguments->scalarOutputCount != 1)
            return kIOReturnNotPermitted;
        ReadyProviderLock lock(&ownedDriver->ivars->stateLock);
        const auto owner = ready_owner_status_locked(this, ownedDriver, ivars->authProbeChallenge[2]);
        if (owner != kIOReturnSuccess) return owner;
        auto *pci = vfio_user_get_pci_device(ownedDriver);
        if (!pci) return kIOReturnNotAttached;
        // This optional probe reads the Vega 10 GRBM_STATUS register.
        extgpu_driver::ResetFunction identity{};
        if (!ready_reset_member(ownedDriver, &identity) || identity.vendor != 0x1002 ||
            identity.device != 0x687f || identity.class_code != 0x030000 || identity.function != 0)
            return kIOReturnUnsupported;
        uint8_t memoryIndex = UINT8_MAX, attributes = 0;
        uint64_t length = 0;
        const auto barStatus = pci->GetBARInfo(5, &memoryIndex, &length, &attributes);
        if (barStatus != kIOReturnSuccess || memoryIndex == UINT8_MAX ||
            length != (UINT64_C(512) << 10) || 0x8010u > length - 4)
            return kIOReturnNotReady;
        uint32_t value = UINT32_MAX;
        pci->MemoryRead32(memoryIndex, 0x8010, &value);
        arguments->scalarOutput[0] = value;
        return kIOReturnSuccess;
    }
    if (selector == 0x7e03) {
        if (!ready_hypervisor_client_entitled(this) || !ivars->claimed ||
            !ivars->authProbeUsed || ivars->authFullVM || !ivars->authProbeBarIssued ||
            !arguments->scalarInput || arguments->scalarInputCount != 1 ||
            arguments->scalarOutputCount != 0 || arguments->scalarInput[0] > 1)
            return kIOReturnNotPermitted;
        ReadyProviderLock lock(&ownedDriver->ivars->stateLock);
        const auto owner = ready_owner_status_locked(this, ownedDriver, ivars->authProbeChallenge[2]);
        if (owner != kIOReturnSuccess) return owner;
        if (ivars->hardwareMutated || ivars->everPreparedDMA ||
            !ready_cold_regions_empty(ivars) || !ready_cold_irq_empty(ivars))
            return kIOReturnNotPermitted;
        auto *pci = vfio_user_get_pci_device(ownedDriver);
        if (!pci) return kIOReturnNotAttached;
        uint16_t command = UINT16_MAX;
        pci->ConfigurationRead16(4, &command);
        if (command == UINT16_MAX) return kIOReturnNotReady;
        if (arguments->scalarInput[0] == 1) {
            if (ivars->authProbeMSEEnabled || (command & 6)) return kIOReturnNotPermitted;
            ivars->authProbeOriginalCommand = command;
            pci->ConfigurationWrite16(4, command | UINT16_C(2));
            uint16_t confirmed = UINT16_MAX;
            pci->ConfigurationRead16(4, &confirmed);
            if (confirmed != (command | UINT16_C(2))) return kIOReturnNotReady;
            ivars->authProbeMSEEnabled = true;
            return kIOReturnSuccess;
        }
        if (!ivars->authProbeMSEEnabled ||
            command != (ivars->authProbeOriginalCommand | UINT16_C(2)))
            return kIOReturnNotReady;
        pci->ConfigurationWrite16(4, ivars->authProbeOriginalCommand);
        uint16_t confirmed = UINT16_MAX;
        pci->ConfigurationRead16(4, &confirmed);
        if (confirmed != ivars->authProbeOriginalCommand) return kIOReturnNotReady;
        ivars->authProbeMSEEnabled = false;
        return kIOReturnSuccess;
    }
    // Isolated read-only, stopped-VM BAR probe. The signer owns the
    // probe connection in its process and never exports or runs the VM.
    if (selector == 0x7e00 || selector == 0x7e01 || selector == 0x7e02 || selector == 0x7e10 || selector == 0x7e11) {
        if (!ready_hypervisor_client_entitled(this) ||
            (ivars->claimed && selector != 0x7e02 && selector != 0x7e11))
            return kIOReturnNotPermitted;
        ReadyProviderLock provider(&ownedDriver->ivars->stateLock);
        if (ownedDriver->ivars->stopping) return kIOReturnNotAttached;
        if (selector == 0x7e00) {
            if (arguments->scalarInputCount != 0 || !arguments->scalarOutput ||
                arguments->scalarOutputCount != 3)
                return kIOReturnBadArgument;
            if (ivars->barMappingIssued || ivars->everPreparedDMA ||
                (ivars->authProbeIssued && ivars->authProbeChallenge[2] != ownedDriver->ivars->generation))
                return kIOReturnNotPermitted;
            if (!ivars->authProbeIssued) {
                arc4random_buf(ivars->authProbeChallenge, 2 * sizeof(uint64_t));
                ivars->authProbeChallenge[2] = ownedDriver->ivars->generation;
            }
            memcpy(arguments->scalarOutput, ivars->authProbeChallenge, sizeof ivars->authProbeChallenge);
            ivars->authProbeIssued = true;
            return kIOReturnSuccess;
        }
        if (arguments->scalarInputCount != 8 || !arguments->scalarInput ||
            arguments->scalarOutputCount != 0 || !ivars->authProbeIssued ||
            (selector == 0x7e01 ? ivars->authProbeUsed :
             selector == 0x7e10 ? (ivars->authProbeUsed && !ivars->authFullVM) :
             selector == 0x7e11 ? (!ivars->authFullVM || !ivars->barMappingIssued) :
             (!ivars->authProbeUsed || ivars->authFullVM)) ||
            ivars->authProbeChallenge[2] != ownedDriver->ivars->generation)
            return kIOReturnNotPermitted;
        const uint64_t domain = selector == 0x7e11 ? UINT64_C(0x4f504155544834) :
            selector == 0x7e10 ? UINT64_C(0x4f504155544833) :
            selector == 0x7e01 ? UINT64_C(0x4f504155544831) : UINT64_C(0x4f504155544832);
        const uint64_t words[] = {domain,
            ivars->authProbeChallenge[0],
            ivars->authProbeChallenge[1], ivars->authProbeChallenge[2]};
        if (!openpci_direct_bar_key_configured) return kIOReturnUnsupported;
        const auto &key = openpci_direct_bar_public_key;
        if (cced25519_verify(ccsha512_di(), sizeof words, words,
                reinterpret_cast<const uint8_t *>(arguments->scalarInput), key) != 0)
            return kIOReturnNotPermitted;
        if (selector == 0x7e01 || selector == 0x7e10) {
            ivars->authProbeUsed = true;
            ivars->authFullVM = selector == 0x7e10;
            return kIOReturnSuccess;
        }
        if (selector == 0x7e11) {
            const auto owner = ready_owner_status_locked(this, ownedDriver,
                                                         ivars->authProbeChallenge[2]);
            if (owner != kIOReturnSuccess) return owner;
            auto *pci = vfio_user_get_pci_device(ownedDriver);
            if (!pci) return kIOReturnNotAttached;
            uint16_t command = UINT16_MAX;
            pci->ConfigurationRead16(4, &command);
            if (command == UINT16_MAX || (command & 4)) return kIOReturnNotReady;
            // The signed VMM owns the sole host view and attests that every
            // vCPU has stopped and every HVF/host BAR view was unmapped.
            // A lost reply may repeat the same authenticated receipt.
            ivars->authBarRetired = true;
            return kIOReturnSuccess;
        }
        if (!ivars->authProbeBarIssued || !ivars->barMappingIssued ||
            ivars->authProbeMSEEnabled || ivars->hardwareMutated ||
            ivars->everPreparedDMA || !ready_cold_regions_empty(ivars) || !ready_cold_irq_empty(ivars))
            return kIOReturnNotPermitted;
        const auto owner = ready_owner_status_locked(this, ownedDriver, ivars->authProbeChallenge[2]);
        if (owner != kIOReturnSuccess) return owner;
        auto *pci = vfio_user_get_pci_device(ownedDriver);
        if (!pci) return kIOReturnNotAttached;
        uint16_t command = UINT16_MAX;
        pci->ConfigurationRead16(4, &command);
        if (command == UINT16_MAX || (command & 6)) return kIOReturnNotReady;
        // Only this probe may close after an authenticated retirement receipt.
        // The ordinary BAR export and release selectors retain their guards.
        ready_close_claim_locked(this, ownedDriver, pci, false);
        return kIOReturnSuccess;
    }
    if (ivars->authProbeUsed && !ivars->authFullVM &&
        selector != EXTGPU_SELECTOR_QUERY_PROTOCOL &&
        selector != EXTGPU_SELECTOR_CLAIM &&
        (selector != EXTGPU_SELECTOR_RELEASE_OWNER || ivars->authProbeBarIssued))
        return kIOReturnNotPermitted;
    if (selector == EXTGPU_SELECTOR_MMIO_READ || selector == EXTGPU_SELECTOR_MMIO_WRITE)
        return dispatch_scalar_mmio(this, ownedDriver, arguments,
                                    selector == EXTGPU_SELECTOR_MMIO_READ);
    if (selector == EXTGPU_SELECTOR_MMIO_WINDOW)
        return service_mmio_window(this, ownedDriver, arguments);
    if (selector == EXTGPU_SELECTOR_MMIO_RING_KICK)
        return kick_mmio_ring(this, ownedDriver, arguments);
    if (selector == EXTGPU_SELECTOR_QUERY_PROTOCOL) {
        if (arguments->scalarInputCount != EXTGPU_QUERY_INPUT_COUNT || arguments->scalarInput == nullptr ||
            arguments->scalarOutputCount < EXTGPU_QUERY_OUTPUT_COUNT || arguments->scalarOutput == nullptr) return kIOReturnBadArgument;
        ivars->protocolNegotiated = false;
        if (arguments->scalarInput[0] != EXTGPU_PROTOCOL_MAJOR || arguments->scalarInput[1] > EXTGPU_PROTOCOL_MINOR ||
            arguments->scalarInput[2] != EXTGPU_PROTOCOL_FINGERPRINT) return kIOReturnUnsupported;
        ReadyProviderLock state(&ownedDriver->ivars->stateLock);
        if (ownedDriver->ivars->stopping) return kIOReturnNotAttached;
        uint64_t features = EXTGPU_CAP_CONFIG_READ | EXTGPU_CAP_CONFIG_WRITE | EXTGPU_CAP_BAR |
            EXTGPU_CAP_DMA_ALLOCATE | EXTGPU_CAP_DMA_REGISTER | EXTGPU_CAP_IRQ_ASYNC | EXTGPU_CAP_FLR_API |
            EXTGPU_CAP_EXCLUSIVE_CLAIM | EXTGPU_CAP_GENERATION | EXTGPU_CAP_SAFE_RELEASE | EXTGPU_CAP_CONFIGURABLE_DMA_MASK |
            EXTGPU_CAP_IRQ_RETIRE | EXTGPU_CAP_COLD_REMOVAL | EXTGPU_CAP_RECOVERY_BROKER |
            EXTGPU_CAP_WARM_VM_FLR_RELEASE | EXTGPU_CAP_WARM_VM_VEGA_RELEASE | EXTGPU_CAP_VEGA_PERST_PROBE |
            EXTGPU_CAP_VM_FLR_PREFLIGHT | EXTGPU_CAP_RESTORE_VEGA_BARS | EXTGPU_CAP_BAR_WORDS |
            EXTGPU_CAP_VEGA_RELEASE_RECEIPT | EXTGPU_CAP_WARM_VM_STARTUP_RELEASE |
            EXTGPU_CAP_WARM_VM_RTX3090_RELEASE | EXTGPU_CAP_WARM_VM_POLARIS_RELEASE |
            EXTGPU_CAP_PREFETCHABLE_BAR_WORDS | EXTGPU_CAP_WARM_VM_NO_DMA_RELEASE | EXTGPU_CAP_MMIO_WINDOW |
            EXTGPU_CAP_IRQ_DRAIN_AND_REARM | EXTGPU_CAP_IRQ_SHARED_NOTIFICATION | EXTGPU_CAP_CONFIGURABLE_DMA_BUDGET |
            EXTGPU_CAP_VM_DMA_SEGMENTS | EXTGPU_CAP_VM_DYNAMIC_DMA | EXTGPU_CAP_MMIO_WINDOW_WORD_WRITES |
            EXTGPU_CAP_MMIO_RING | EXTGPU_CAP_MMIO_RING_POLL | EXTGPU_CAP_MMIO_RING_ADAPTIVE |
            EXTGPU_CAP_MMIO_RING_IRQ_ARM | EXTGPU_CAP_MMIO_RING_EVENT |
            EXTGPU_CAP_MMIO_RING_GROUPED_STORES | EXTGPU_CAP_VM_GROUP_PREFLIGHT;
        if (ownedDriver->ivars->quarantined || ivars->dmaRetirementFailuresPending != 0 || ivars->irqCancelFailure != kIOReturnSuccess) features |= EXTGPU_STATE_QUARANTINED;
        const uint64_t result[8] = {EXTGPU_PROTOCOL_MAJOR, EXTGPU_PROTOCOL_MINOR, ownedDriver->ivars->generation,
            features, EXTGPU_MAX_DMA_REGIONS, ivars->dmaByteBudget, EXTGPU_MAX_RETURNED_SEGMENTS, EXTGPU_PROTOCOL_FINGERPRINT};
        memcpy(arguments->scalarOutput, result, sizeof(result)); arguments->scalarOutputCount = 8;
        ivars->protocolNegotiated = true; ivars->negotiatedGeneration = ownedDriver->ivars->generation;
        return kIOReturnSuccess;
    }
    if (!ivars->protocolNegotiated || ivars->negotiatedGeneration != ownedDriver->ivars->generation) return kIOReturnNotPermitted;
    if (selector == EXTGPU_SELECTOR_VM_GROUP_PREFLIGHT)
        return ready_group_preflight(this, ownedDriver, arguments);
    if (selector == EXTGPU_SELECTOR_GET_RECOVERY_ENDPOINT) {
        if (!arguments->scalarInput || arguments->scalarInputCount != EXTGPU_RECOVERY_ENDPOINT_INPUT_COUNT ||
            !arguments->scalarOutput || arguments->scalarOutputCount != EXTGPU_RECOVERY_ENDPOINT_OUTPUT_COUNT)
            return kIOReturnBadArgument;
        ReadyProviderLock state(&ownedDriver->ivars->stateLock);
        if (ownedDriver->ivars->stopping || ownedDriver->ivars->quarantined ||
            arguments->scalarInput[0] != ownedDriver->ivars->generation) return kIOReturnNotReady;
        uint64_t brokerID = 0;
        kern_return_t ret = ready_recovery_endpoint(&brokerID);
        if (ret != kIOReturnSuccess) return ret;
        arguments->scalarOutput[0] = brokerID;
        arguments->scalarOutput[1] = EXTGPU_PROTOCOL_MAJOR;
        arguments->scalarOutput[2] = EXTGPU_PROTOCOL_MINOR;
        arguments->scalarOutputCount = EXTGPU_RECOVERY_ENDPOINT_OUTPUT_COUNT;
        return kIOReturnSuccess;
    }
    if (selector != EXTGPU_SELECTOR_GET_IDENTITY && selector != EXTGPU_SELECTOR_GET_BAR_INFO && selector != EXTGPU_SELECTOR_CLAIM) {
        kern_return_t owner = ready_owner_status(this);
        if (owner != kIOReturnSuccess) return owner;
    }
    if (selector == EXTGPU_SELECTOR_VM_FLR_PREFLIGHT)
        return ready_vm_flr_preflight(this, ownedDriver, arguments);
    if (selector == EXTGPU_SELECTOR_SET_DMA_BUDGET) {
        const auto *request = arguments->scalarInput;
        if (!request || arguments->scalarInputCount != EXTGPU_DMA_BUDGET_WORDS || !request[1] ||
            !arguments->scalarOutput || arguments->scalarOutputCount < EXTGPU_DMA_BUDGET_WORDS)
            return kIOReturnBadArgument;
        ReadyProviderLock lock(&ownedDriver->ivars->stateLock);
        const auto owner = ready_owner_status_locked(this, ownedDriver, request[0]);
        if (owner != kIOReturnSuccess) return owner;
        if (ivars->dmaRequested || ivars->activeDMARegionCount || ivars->dmaBuffer)
            return kIOReturnStillOpen;
        ivars->dmaByteBudget = request[1];
        memcpy(arguments->scalarOutput, request, EXTGPU_DMA_BUDGET_WORDS * sizeof(*request));
        arguments->scalarOutputCount = EXTGPU_DMA_BUDGET_WORDS;
        return kIOReturnSuccess;
    }
    if (selector == EXTGPU_SELECTOR_ENABLE_VM_DYNAMIC_DMA) {
        const auto *in = arguments->scalarInput;
        auto *out = arguments->scalarOutput;
        if (!in || arguments->scalarInputCount != 3 || !out || arguments->scalarOutputCount < 3 || !in[2])
            return kIOReturnBadArgument;
        if (!ready_hypervisor_client_entitled(this)) return kIOReturnNotPermitted;
        const extgpu_driver::WarmVMKey key{in[0], in[1], in[2]};
        ReadyProviderLock lock(&ownedDriver->ivars->stateLock);
        const auto owner = ready_owner_status_locked(this, ownedDriver, key.generation);
        if (owner != kIOReturnSuccess) return owner;
        if (!ready_vm_window_matches(ivars, key, true) || ivars->hardwareMutated || !ready_cold_irq_empty(ivars))
            return kIOReturnStillOpen;
        ivars->dynamicDMAWindow = key;
        memcpy(out, in, 3 * sizeof(*in));
        arguments->scalarOutputCount = 3;
        return kIOReturnSuccess;
    }
    if (selector == EXTGPU_SELECTOR_GET_VM_DMA_SEGMENT) {
        const auto *request = arguments->scalarInput;
        auto *reply = arguments->scalarOutput;
        if (!request || arguments->scalarInputCount != 3 || !reply || arguments->scalarOutputCount < 2 ||
            request[0] != ivars->negotiatedGeneration || request[2] >= ivars->vmDMASegmentCount)
            return kIOReturnBadArgument;
        const auto *region = find_dma_region(*ivars, request[1]);
        if (!region || !region->vmSegmented || !region->fullCoverage || ivars->activeDMARegionCount != 1)
            return kIOReturnNotFound;
        const auto &segment = ivars->vmDMASegments[request[2]];
        reply[0] = segment.address;
        reply[1] = segment.length;
        arguments->scalarOutputCount = 2;
        return kIOReturnSuccess;
    }
    if (selector == EXTGPU_SELECTOR_RELEASE_OWNER) {
        if (!arguments->scalarInput || arguments->scalarInputCount != 1) return kIOReturnBadArgument;
        return ready_release_owner(this, ownedDriver, arguments->scalarInput[0]);
    }
    if (selector == EXTGPU_SELECTOR_RESTORE_VEGA_BARS)
        return ready_restore_vega_bars(this, ownedDriver, arguments);
    if (selector == EXTGPU_SELECTOR_BAR_WORDS) {
        ivars->startupDMARequest = {};
        return vfio_user_bar_words(this, ownedDriver, arguments);
    }
    if (selector == EXTGPU_SELECTOR_PREPARE_RECOVERY_ESCROW) {
        if (!arguments->scalarInput || arguments->scalarInputCount != EXTGPU_RECOVERY_ESCROW_INPUT_COUNT ||
            !arguments->scalarOutput || arguments->scalarOutputCount != EXTGPU_RECOVERY_PREPARE_OUTPUT_COUNT)
            return kIOReturnBadArgument;
        return ready_cold_prepare(this, ownedDriver, arguments);
    }
    const bool segmented = selector == EXTGPU_SELECTOR_REGISTER_VM_DMA;
    const bool dynamic = selector == EXTGPU_SELECTOR_REGISTER_VM_DYNAMIC_DMA;
    if (dynamic && !ivars->dynamicDMAWindow.bytes) return kIOReturnNotPermitted;
    if (!dynamic && ivars->dynamicDMAWindow.bytes && (segmented || selector == EXTGPU_SELECTOR_REGISTER_DMA_REGION))
        return kIOReturnNotPermitted;
    if (segmented) {
        const auto *request = arguments->scalarInput;
        if (!request || arguments->scalarInputCount != 3 || !request[1] || !request[2] ||
            ((request[0] | request[1] | request[2]) & UINT64_C(0x3fff)) ||
            !arguments->scalarOutput || arguments->scalarOutputCount < 3)
            return kIOReturnBadArgument;
        if (ivars->dmaRequested || !ivars->claimPristine || ivars->hardwareMutated ||
            ivars->barMappingIssued || !ready_cold_irq_empty(ivars) || !ready_cold_regions_empty(ivars))
            return kIOReturnStillOpen;
    }
    if (ivars->claimed && !extgpu_driver::preserves_pristine_claim(
            selector, arguments->scalarInput, arguments->scalarInputCount)) {
        /* Remember the first DMA request before DriverKit may prepare any
         * pages. Ordinary rollback may remove the region before the reply. */
        if ((selector == EXTGPU_SELECTOR_REGISTER_DMA_REGION || segmented) && ivars->claimPristine &&
            !ivars->hardwareMutated && !ivars->everPreparedDMA && !ivars->barMappingIssued &&
            arguments->scalarInput && arguments->scalarInputCount == 3 &&
            ready_cold_irq_empty(ivars) && ready_cold_regions_empty(ivars)) {
            ivars->startupDMARequest = {ivars->negotiatedGeneration,
                                        arguments->scalarInput[0], arguments->scalarInput[2]};
        } else if (selector != EXTGPU_SELECTOR_UNREGISTER_DMA_REGION) {
            ivars->startupDMARequest = {};
        }
        ivars->claimPristine = false;
    }
    if (selector == EXTGPU_SELECTOR_CONFIG_WRITE || selector == EXTGPU_SELECTOR_SETUP_INTERRUPTS ||
        selector == EXTGPU_SELECTOR_RESET_DEVICE) ivars->hardwareMutated = true;
    if (selector == EXTGPU_SELECTOR_REGISTER_DMA_REGION || segmented || dynamic ||
        selector == EXTGPU_SELECTOR_ALLOCATE_DMA_BUFFER)
        ivars->dmaRequested = true;

    if (selector == EXTGPU_SELECTOR_CONFIG_READ) {
        if (!arguments->scalarInput || arguments->scalarInputCount < 2 ||
            !arguments->scalarOutput || arguments->scalarOutputCount < 1)
            return kIOReturnBadArgument;
        const auto ret = vfio_user_config_read(this, ownedDriver, arguments->scalarInput[0],
                                               arguments->scalarInput[1], arguments->scalarOutput);
        if (ret == kIOReturnSuccess) arguments->scalarOutputCount = 1;
        return ret;
    }
    if (selector == EXTGPU_SELECTOR_CONFIG_WRITE) {
        if (!arguments->scalarInput || arguments->scalarInputCount < 3) return kIOReturnBadArgument;
        return vfio_user_config_write(this, ownedDriver, arguments->scalarInput[0],
                                      arguments->scalarInput[1], arguments->scalarInput[2]);
    }

    const auto *in = arguments->scalarInput;
    auto *out = arguments->scalarOutput;
    const auto inCount = in ? arguments->scalarInputCount : 0;
    const auto outCount = out ? arguments->scalarOutputCount : 0;

    switch (selector) {
    case EXTGPU_SELECTOR_GET_IDENTITY: {
        if (outCount < 6) return kIOReturnBadArgument;
        ReadyProviderLock lock(&ownedDriver->ivars->stateLock);
        if (ownedDriver->ivars->stopping) return kIOReturnNotAttached;
        VFIOUserPCIDeviceIdentity identity{};
        const auto status = vfio_user_read_identity(ownedDriver, &identity);
        if (status != kIOReturnSuccess) return status;
        const uint64_t fields[] = {identity.bus, identity.device, identity.function,
                                   identity.vendorID, identity.deviceID, identity.classCode};
        memcpy(out, fields, sizeof(fields));
        arguments->scalarOutputCount = 6;
        return kIOReturnSuccess;
    }
    case EXTGPU_SELECTOR_CLAIM:
        return vfio_user_claim_exclusive(this, ownedDriver);
    case EXTGPU_SELECTOR_SET_DMA_MASK:
        if (inCount < 1 || arguments->scalarInputCount != 1 || in[0] < 32 || in[0] > 64)
            return kIOReturnBadArgument;
        if (ivars->everPreparedDMA || ivars->dmaBuffer || ivars->activeDMARegionCount)
            return kIOReturnStillOpen;
        ivars->dmaAddressBits = static_cast<uint32_t>(in[0]);
        return kIOReturnSuccess;
    case EXTGPU_SELECTOR_ALLOCATE_DMA_BUFFER: {
        if (inCount < 2 || outCount < 6) return kIOReturnBadArgument;
        VFIOUserPCIDeviceIdentity identity{};
        auto status = vfio_user_read_identity(ownedDriver, &identity);
        if (status != kIOReturnSuccess) return status;
        status = vfio_user_allocate_dma_buffer(this, in[0], in[1]);
        if (status != kIOReturnSuccess) return status;
        const uint64_t fields[] = {ivars->dmaBufferSize, ivars->dmaFlags, 1,
                                   ivars->dmaSegment.address, ivars->dmaSegment.length,
                                   identity.classCode};
        memcpy(out, fields, sizeof(fields));
        arguments->scalarOutputCount = 6;
        return kIOReturnSuccess;
    }
    case EXTGPU_SELECTOR_FREE_DMA_BUFFER:
        return vfio_user_release_dma_buffer(this);
    case EXTGPU_SELECTOR_REGISTER_DMA_REGION:
    case EXTGPU_SELECTOR_REGISTER_VM_DMA:
    case EXTGPU_SELECTOR_REGISTER_VM_DYNAMIC_DMA: {
        if (inCount < 3 || outCount < 3) return kIOReturnBadArgument;
        if (dynamic && ((in[0] | in[1] | in[2]) & UINT64_C(0x3fff))) return kIOReturnBadArgument;
        IOAddressSegment segment{};
        const auto status = vfio_user_register_client_dma_region(this, in[0], in[1], in[2], &segment, segmented, dynamic);
        if (status != kIOReturnSuccess) return status;
        const uint64_t fields[] = {segmented ? ivars->vmDMASegmentCount : 1u, segment.address,
                                   segmented ? in[2] : segment.length};
        memcpy(out, fields, sizeof(fields));
        arguments->scalarOutputCount = 3;
        return kIOReturnSuccess;
    }
    case EXTGPU_SELECTOR_UNREGISTER_DMA_REGION:
        if (inCount && ivars->dynamicDMAWindow.bytes && in[0] == ivars->dynamicDMAWindow.registration)
            return kIOReturnNotPermitted;
        return inCount ? vfio_user_unregister_client_dma_region(this, in[0]) :
            kIOReturnBadArgument;
    case EXTGPU_SELECTOR_PROBE_DMA_REGION: {
        if (inCount < 2 || outCount < 1) return kIOReturnBadArgument;
        const auto status = vfio_user_probe_client_dma_region(this, in[0], in[1], out);
        if (status == kIOReturnSuccess) arguments->scalarOutputCount = 1;
        return status;
    }
    case EXTGPU_SELECTOR_GET_BAR_INFO: {
        if (inCount < 1 || outCount < 3 || in[0] >= 6) return kIOReturnBadArgument;
        ReadyProviderLock lock(&ownedDriver->ivars->stateLock);
        if (ownedDriver->ivars->stopping) return kIOReturnNotAttached;
        auto *pci = vfio_user_get_pci_device(ownedDriver);
        if (!pci) return kIOReturnUnsupported;
        uint8_t index = 0, attributes = 0;
        uint64_t size = 0;
        const auto status = pci->GetBARInfo(static_cast<uint8_t>(in[0]), &index, &size, &attributes);
        if (status != kIOReturnSuccess) return status;
        const uint64_t fields[] = {index, size, attributes};
        memcpy(out, fields, sizeof(fields));
        arguments->scalarOutputCount = 3;
        return kIOReturnSuccess;
    }
    case EXTGPU_SELECTOR_SETUP_INTERRUPTS: {
        if (outCount < 1) return kIOReturnBadArgument;
        const auto status = vfio_user_setup_interrupts(this, ownedDriver);
        if (status == kIOReturnSuccess) {
            *out = ivars->numInterrupts;
            arguments->scalarOutputCount = 1;
        }
        return status;
    }
    case EXTGPU_SELECTOR_RETIRE_INTERRUPTS:
        return arguments->scalarInputCount == 0 && arguments->scalarOutputCount == 0 ?
            ready_retire_interrupts(this) : kIOReturnBadArgument;
    case EXTGPU_SELECTOR_CHECK_INTERRUPT:
    case EXTGPU_SELECTOR_SET_IRQ_MASK:
    case EXTGPU_SELECTOR_WAIT_INTERRUPT: {
        const bool check = selector == EXTGPU_SELECTOR_CHECK_INTERRUPT;
        const bool mask = selector == EXTGPU_SELECTOR_SET_IRQ_MASK;
        if ((check && outCount < EXTGPU_IRQ_BITMAP_WORDS) ||
            (mask && inCount < EXTGPU_IRQ_BITMAP_WORDS) ||
            (!check && !mask && !arguments->completion))
            return kIOReturnBadArgument;

        ReadyProviderLock irqLock(&ivars->irqLock);
        if (!ivars->interruptsSetUp) return kIOReturnNotReady;
        if (check && ivars->irqPersistent) {
            openpci_irq_drain_mailbox(ivars->irqMailbox, out);
            arguments->scalarOutputCount = EXTGPU_IRQ_BITMAP_WORDS;
            return kIOReturnSuccess;
        }
        if (check || mask) {
            for (unsigned i = 0; i < EXTGPU_IRQ_BITMAP_WORDS; ++i) {
                if (check)
                    out[i] = __atomic_exchange_n(&ivars->irqPending[i], 0, __ATOMIC_ACQ_REL);
                else
                    __atomic_store_n(&ivars->irqEnabled[i], in[i], __ATOMIC_RELEASE);
            }
            if (check) arguments->scalarOutputCount = EXTGPU_IRQ_BITMAP_WORDS;
            return kIOReturnSuccess;
        }

        // Serialize arming and the pending-bit check with the interrupt handler.
        if (ivars->irqPersistent) return kIOReturnBusy;
        auto *completion = arguments->completion;
        completion->retain();
        OSSafeReleaseNULL(ivars->pendingInterruptNotify);
        ivars->pendingInterruptNotify = completion;
        for (unsigned i = 0; i < EXTGPU_IRQ_BITMAP_WORDS; ++i) {
            if (!__atomic_load_n(&ivars->irqPending[i], __ATOMIC_ACQUIRE)) continue;
            ivars->pendingInterruptNotify = nullptr;
            AsyncCompletion(completion, kIOReturnSuccess, nullptr, 0);
            completion->release();
            break;
        }
        return kIOReturnSuccess;
    }
    case EXTGPU_SELECTOR_IRQ_DRAIN_AND_REARM: {
        if (inCount || outCount != EXTGPU_IRQ_BITMAP_WORDS || !arguments->completion)
            return kIOReturnBadArgument;

        ReadyProviderLock irqLock(&ivars->irqLock);
        if (!ivars->interruptsSetUp) return kIOReturnNotReady;
        if (ivars->pendingInterruptNotify) return kIOReturnBusy;

        // Keep the drain and installation of the next action atomic with
        // respect to DeliverVector. A later interrupt must notify the app.
        auto *completion = arguments->completion;
        completion->retain();
        for (unsigned i = 0; i < EXTGPU_IRQ_BITMAP_WORDS; ++i)
            out[i] = __atomic_exchange_n(&ivars->irqPending[i], 0, __ATOMIC_ACQ_REL);
        ivars->pendingInterruptNotify = completion;
        return kIOReturnSuccess;
    }
    case EXTGPU_SELECTOR_IRQ_SUBSCRIBE_SHARED: {
        if (inCount != 1 || outCount || !arguments->completion) return kIOReturnBadArgument;
        ReadyProviderLock irqLock(&ivars->irqLock);
        auto *mailbox = ivars->irqMailbox;
        if (!ivars->interruptsSetUp || !mailbox) return kIOReturnNotReady;
        if (ivars->irqMailboxUsed || ivars->irqStateExported || ivars->pendingInterruptNotify)
            return kIOReturnBusy;
        if (in[0] != ivars->irqMailboxGeneration || in[0] != ivars->negotiatedGeneration)
            return kIOReturnNotPermitted;

        auto *completion = arguments->completion;
        completion->retain();
        ivars->pendingInterruptNotify = completion;
        ivars->irqMailboxUsed = true;
        ivars->irqPersistent = true;
        uint64_t pending = 0;
        for (unsigned i = 0; i < EXTGPU_IRQ_BITMAP_WORDS; ++i) {
            const uint64_t bits = __atomic_exchange_n(&ivars->irqPending[i], 0, __ATOMIC_ACQ_REL);
            __atomic_store_n(&mailbox->pending[i], bits, __ATOMIC_RELEASE);
            pending |= bits;
        }
        if (pending && openpci_irq_request_notification(mailbox))
            AsyncCompletion(completion, kIOReturnSuccess, nullptr, 0);
        return kIOReturnSuccess;
    }
    case EXTGPU_SELECTOR_RESET_DEVICE:
        return vfio_user_reset_device(this, ownedDriver);
    default:
        return kIOReturnUnsupported;
    }
}

void IMPL(VFIOUserPCIDriverUserClient, DeliverVector)
{
    auto *state = ivars;
    if (!state) return;
    ReadyProviderLock irqLock(&state->irqLock);
    if (!state->interruptsSetUp) return;
    const auto *vector = static_cast<const uint32_t *>(action->GetReference());
    if (!vector || *vector >= state->numInterrupts) return;

    const unsigned word = *vector >> 6;
    const uint64_t bit = UINT64_C(1) << (*vector & 63);
    const uint64_t enabled = __atomic_load_n(&state->irqEnabled[word], __ATOMIC_ACQUIRE);
    if (!(enabled & bit)) return;
    if (state->irqPersistent) {
        if (openpci_irq_publish(state->irqMailbox, word, bit)) {
            arm_mmio_for_shared_irq(this, state);
            AsyncCompletion(state->pendingInterruptNotify, kIOReturnSuccess, nullptr, 0);
        }
        return;
    }
    __atomic_fetch_or(&state->irqPending[word], bit, __ATOMIC_RELEASE);

    if (auto *completion = state->pendingInterruptNotify) {
        state->pendingInterruptNotify = nullptr;
        AsyncCompletion(completion, kIOReturnSuccess, nullptr, 0);
        completion->release();
    }
}

void IMPL(VFIOUserPCIDriverUserClient, AsyncCompletion) {}

kern_return_t IMPL(VFIOUserPCIDriverUserClient, CopyClientMemoryForType)
{
    if (!memory || !options) return kIOReturnBadArgument;
    *memory = nullptr;
    *options = 0;

    auto *state = ivars;
    if (!state) return kIOReturnNotAttached;
    ReadyProviderLock operation(&state->operationLock);
    // Map and unmap must refer to the same CPU buffer for this user client,
    // including after ownership ends. The service selector binds I/O to the
    // original generation and still applies ordinary ownership checks.
    auto *cpuPage = type == EXTGPU_MEMORY_MMIO_WINDOW ? state->mmioWindowMemory :
        type == EXTGPU_MEMORY_IRQ_MAILBOX ? state->irqMailboxMemory :
        type == EXTGPU_MEMORY_MMIO_RING ? state->mmioRingMemory : nullptr;
    if (cpuPage) {
        cpuPage->retain();
        *memory = cpuPage;
        return kIOReturnSuccess;
    }
    if (state->coldPrepared || state->coldFrozen ||
        state->warmVMRelease.phase != extgpu_driver::WarmVMRelease::idle)
        return kIOReturnNotPermitted;
    auto *driver = OSDynamicCast(VFIOUserPCIDriver, GetProvider());
    if (!driver || !driver->ivars) return kIOReturnNotAttached;
    auto &provider = *driver->ivars;
    ReadyProviderLock lock(&provider.stateLock);
    auto status = ready_owner_status_locked(this, driver, state->negotiatedGeneration);
    if (status != kIOReturnSuccess) return status;
    if (state->authProbeUsed && !state->authFullVM) {
        if (type < 0x7e00 || type > 0x7e05 ||
            state->authProbeChallenge[2] != provider.generation ||
            state->hardwareMutated || state->everPreparedDMA ||
            !ready_cold_regions_empty(state) || !ready_cold_irq_empty(state))
            return kIOReturnNotPermitted;
        auto *pci = vfio_user_get_pci_device(driver);
        if (!pci) return kIOReturnNotAttached;
        uint16_t command = UINT16_MAX;
        pci->ConfigurationRead16(4, &command);
        if (command == UINT16_MAX || (command & 6)) return kIOReturnNotReady;

        const uint32_t bar = type - 0x7e00;
        auto &cached = provider.barDescCache[bar];
        if (!state->authProbeBarIssued) {
            if (state->barMappingIssued || !state->claimPristine || cached) return kIOReturnNotPermitted;
            uint8_t index = 0, attributes = 0;
            uint64_t length = 0;
            status = pci->GetBARInfo(bar, &index, &length, &attributes);
            if (status != kIOReturnSuccess || length == 0 || (length & 0xfff))
                return kIOReturnUnsupported;
            status = pci->_CopyDeviceMemoryWithIndex(index, &cached, this);
            if (status != kIOReturnSuccess || !cached) {
                OSSafeReleaseNULL(cached);
                return status == kIOReturnSuccess ? kIOReturnNoMemory : status;
            }
            state->claimPristine = false;
            state->barMappingIssued = true;
            state->authProbeBarIssued = true;
        }
        if (!cached) return kIOReturnNotReady;
        cached->retain();
        // HVF pins a writable host view even for guest-read-only mappings.
        // This probe has decoding and bus mastering disabled, and no vCPU.
        *options = 0;
        *memory = cached;
        return kIOReturnSuccess;
    }
    if (type == EXTGPU_MEMORY_IRQ_MAILBOX) {
        if (!ready_hypervisor_client_entitled(this)) return kIOReturnNotPermitted;
        ReadyProviderLock irqLock(&state->irqLock);
        if (!state->interruptsSetUp || state->irqStateExported || state->pendingInterruptNotify)
            return kIOReturnNotReady;
        IOBufferMemoryDescriptor *buffer = nullptr;
        constexpr auto bytes = OPENPCI_IRQ_MAILBOX_BYTES;
        status = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionOutIn, bytes, bytes, &buffer);
        IOAddressSegment range{};
        if (status == kIOReturnSuccess)
            status = buffer ? buffer->GetAddressRange(&range) : kIOReturnNoMemory;
        if (status != kIOReturnSuccess || !range.address || range.length < bytes || range.address % bytes) {
            OSSafeReleaseNULL(buffer);
            return status == kIOReturnSuccess ? kIOReturnNoMemory : status;
        }
        memset(reinterpret_cast<void *>(range.address), 0, bytes);
        state->irqMailbox = reinterpret_cast<openpci_irq_mailbox *>(range.address);
        state->irqMailboxGeneration = state->negotiatedGeneration;
        state->irqMailbox->generation = state->irqMailboxGeneration;
        state->irqMailboxMemory = buffer;
        buffer->retain();
        *memory = buffer;
        return kIOReturnSuccess;
    }
    if (type == EXTGPU_MEMORY_MMIO_WINDOW || type == EXTGPU_MEMORY_MMIO_RING) {
        if (!ready_hypervisor_client_entitled(this)) return kIOReturnNotPermitted;
        IOBufferMemoryDescriptor *buffer = nullptr;
        const auto bytes = OPENPCI_MMIO_WINDOW_BYTES;
        status = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionOutIn, bytes, bytes, &buffer);
        IOAddressSegment range{};
        if (status == kIOReturnSuccess)
            status = buffer ? buffer->GetAddressRange(&range) : kIOReturnNoMemory;
        if (status != kIOReturnSuccess || !range.address || range.length < bytes || range.address % bytes) {
            OSSafeReleaseNULL(buffer);
            return status == kIOReturnSuccess ? kIOReturnNoMemory : status;
        }
        memset(reinterpret_cast<void *>(range.address), 0, bytes);
        if (type == EXTGPU_MEMORY_MMIO_RING) {
            static_assert(sizeof(openpci_mmio_ring) <= OPENPCI_MMIO_WINDOW_BYTES);
            status = IODispatchQueue::Create("org.openpci.mmio-ring", 0, 0, &state->mmioRingQueue);
            if (status != kIOReturnSuccess) {
                buffer->release();
                return status;
            }
            state->mmioRing = reinterpret_cast<openpci_mmio_ring *>(range.address);
            state->mmioRingGeneration = state->negotiatedGeneration;
            state->mmioRing->generation = state->mmioRingGeneration;
            state->mmioRingMemory = buffer;
        } else {
            state->mmioWindow = reinterpret_cast<openpci_mmio_window *>(range.address);
            state->mmioWindowGeneration = state->negotiatedGeneration;
            state->mmioWindow->generation = state->mmioWindowGeneration;
            state->mmioWindowMemory = buffer;
        }
        buffer->retain();
        *memory = buffer;
        return kIOReturnSuccess;
    }
    if (type > EXTGPU_MEMORY_IRQ_STATE) return kIOReturnUnsupported;

    state->claimPristine = false;
    state->startupDMARequest = {};
    IOMemoryDescriptor *descriptor = nullptr;
    switch (type) {
    case EXTGPU_MEMORY_IRQ_STATE: {
        ReadyProviderLock irqLock(&state->irqLock);
        if (state->irqPersistent) return kIOReturnBusy;
        if (!state->irqSharedBuffer || (!state->interruptsSetUp && !state->irqStateExported))
            return kIOReturnNotReady;
        state->irqStateExported = true;
        descriptor = state->irqSharedBuffer;
        break;
    }
    case EXTGPU_MEMORY_DMA_BUFFER:
        if (!state->dmaBuffer || state->dmaBufferRetirementFailed) return kIOReturnNotReady;
        descriptor = state->dmaBuffer;
        break;
    default: {
        const auto bar = static_cast<uint8_t>(type - EXTGPU_MEMORY_BAR0);
        if (state->authFullVM && state->authBarRetired) return kIOReturnNotPermitted;
        auto &cached = provider.barDescCache[bar];
        if (!cached) {
            auto *pci = vfio_user_get_pci_device(driver);
            if (!pci) return kIOReturnUnsupported;
            uint8_t index = 0;
            status = pci->GetBARInfo(bar, &index);
            if (status == kIOReturnSuccess)
                status = pci->_CopyDeviceMemoryWithIndex(index, &cached, this);
            if (status != kIOReturnSuccess || !cached) {
                OSSafeReleaseNULL(cached);
                return status == kIOReturnSuccess ? kIOReturnNoMemory : status;
            }
        }
        descriptor = cached;
        state->barMappingIssued = true;
        break;
    }
    }

    descriptor->retain();
    *memory = descriptor;
    return kIOReturnSuccess;
}
