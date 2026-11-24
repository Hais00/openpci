#ifndef EXTGPU_OWNERSHIP_RECOVERY_HPP
#define EXTGPU_OWNERSHIP_RECOVERY_HPP

#include <extgpu/extgpu_driver_abi.h>

namespace extgpu_driver {

/* Only standard PCI configuration header reads are admitted as observational.
 * MMIO and device-specific configuration reads may have device side effects.
 * This classifies an attempted operation before its arguments reach hardware;
 * failure is not evidence that an operation had no effect.
 */
inline bool preserves_pristine_claim(uint64_t selector,
                                     const uint64_t *inputs,
                                     uint32_t inputCount)
{
    switch (selector) {
    case EXTGPU_SELECTOR_QUERY_PROTOCOL:
    case EXTGPU_SELECTOR_GET_IDENTITY:
    case EXTGPU_SELECTOR_GET_BAR_INFO:
    case EXTGPU_SELECTOR_CLAIM:
    case EXTGPU_SELECTOR_RELEASE_OWNER:
        return true;
    case EXTGPU_SELECTOR_CONFIG_READ:
        if (inputs == nullptr || inputCount < 2) return false;
        return (inputs[1] == 1 || inputs[1] == 2 || inputs[1] == 4) &&
               inputs[0] <= 64 - inputs[1] &&
               (inputs[0] & (inputs[1] - 1)) == 0;
    default:
        return false;
    }
}

} // namespace extgpu_driver
#endif
