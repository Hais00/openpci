#ifndef EXTGPU_IRQ_CANCELLATION_HPP
#define EXTGPU_IRQ_CANCELLATION_HPP

#include <stdint.h>

namespace extgpu_driver {

/* The SDK can deliver Cancel completion before Cancel returns. A failed call
 * does not authorize releasing its source, even if a completion later arrives.
 * Exactly one observer may retire a successful call after both events.
 * The caller owns the source/client references until that observer retires it.
 */
struct IRQCancellation {
    uint32_t state;
};

enum : uint32_t {
    IRQ_CANCEL_STARTED = 1u << 0,
    IRQ_CANCEL_RETURNED = 1u << 1,
    IRQ_CANCEL_COMPLETED = 1u << 2,
    IRQ_CANCEL_FAILED = 1u << 3,
    IRQ_CANCEL_RETIRED = 1u << 4
};

inline bool begin_irq_cancel(IRQCancellation &cancel)
{
    uint32_t expected = 0;
    return __atomic_compare_exchange_n(&cancel.state, &expected,
        IRQ_CANCEL_STARTED, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

inline bool finish_irq_cancel_if_proven(IRQCancellation &cancel)
{
    uint32_t expected = IRQ_CANCEL_STARTED | IRQ_CANCEL_RETURNED | IRQ_CANCEL_COMPLETED;
    return __atomic_compare_exchange_n(&cancel.state, &expected,
        expected | IRQ_CANCEL_RETIRED, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

inline bool record_irq_cancel_return(IRQCancellation &cancel, bool success)
{
    __atomic_fetch_or(&cancel.state,
        IRQ_CANCEL_RETURNED | (success ? 0u : IRQ_CANCEL_FAILED), __ATOMIC_ACQ_REL);
    return finish_irq_cancel_if_proven(cancel);
}

inline bool record_irq_cancel_completion(IRQCancellation &cancel)
{
    __atomic_fetch_or(&cancel.state, IRQ_CANCEL_COMPLETED, __ATOMIC_ACQ_REL);
    return finish_irq_cancel_if_proven(cancel);
}

} // namespace extgpu_driver
#endif
