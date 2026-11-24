#pragma once
#include <stdint.h>
#include "PCIResetScope.hpp"

namespace extgpu_driver {
struct WarmVMKey {
    uint64_t generation;
    uint64_t registration;
    uint64_t bytes;

    bool matches(const WarmVMKey &other) const
    {
        return generation == other.generation && registration == other.registration && bytes == other.bytes;
    }
};

/* The operation lock serializes this ledger. Preparing pins the owners and
 * may include an audio Open. Quiescing retries bus-master disable on the
 * verified owners. An ambiguous Open or Reset cannot be issued again. */
struct WarmVMRelease {
    enum Phase : uint8_t {
        idle = 0, preparing, quiescing, draining, armed, reset_returned, reset_verified, dma_retired, closed
    };
    Phase phase;
    WarmVMKey key;
    uint64_t pci_registry;
    uint32_t pci_identifier;
    PCIResetScope group_scope;
    uint32_t selector;
    uint32_t companion_pci_identifier;
    uint64_t perst_outcome;

    bool begin(const WarmVMKey &requested, uint64_t registry,
               const PCIResetScope &scope = {}, uint32_t method = EXTGPU_SELECTOR_WARM_RELEASE_VM_FLR)
    {
        const bool probe = method == EXTGPU_SELECTOR_PROBE_VEGA_PERST;
        if (phase != idle || !requested.generation || !registry ||
            (method != EXTGPU_SELECTOR_WARM_RELEASE_VM_FLR && !scope.valid_ids()) ||
            (probe && requested.bytes) || (!requested.bytes && requested.registration))
            return false;
        key = requested;
        pci_registry = registry;
        group_scope = scope;
        selector = method;
        phase = preparing;
        return true;
    }

    bool arm_reset(bool quiet)
    {
        if (phase != draining || !quiet) return false;
        phase = armed;
        return true;
    }

    bool has_group_release_receipt(const WarmVMKey &requested, const PCIResetScope &scope, uint32_t method) const
    {
        return phase == closed &&
            (method == EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA || method == EXTGPU_SELECTOR_WARM_RELEASE_VM_RTX3090 ||
             method == EXTGPU_SELECTOR_WARM_RELEASE_VM_POLARIS) &&
            selector == method && scope.valid_ids() && key.matches(requested) && group_scope.matches(scope);
    }

    bool advance(Phase next)
    {
        if (phase == idle || phase == closed || next == armed ||
            static_cast<int>(next) != static_cast<int>(phase) + 1)
            return false;
        phase = next;
        return true;
    }
};
}
