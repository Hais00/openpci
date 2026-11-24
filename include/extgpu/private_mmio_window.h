/* SPDX-License-Identifier: MIT
 * Driver/SDK wire payload. No PCI or DMA memory is shared with the client.
 */
#ifndef OPENPCI_PRIVATE_MMIO_WINDOW_H
#define OPENPCI_PRIVATE_MMIO_WINDOW_H

#include <stdbool.h>
#include <stdint.h>
#include <extgpu/extgpu.h>

#define OPENPCI_MMIO_WINDOW_BYTES 16384u
#define OPENPCI_MMIO_WINDOW_SERVICE_NS UINT64_C(150000)

enum openpci_mmio_phase {
    OPENPCI_MMIO_OFFLINE, OPENPCI_MMIO_READY,
    OPENPCI_MMIO_START, OPENPCI_MMIO_REQUEST, OPENPCI_MMIO_BUSY,
    OPENPCI_MMIO_DONE, OPENPCI_MMIO_DONE_OFFLINE, OPENPCI_MMIO_ABORTED
};

enum openpci_mmio_operation {
    OPENPCI_MMIO_SCALAR_READ, OPENPCI_MMIO_SCALAR_WRITE, OPENPCI_MMIO_BAR_WORD_WRITE
};

/* The SDK publishes one ticket at a time. Only a successfully claimed ticket
 * may perform I/O. DONE_OFFLINE preserves the reply after the window ends.
 */
struct openpci_mmio_window {
    uint64_t state, generation;
    uint64_t index, offset, width, input, write;
    uint64_t output, status;
    /* For BAR_WORD_WRITE, width is the contiguous 64-bit word count. */
    uint64_t words[EXTGPU_BAR_WORDS_MAX];
};

static inline uint64_t openpci_mmio_state(const struct openpci_mmio_window *window)
{
    return __atomic_load_n(&window->state, __ATOMIC_ACQUIRE);
}

static inline bool openpci_mmio_change(struct openpci_mmio_window *window,
                                       uint64_t old, uint64_t next)
{
    return __atomic_compare_exchange_n(&window->state, &old, next, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static inline unsigned openpci_mmio_phase(uint64_t state) { return (unsigned)(state & 7); }
static inline uint64_t openpci_mmio_ticket(uint64_t state) { return state & ~UINT64_C(7); }

static inline bool openpci_mmio_should_spin(unsigned phase, enum openpci_mmio_operation operation,
                                           uint64_t elapsed_ns)
{
    // A claimed batch can still be waiting for a PCI read or its typed writes.
    // Poll it within the service budget to avoid sleeping through completion.
    const uint64_t spin_ns = phase == OPENPCI_MMIO_BUSY && operation == OPENPCI_MMIO_BAR_WORD_WRITE
        ? OPENPCI_MMIO_WINDOW_SERVICE_NS : UINT64_C(40000);
    return elapsed_ns < spin_ns;
}

static inline void openpci_mmio_pause(void)
{
#if defined(__aarch64__)
    __asm__ volatile("yield");
#elif defined(__x86_64__)
    __asm__ volatile("pause");
#endif
}

#endif
