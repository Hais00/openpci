/* SPDX-License-Identifier: MIT */
#include <extgpu/private_mmio_window.h>
#include <extgpu/private_mmio_ring.h>

#include <assert.h>
#include <pthread.h>

_Static_assert(offsetof(struct openpci_mmio_window, words) == 9 * sizeof(uint64_t),
               "The scalar window prefix must remain compatible with 1.21");
_Static_assert(sizeof(struct openpci_mmio_window) <= OPENPCI_MMIO_WINDOW_BYTES,
               "The word payload must fit in the shared page");

enum { FIRST_TICKET = 8, SECOND_TICKET = 16 };

struct race {
    struct openpci_mmio_window window;
    unsigned claimed;
};

static void *claim_request(void *context)
{
    struct race *race = context;
    if (openpci_mmio_change(&race->window, FIRST_TICKET | OPENPCI_MMIO_START,
                            FIRST_TICKET | OPENPCI_MMIO_BUSY)) {
        ++race->claimed;
        __atomic_store_n(&race->window.output, UINT64_C(0x1234abcd), __ATOMIC_RELAXED);
        assert(openpci_mmio_change(&race->window, FIRST_TICKET | OPENPCI_MMIO_BUSY,
                                   FIRST_TICKET | OPENPCI_MMIO_DONE));
    }
    return NULL;
}

static void test_cancel_and_claim_are_mutually_exclusive(void)
{
    struct race race = {.window.state = FIRST_TICKET | OPENPCI_MMIO_START};
    pthread_t server;
    assert(pthread_create(&server, NULL, claim_request, &race) == 0);
    bool canceled = openpci_mmio_change(&race.window, FIRST_TICKET | OPENPCI_MMIO_START,
                                        FIRST_TICKET | OPENPCI_MMIO_OFFLINE);
    assert(pthread_join(server, NULL) == 0);

    const uint64_t state = openpci_mmio_state(&race.window);
    assert(openpci_mmio_ticket(state) == FIRST_TICKET);
    if (canceled) {
        assert(race.claimed == 0 && openpci_mmio_phase(state) == OPENPCI_MMIO_OFFLINE);
    } else {
        assert(race.claimed == 1 && openpci_mmio_phase(state) == OPENPCI_MMIO_DONE);
        assert(__atomic_load_n(&race.window.output, __ATOMIC_RELAXED) == UINT64_C(0x1234abcd));
    }
}

static void test_retirement_preserves_last_reply_and_next_ticket(void)
{
    struct openpci_mmio_window window = {.state = FIRST_TICKET | OPENPCI_MMIO_DONE};
    assert(openpci_mmio_change(&window, FIRST_TICKET | OPENPCI_MMIO_DONE,
                               FIRST_TICKET | OPENPCI_MMIO_DONE_OFFLINE));
    assert(!openpci_mmio_change(&window, FIRST_TICKET | OPENPCI_MMIO_DONE,
                                FIRST_TICKET | OPENPCI_MMIO_READY));
    assert(openpci_mmio_change(&window, FIRST_TICKET | OPENPCI_MMIO_DONE_OFFLINE,
                               FIRST_TICKET | OPENPCI_MMIO_OFFLINE));
    assert(openpci_mmio_change(&window, FIRST_TICKET | OPENPCI_MMIO_OFFLINE,
                               SECOND_TICKET | OPENPCI_MMIO_START));
    assert(!openpci_mmio_change(&window, FIRST_TICKET | OPENPCI_MMIO_START,
                                FIRST_TICKET | OPENPCI_MMIO_ABORTED));
    assert(openpci_mmio_state(&window) == (SECOND_TICKET | OPENPCI_MMIO_START));
}

static void test_claimed_batch_has_a_bounded_spin_budget(void)
{
    for (unsigned phase = OPENPCI_MMIO_OFFLINE; phase <= OPENPCI_MMIO_ABORTED; ++phase) {
        for (unsigned op = OPENPCI_MMIO_SCALAR_READ; op <= OPENPCI_MMIO_BAR_WORD_WRITE; ++op) {
            uint64_t budget = phase == OPENPCI_MMIO_BUSY && op == OPENPCI_MMIO_BAR_WORD_WRITE
                ? OPENPCI_MMIO_WINDOW_SERVICE_NS : 40000;
            assert(openpci_mmio_should_spin(phase, op, budget - 1));
            assert(!openpci_mmio_should_spin(phase, op, budget));
        }
    }
}

struct ring_race {
    struct openpci_mmio_ring ring;
    bool won;
};

static void *disarm_ring(void *context)
{
    struct ring_race *race = context;
    race->won = openpci_mmio_ring_disarm(&race->ring, 0);
    return NULL;
}

static void test_ring_publication_assigns_a_consumer(void)
{
    struct ring_race race = {.ring.active = OPENPCI_RING_ACTIVE};
    pthread_t server;
    assert(pthread_create(&server, NULL, disarm_ring, &race) == 0);
    const bool kick = openpci_mmio_ring_publish_active(&race.ring, 1);
    assert(pthread_join(server, NULL) == 0);
    assert(kick == race.won);
    assert(race.ring.active == OPENPCI_RING_ACTIVE && race.ring.published == 1);
}

static void *irq_arm_ring(void *context)
{
    struct ring_race *race = context;
    race->won = openpci_mmio_ring_arm_idle(&race->ring);
    return NULL;
}

static void test_irq_arm_and_producer_share_one_consumer(void)
{
    struct ring_race race = {0};
    pthread_t irq;
    assert(pthread_create(&irq, NULL, irq_arm_ring, &race) == 0);
    const bool kick = openpci_mmio_ring_publish_active(&race.ring, 1);
    assert(pthread_join(irq, NULL) == 0);
    assert(kick != race.won);
    assert(race.ring.active == OPENPCI_RING_ACTIVE && race.ring.published == 1);
}

static void test_ring_retirement_and_adaptive_budget(void)
{
    struct openpci_mmio_ring ring = {.active = OPENPCI_RING_RETIRED};
    assert(!openpci_mmio_ring_publish_active(&ring, 1));
    assert(ring.active == OPENPCI_RING_RETIRED);
    assert(openpci_mmio_ring_adaptive_idle(192000, 95999) == 192000);
    assert(openpci_mmio_ring_adaptive_idle(192000, 96000) == 48000);
    assert(openpci_mmio_ring_adaptive_idle(192000, 192000) == 192000);
    assert(openpci_mmio_ring_adaptive_idle(32000, 24000) == 24000);
}

int main(void)
{
    test_retirement_preserves_last_reply_and_next_ticket();
    test_claimed_batch_has_a_bounded_spin_budget();
    test_ring_retirement_and_adaptive_budget();
    for (unsigned i = 0; i < 1000; ++i) {
        test_cancel_and_claim_are_mutually_exclusive();
        test_ring_publication_assigns_a_consumer();
        test_irq_arm_and_producer_share_one_consumer();
    }
}
