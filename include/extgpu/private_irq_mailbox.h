/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef OPENPCI_PRIVATE_IRQ_MAILBOX_H
#define OPENPCI_PRIVATE_IRQ_MAILBOX_H

#include <stdbool.h>
#include <stdint.h>
#include <extgpu/extgpu_driver_abi.h>

#define OPENPCI_IRQ_MAILBOX_BYTES 16384u

struct openpci_irq_mailbox {
    uint64_t pending[EXTGPU_IRQ_BITMAP_WORDS];
    uint64_t notified;
    uint64_t generation;
};

static inline bool openpci_irq_request_notification(struct openpci_irq_mailbox *mailbox)
{
    return __atomic_exchange_n(&mailbox->notified, 1, __ATOMIC_ACQ_REL) == 0;
}

static inline bool openpci_irq_publish(struct openpci_irq_mailbox *mailbox, unsigned word, uint64_t bits)
{
    __atomic_fetch_or(&mailbox->pending[word], bits, __ATOMIC_RELEASE);
    return openpci_irq_request_notification(mailbox);
}

static inline void openpci_irq_drain_mailbox(struct openpci_irq_mailbox *mailbox,
                                             uint64_t pending[EXTGPU_IRQ_BITMAP_WORDS])
{
    /* Reset before reading any words. A producer that observes the old
     * notification must already have published a bit we are about to drain.
     * A later producer can send a new notification, even during the drain. */
    __atomic_exchange_n(&mailbox->notified, 0, __ATOMIC_ACQ_REL);
    for (unsigned i=0; i<EXTGPU_IRQ_BITMAP_WORDS; ++i)
        pending[i] = __atomic_exchange_n(&mailbox->pending[i], 0, __ATOMIC_ACQ_REL);
}

#endif
