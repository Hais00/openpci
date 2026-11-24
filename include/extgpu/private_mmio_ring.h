/* SPDX-License-Identifier: MIT
 * CPU-only DriverKit/SDK transport. One serialized producer per claim.
 */
#ifndef OPENPCI_PRIVATE_MMIO_RING_H
#define OPENPCI_PRIVATE_MMIO_RING_H
#include <extgpu/private_mmio_window.h>

#define OPENPCI_MMIO_RING_SLOTS 32u
#define OPENPCI_MMIO_RING_POLL_MIN_US 32u
#define OPENPCI_MMIO_RING_POLL_MAX_US 1000u
#define OPENPCI_MMIO_RING_IRQ_FIRST_NS UINT64_C(48000)
#define OPENPCI_MMIO_RING_ADAPTIVE_FLAG UINT64_C(1)
#define OPENPCI_MMIO_RING_IRQ_ARM_FLAG UINT64_C(2)
#define OPENPCI_MMIO_RING_EVENT_FLAG UINT64_C(4)

enum openpci_mmio_ring_state { OPENPCI_RING_IDLE, OPENPCI_RING_ACTIVE, OPENPCI_RING_RETIRED };
enum openpci_mmio_ring_op {
    OPENPCI_RING_READ, OPENPCI_RING_WRITE, OPENPCI_RING_READ_WORDS, OPENPCI_RING_WRITE_WORDS,
    OPENPCI_RING_WRITE_GROUPED
};

struct openpci_mmio_ring_slot {
    uint64_t operation, index, offset, width, input, output;
};

struct openpci_mmio_ring {
    uint64_t generation;
    uint64_t published, active;
    uint64_t completed, status;
    struct openpci_mmio_ring_slot slots[OPENPCI_MMIO_RING_SLOTS];
    uint64_t irqArms, irqHits, irqExtensions;
};

static inline struct openpci_mmio_ring_slot *openpci_mmio_ring_slot_at(
    struct openpci_mmio_ring *ring, uint64_t sequence)
{
    return &ring->slots[sequence % OPENPCI_MMIO_RING_SLOTS];
}

/* The producer publishes a fully written batch before exchanging active.
 * The idle consumer clears active and then rechecks published. Therefore an
 * arrival racing disarm either owns the next kick or is observed by the
 * existing consumer. The exchanges arbitrate ownership of that kick. */
static inline bool openpci_mmio_ring_publish(struct openpci_mmio_ring *ring, uint64_t end)
{
    __atomic_store_n(&ring->published, end, __ATOMIC_RELEASE);
    return __atomic_exchange_n(&ring->active, OPENPCI_RING_ACTIVE, __ATOMIC_SEQ_CST) == OPENPCI_RING_IDLE;
}

static inline bool openpci_mmio_ring_arm_idle(struct openpci_mmio_ring *ring)
{
    uint64_t idle = OPENPCI_RING_IDLE;
    return __atomic_compare_exchange_n(&ring->active, &idle, OPENPCI_RING_ACTIVE, false,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

/* With the 1.25 consumer, an active window needs only a load. The sequential
 * order of publication, state observation and the disarm recheck makes every
 * new command visible to the consumer or the producer responsible for a kick. */
static inline bool openpci_mmio_ring_publish_active(struct openpci_mmio_ring *ring, uint64_t end)
{
    __atomic_store_n(&ring->published, end, __ATOMIC_SEQ_CST);
    return __atomic_load_n(&ring->active, __ATOMIC_SEQ_CST) == OPENPCI_RING_IDLE &&
        openpci_mmio_ring_arm_idle(ring);
}

static inline bool openpci_mmio_ring_disarm(struct openpci_mmio_ring *ring, uint64_t consumed)
{
    __atomic_exchange_n(&ring->active, OPENPCI_RING_IDLE, __ATOMIC_SEQ_CST);
    return __atomic_load_n(&ring->published, __ATOMIC_SEQ_CST) == consumed ||
        __atomic_exchange_n(&ring->active, OPENPCI_RING_ACTIVE, __ATOMIC_SEQ_CST) != OPENPCI_RING_IDLE;
}

static inline uint64_t openpci_mmio_ring_adaptive_idle(uint64_t budget_ns, uint64_t gap_ns)
{
    /* An arrival near the budget boundary provides weak evidence that the
     * same burst will continue. Give it a shorter, bounded grace period. */
    if (gap_ns < budget_ns / 2 || gap_ns >= budget_ns) return budget_ns;
    const uint64_t floor = UINT64_C(24000);
    return budget_ns / 4 > floor ? budget_ns / 4 : floor;
}
#endif
