/* SPDX-License-Identifier: MIT */
#include <extgpu/private_irq_mailbox.h>

#include <assert.h>
#include <pthread.h>

struct irq_race {
    struct openpci_irq_mailbox mailbox;
    bool notification;
};

static void *interrupt(void *context)
{
    struct irq_race *race = context;
    race->notification = openpci_irq_publish(&race->mailbox, 3, 8);
    return NULL;
}

int main(void)
{
    struct openpci_irq_mailbox mailbox = {0};
    uint64_t pending[EXTGPU_IRQ_BITMAP_WORDS];
    assert(openpci_irq_publish(&mailbox, 0, 1));
    assert(!openpci_irq_publish(&mailbox, 1, 2));
    openpci_irq_drain_mailbox(&mailbox, pending);
    assert(pending[0]==1 && pending[1]==2 && !pending[2] && !pending[3]);
    assert(openpci_irq_publish(&mailbox, 2, 4));
    openpci_irq_drain_mailbox(&mailbox, pending);
    assert(!pending[0] && !pending[1] && pending[2]==4 && !pending[3]);

    for (unsigned i=0; i<2000; ++i) {
        struct irq_race race = {0};
        assert(openpci_irq_publish(&race.mailbox, 0, 1));
        pthread_t thread;
        assert(pthread_create(&thread,NULL,interrupt,&race)==0);
        openpci_irq_drain_mailbox(&race.mailbox,pending);
        assert(pending[0]==1);
        assert(pthread_join(thread,NULL)==0);
        /* An interrupt concurrent with the drain must either be in the
         * first bitmap or have requested another wake for the next drain. */
        if (!pending[3]) {
            assert(race.notification);
            openpci_irq_drain_mailbox(&race.mailbox,pending);
        }
        assert(pending[3]==8);
    }
}
