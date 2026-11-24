#include "ColdRemovalLedger.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <type_traits>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)
using namespace extgpu_driver;
static_assert(std::is_trivial<ColdRemovalLedger>::value, "DriverKit zero-initialized POD required");
static_assert(std::is_standard_layout<ColdRemovalLedger>::value, "No hidden ownership/layout machinery");
static_assert(std::is_trivial<ColdRemovalEntry>::value, "Preallocated POD entries required");

struct Resource {
    bool command = true, descriptor = true;
    int32_t status = 0;
    unsigned completes = 0, commandReleases = 0, descriptorReleases = 0;
};

struct Operations {
    ColdRemovalLedger *ledger;
    bool checkReentrancy = false;
    unsigned callbacks = 0;

    Resource &resource(ColdRemovalEntry &entry)
    {
        CHECK(entry.map.token);
        return *reinterpret_cast<Resource *>(entry.map.token);
    }
    int32_t complete(ColdRemovalEntry &entry)
    {
        ++callbacks;
        auto &r = resource(entry);
        CHECK(ledger->inFlight && entry.state == ColdRemovalEntryState::completing);
        CHECK(r.command && r.descriptor && !entry.commitSequence);
        CHECK(!r.commandReleases && !r.descriptorReleases);
        ++r.completes;
        if (checkReentrancy) {
            CHECK(ledger->retire(ledger->key, entry.map.iova, entry.map.bytes, 3, *this).result == ColdRemovalResult::busy);
            CHECK(ledger->finish(ledger->key, 3).result == ColdRemovalResult::busy);
            const ColdRemovalIdentity identity{ledger->key.generation, ledger->key.serviceID, ledger->key.pciID};
            CHECK(ledger->arm(ledger->key, identity, ledger->entries, ledger->capacity, ledger->count) == ColdRemovalResult::busy);
            ledger->note_provider_stop();
            CHECK(entry.state == ColdRemovalEntryState::completing && ledger->inFlight);
        }
        return r.status;
    }
    void release_command(ColdRemovalEntry &entry)
    {
        ++callbacks;
        auto &r = resource(entry);
        CHECK(ledger->inFlight && entry.state == ColdRemovalEntryState::completed_refs_held);
        CHECK(r.status == 0 && r.command && r.descriptor && !entry.nativeStatus);
        CHECK(!entry.commitSequence && ledger->remaining());
        CHECK(!r.commandReleases && !r.descriptorReleases);
        r.command = false;
        ++r.commandReleases;
    }
    void release_descriptor(ColdRemovalEntry &entry)
    {
        ++callbacks;
        auto &r = resource(entry);
        CHECK(ledger->inFlight && entry.state == ColdRemovalEntryState::completed_refs_held);
        CHECK(!r.command && r.descriptor && r.commandReleases == 1 && !r.descriptorReleases);
        CHECK(!entry.commitSequence && ledger->remaining());
        CHECK(ledger->finish(ledger->key, 3).result == ColdRemovalResult::busy);
        r.descriptor = false;
        ++r.descriptorReleases;
    }
};

struct Fixture {
    ColdRemovalLedger ledger{};
    ColdRemovalEntry entries[2]{};
    ColdRemovalKey key{41, 1001, 2002, {17, 29}};
    ColdRemovalIdentity identity{41, 1001, 2002};
    Resource resources[2]{};
    Operations operations{&ledger};

    Fixture()
    {
        entries[0].map = {0, 4096, reinterpret_cast<uintptr_t>(&resources[0])};
        entries[1].map = {8192, 8192, reinterpret_cast<uintptr_t>(&resources[1])};
    }
    void arm()
    {
        CHECK(ledger.arm(key, identity, entries, 2, 2) == ColdRemovalResult::success);
        CHECK(ledger.started() && !ledger.closed() && !ledger.inFlight);
        CHECK(ledger.remaining() == 2 && ledger.count == 2 && ledger.capacity == 2);
        CHECK(!ledger.commitSequence && same_cold_removal_key(ledger.key, key));
        CHECK(!operations.callbacks && resources[0].command && resources[0].descriptor);
    }
    ColdRemovalReply retire(unsigned i, uint64_t confirmations = 3)
    {
        return ledger.retire(key, entries[i].map.iova, entries[i].map.bytes, confirmations, operations);
    }
    void denied(const ColdRemovalKey &candidate, uint64_t iova, uint64_t bytes,
                 uint64_t confirmations, ColdRemovalResult wanted)
    {
        const unsigned callbacks = operations.callbacks;
        const uint64_t sequence = ledger.commitSequence;
        CHECK(ledger.retire(candidate, iova, bytes, confirmations, operations).result == wanted);
        CHECK(operations.callbacks == callbacks && ledger.commitSequence == sequence);
    }
};

static void arm_preflight_is_all_or_nothing()
{
    for (unsigned fault = 0; fault < 12; ++fault) {
        Fixture f;
        if (fault == 0) f.entries[1].map.bytes = 0;
        if (fault == 1) f.entries[1].map.token = 0;
        if (fault == 2) f.entries[1].map.token = f.entries[0].map.token;
        if (fault == 3) f.entries[1].map.iova = 2048; // Overlap first range.
        if (fault == 4) f.entries[1].map.iova = 0; // Duplicate ID.
        if (fault == 5) f.entries[1].map = {UINT64_MAX - 7, 8, f.entries[1].map.token};
        if (fault == 6) f.entries[1].state = ColdRemovalEntryState::committed;
        if (fault == 7) f.entries[1].commitSequence = 1;
        if (fault == 8) f.entries[1].nativeStatus = -1;
        if (fault == 9) f.key.nonce[0] = f.key.nonce[1] = 0;
        if (fault == 10) f.key.serviceID = 0;
        if (fault == 11) f.key.generation = 0;
        unsigned char beforeLedger[sizeof(f.ledger)], beforeEntries[sizeof(f.entries)];
        std::memcpy(beforeLedger, &f.ledger, sizeof(f.ledger));
        std::memcpy(beforeEntries, f.entries, sizeof(f.entries));
        CHECK(f.ledger.arm(f.key, f.identity, f.entries, 2, 2) == ColdRemovalResult::bad_argument);
        CHECK(!std::memcmp(beforeLedger, &f.ledger, sizeof(f.ledger)));
        CHECK(!std::memcmp(beforeEntries, f.entries, sizeof(f.entries)));
        CHECK(!f.operations.callbacks);
    }
    for (unsigned field = 0; field < 3; ++field) {
        Fixture f;
        if (field == 0) ++f.identity.generation;
        if (field == 1) ++f.identity.serviceID;
        if (field == 2) ++f.identity.pciID;
        CHECK(f.ledger.arm(f.key, f.identity, f.entries, 2, 2) == ColdRemovalResult::stale_key);
        CHECK(!f.ledger.started() && f.entries[0].state == ColdRemovalEntryState::empty);
    }
    Fixture f;
    CHECK(f.ledger.arm(f.key, f.identity, f.entries, 1, 2) == ColdRemovalResult::capacity);
    CHECK(f.ledger.arm(f.key, f.identity, f.entries, ColdRemovalMaximumEntries + 1, 0) == ColdRemovalResult::capacity);
    CHECK(f.ledger.arm(f.key, f.identity, nullptr, 2, 0) == ColdRemovalResult::bad_argument);
    CHECK(f.ledger.arm(f.key, f.identity, f.entries, 0, 0) == ColdRemovalResult::bad_argument);
    f.identity.pciID = 0;
    CHECK(f.ledger.arm(f.key, f.identity, f.entries, 2, 2) == ColdRemovalResult::unavailable);
    CHECK(!f.ledger.started() && !f.operations.callbacks);
}

static void frozen_identity_and_confirmations_gate_every_callback()
{
    Fixture f;
    f.denied(f.key, 0, 4096, 3, ColdRemovalResult::unavailable);
    CHECK(f.ledger.finish(f.key, 3).result == ColdRemovalResult::unavailable);
    f.arm();
    for (unsigned field = 0; field < 5; ++field) {
        auto stale = f.key;
        if (field == 0) ++stale.generation;
        if (field == 1) ++stale.serviceID;
        if (field == 2) ++stale.pciID;
        if (field == 3) ++stale.nonce[0];
        if (field == 4) ++stale.nonce[1];
        f.denied(stale, 0, 4096, 3, ColdRemovalResult::stale_key);
        CHECK(f.ledger.finish(stale, 3).result == ColdRemovalResult::stale_key);
        CHECK(f.ledger.arm(stale, f.identity, f.entries, 2, 2) == ColdRemovalResult::stale_key);
    }
    for (uint64_t flags : {UINT64_C(0), UINT64_C(1), UINT64_C(2), UINT64_C(4), UINT64_C(7), UINT64_MAX}) {
        f.denied(f.key, 0, 4096, flags, ColdRemovalResult::bad_argument);
        CHECK(f.ledger.finish(f.key, flags).result == ColdRemovalResult::bad_argument);
    }
    f.denied(f.key, 0, 4095, 3, ColdRemovalResult::bad_argument);
    f.denied(f.key, 0, 8192, 3, ColdRemovalResult::bad_argument);
    f.denied(f.key, 4096, 4096, 3, ColdRemovalResult::not_found);
    CHECK(f.ledger.finish(f.key, 3).result == ColdRemovalResult::busy);
    CHECK(f.ledger.arm(f.key, f.identity, f.entries, 2, 2) == ColdRemovalResult::success); // Lost BEGIN reply.
    CHECK(f.ledger.arm(f.key, f.identity, f.entries, 2, 1) == ColdRemovalResult::bad_argument);
    f.ledger.note_provider_stop();
    f.ledger.note_provider_stop();
    CHECK(f.ledger.remaining() == 2 && f.ledger.state == ColdRemovalLedgerState::frozen);
    CHECK(!f.operations.callbacks && f.resources[0].command && f.resources[0].descriptor);
}

static void complete_failures_retain_refs_and_retry_requires_success()
{
    // Native NotReady/unknown failures are not reinterpreted as completed.
    for (int32_t status : {int32_t(-1), int32_t(-7), int32_t(1), int32_t(0xe00002d8u)}) {
        Fixture f;
        f.arm();
        f.resources[0].status = status;
        for (unsigned attempt = 1; attempt <= 2; ++attempt) {
            auto result = f.retire(0);
            CHECK(result.result == ColdRemovalResult::completion_unknown && result.nativeStatus == status);
            CHECK(!result.commitSequence && result.remaining == 2 && !f.ledger.commitSequence);
            CHECK(f.entries[0].state == ColdRemovalEntryState::completion_unknown);
            CHECK(f.entries[0].nativeStatus == status && f.resources[0].completes == attempt);
            CHECK(f.resources[0].command && f.resources[0].descriptor);
            CHECK(!f.resources[0].commandReleases && !f.resources[0].descriptorReleases);
            CHECK(f.entries[1].state == ColdRemovalEntryState::enrolled && !f.resources[1].completes);
            f.ledger.note_provider_stop();
            CHECK(f.ledger.finish(f.key, 3).result == ColdRemovalResult::busy);
        }
        f.resources[0].status = 0;
        f.operations.checkReentrancy = true;
        auto success = f.retire(0);
        CHECK(success.result == ColdRemovalResult::success && success.commitSequence == 1 && success.remaining == 1);
        CHECK(f.resources[0].completes == 3 && !f.resources[0].command && !f.resources[0].descriptor);
        CHECK(f.resources[0].commandReleases == 1 && f.resources[0].descriptorReleases == 1);
        CHECK(f.entries[0].state == ColdRemovalEntryState::committed && !f.entries[0].nativeStatus);
    }
}

static void partial_lost_reply_and_closed_tombstones_are_idempotent()
{
    Fixture f;
    f.arm();
    f.resources[0].status = -3;
    CHECK(f.retire(0).result == ColdRemovalResult::completion_unknown);
    auto second = f.retire(1);
    CHECK(second.result == ColdRemovalResult::success && second.commitSequence == 1 && second.remaining == 1);
    CHECK(f.resources[0].command && f.resources[0].descriptor && !f.resources[1].command && !f.resources[1].descriptor);
    const unsigned callbacks = f.operations.callbacks;
    for (unsigned retry = 0; retry < 4; ++retry) {
        auto replay = f.retire(1); // The original successful wire reply was lost.
        CHECK(replay.result == ColdRemovalResult::success && replay.commitSequence == 1);
        CHECK(f.operations.callbacks == callbacks && f.resources[1].completes == 1);
    }
    f.resources[0].status = 0;
    auto first = f.retire(0);
    CHECK(first.result == ColdRemovalResult::success && first.commitSequence == 2 && !first.remaining);
    CHECK(f.entries[1].commitSequence == 1 && f.entries[0].commitSequence == 2);
    auto finished = f.ledger.finish(f.key, 3);
    CHECK(finished.result == ColdRemovalResult::success && finished.commitSequence == 3 && !finished.remaining);
    CHECK(f.ledger.closed() && f.ledger.started());
    const unsigned closedCallbacks = f.operations.callbacks;
    CHECK(f.ledger.finish(f.key, 3).commitSequence == 3);
    CHECK(f.retire(0).commitSequence == 2 && f.retire(1).commitSequence == 1);
    CHECK(f.operations.callbacks == closedCallbacks && f.ledger.commitSequence == 3);
    CHECK(f.ledger.arm(f.key, f.identity, f.entries, 2, 2) == ColdRemovalResult::closed);
    auto newKey = f.key;
    ++newKey.nonce[0];
    CHECK(f.ledger.arm(newKey, f.identity, f.entries, 2, 2) == ColdRemovalResult::stale_key);
    CHECK(f.ledger.closed() && same_cold_removal_key(f.ledger.key, f.key));
    f.denied(f.key, 0, 4095, 3, ColdRemovalResult::bad_argument);
    f.denied(f.key, 4096, 4096, 3, ColdRemovalResult::not_found);
}

static void maximum_capacity_and_empty_ledger()
{
    ColdRemovalEntry entries[ColdRemovalMaximumEntries]{};
    for (uint32_t i = 0; i < ColdRemovalMaximumEntries; ++i)
        entries[i].map = {uint64_t(i) * 4096, 4096, uintptr_t(i) + 1};
    ColdRemovalLedger ledger{};
    const ColdRemovalKey key{1, 2, 3, {0, 4}}; // Nonzero 128-bit nonce, not two mandatory nonzero words.
    const ColdRemovalIdentity identity{1, 2, 3};
    CHECK(ledger.arm(key, identity, entries, ColdRemovalMaximumEntries, ColdRemovalMaximumEntries) == ColdRemovalResult::success);
    CHECK(ledger.remaining() == ColdRemovalMaximumEntries && !ledger.commitSequence);
    CHECK(ledger.finish(key, 3).result == ColdRemovalResult::busy);
    ColdRemovalLedger empty{};
    CHECK(empty.arm(key, identity, nullptr, 0, 0) == ColdRemovalResult::success);
    CHECK(empty.started() && !empty.closed() && !empty.remaining());
    empty.note_provider_stop();
    CHECK(!empty.closed());
    CHECK(empty.finish(key, 1).result == ColdRemovalResult::bad_argument);
    auto closed = empty.finish(key, 3);
    CHECK(closed.result == ColdRemovalResult::success && closed.commitSequence == 1 && empty.closed());
    CHECK(empty.finish(key, 3).commitSequence == 1);

    Fixture exhausted;
    exhausted.arm();
    exhausted.ledger.commitSequence = UINT64_MAX;
    CHECK(exhausted.retire(0).result == ColdRemovalResult::capacity);
    CHECK(!exhausted.operations.callbacks && exhausted.resources[0].command && exhausted.resources[0].descriptor);
    ColdRemovalLedger emptyExhausted{};
    CHECK(emptyExhausted.arm(key, identity, nullptr, 0, 0) == ColdRemovalResult::success);
    emptyExhausted.commitSequence = UINT64_MAX;
    CHECK(emptyExhausted.finish(key, 3).result == ColdRemovalResult::capacity);
    CHECK(!emptyExhausted.closed() && emptyExhausted.commitSequence == UINT64_MAX);
}

int main()
{
    arm_preflight_is_all_or_nothing();
    frozen_identity_and_confirmations_gate_every_callback();
    complete_failures_retain_refs_and_retry_requires_success();
    partial_lost_reply_and_closed_tombstones_are_idempotent();
    maximum_capacity_and_empty_ledger();
    std::puts("PASS ColdRemovalLedger: preflight atomicity, exact keys/tuples/confirmations, retained unknown Complete failures, held-ref release ordering, monotonic tombstones, lost replies/retries, bounded capacity and irreversible finish; CPU only");
    return 0;
}
