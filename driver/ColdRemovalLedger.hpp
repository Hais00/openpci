/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Bounded terminal DMA retirement policy. Caller serializes every operation and
 * owns the preallocated entry storage for the entire authenticated endpoint
 * lifetime. No allocation, physical observation or platform API occurs here.
 */
#ifndef EXTGPU_COLD_REMOVAL_LEDGER_HPP
#define EXTGPU_COLD_REMOVAL_LEDGER_HPP
#include <stdint.h>

namespace extgpu_driver {
static constexpr uint32_t ColdRemovalMaximumEntries = 4096;
static constexpr uint64_t ColdRemovalRequiredConfirmations = 3;

struct ColdRemovalKey {
    uint64_t generation, serviceID, pciID;
    uint64_t nonce[2];
};

struct ColdRemovalIdentity {
    uint64_t generation, serviceID, pciID;
};

struct ColdRemovalMap {
    uint64_t iova, bytes;
    uintptr_t token; // Nonzero unique association with the original retained map.
};

enum class ColdRemovalEntryState : uint32_t {
    empty, enrolled, completing, completion_unknown, completed_refs_held, committed
};
enum class ColdRemovalLedgerState : uint32_t { idle, frozen, closed };
enum class ColdRemovalResult : uint32_t {
    success, bad_argument, stale_key, capacity, unavailable, busy, not_found,
    completion_unknown, closed
};

struct ColdRemovalEntry {
    ColdRemovalMap map;
    ColdRemovalEntryState state;
    uint64_t commitSequence;
    int32_t nativeStatus;
};

struct ColdRemovalReply {
    ColdRemovalResult result;
    int32_t nativeStatus;
    uint64_t commitSequence;
    uint32_t remaining;
};

inline bool same_cold_removal_key(const ColdRemovalKey &a, const ColdRemovalKey &b)
{
    return a.generation == b.generation && a.serviceID == b.serviceID &&
        a.pciID == b.pciID && a.nonce[0] == b.nonce[0] && a.nonce[1] == b.nonce[1];
}

inline bool valid_cold_removal_key(const ColdRemovalKey &key)
{
    return key.generation && key.serviceID && key.pciID && (key.nonce[0] || key.nonce[1]);
}

/* POD, deliberately without constructors: zero initialization is the idle
 * state. Prepopulate only Entry.map in otherwise-zero storage before arm().
 * The caller must not mutate enrolled map tuples or reuse their IDs/storage.
 */
struct ColdRemovalLedger {
    ColdRemovalKey key;
    ColdRemovalEntry *entries;
    uint32_t capacity, count;
    uint64_t commitSequence;
    ColdRemovalLedgerState state;
    bool inFlight;

    bool started() const { return state != ColdRemovalLedgerState::idle; }
    bool closed() const { return state == ColdRemovalLedgerState::closed; }

    uint32_t remaining() const
    {
        uint32_t result = 0;
        for (uint32_t i = 0; i < count; ++i)
            if (entries[i].state != ColdRemovalEntryState::committed) ++result;
        return result;
    }

    ColdRemovalReply reply(ColdRemovalResult result, int32_t native = 0,
                           uint64_t sequence = 0) const
    {
        return {result, native, sequence, remaining()};
    }

    ColdRemovalResult arm(const ColdRemovalKey &candidate,
                           const ColdRemovalIdentity &identity,
                           ColdRemovalEntry *storage, uint32_t storageCapacity,
                           uint32_t enrolledCount)
    {
        if (!valid_cold_removal_key(candidate)) return ColdRemovalResult::bad_argument;
        if (!identity.generation || !identity.serviceID || !identity.pciID)
            return ColdRemovalResult::unavailable;
        if (candidate.generation != identity.generation || candidate.serviceID != identity.serviceID ||
            candidate.pciID != identity.pciID) return ColdRemovalResult::stale_key;
        if (started()) {
            if (!same_cold_removal_key(candidate, key)) return ColdRemovalResult::stale_key;
            if (closed()) return ColdRemovalResult::closed;
            if (inFlight) return ColdRemovalResult::busy;
            return storage == entries && storageCapacity == capacity && enrolledCount == count
                ? ColdRemovalResult::success : ColdRemovalResult::bad_argument;
        }
        if (storageCapacity > ColdRemovalMaximumEntries || enrolledCount > storageCapacity)
            return ColdRemovalResult::capacity;
        if ((storageCapacity && !storage) || (!storageCapacity && storage))
            return ColdRemovalResult::bad_argument;
        // Preflight all entries before changing either ledger or entry storage.
        for (uint32_t i = 0; i < enrolledCount; ++i) {
            const auto &entry = storage[i];
            const auto &map = entry.map;
            if (entry.state != ColdRemovalEntryState::empty || entry.commitSequence ||
                entry.nativeStatus || !map.token || !map.bytes || map.bytes > UINT64_MAX - map.iova)
                return ColdRemovalResult::bad_argument;
            for (uint32_t j = 0; j < i; ++j) {
                const auto &prior = storage[j].map;
                if (map.token == prior.token ||
                    (map.iova < prior.iova + prior.bytes && prior.iova < map.iova + map.bytes))
                    return ColdRemovalResult::bad_argument;
            }
        }
        for (uint32_t i = 0; i < enrolledCount; ++i)
            storage[i].state = ColdRemovalEntryState::enrolled;
        key = candidate;
        entries = storage;
        capacity = storageCapacity;
        count = enrolledCount;
        commitSequence = 0;
        inFlight = false;
        state = ColdRemovalLedgerState::frozen;
        return ColdRemovalResult::success;
    }

    /* Stop, provider disappearance or IPC failure authorizes no retirement and
     * creates no tombstone. The enclosing driver must keep this storage alive. */
    void note_provider_stop() {}

    /* Confirmations are authenticated caller assertions, not observations made
     * by this helper. The SDK/owner must establish the exact old provider's
     * absence and physical power-off; Stop never supplies either assertion.
     * Operations must not mutate Entry.map, throw, or reenter platform cleanup.
     * complete(entry) ONLY obtains the real CompleteDMA acknowledgement; it
     * releases no reference. Release callbacks are synchronous/nonfailing and
     * each releases only its named reference, after completed_refs_held is set.
     * The driver separately validates exact one-chunk command/descriptor state.
     */
    template <typename Operations>
    ColdRemovalReply retire(const ColdRemovalKey &candidate, uint64_t iova,
                             uint64_t bytes, uint64_t confirmations,
                             Operations &operations)
    {
        if (!started()) return reply(ColdRemovalResult::unavailable);
        if (!same_cold_removal_key(candidate, key)) return reply(ColdRemovalResult::stale_key);
        if (confirmations != ColdRemovalRequiredConfirmations)
            return reply(ColdRemovalResult::bad_argument);
        if (inFlight) return reply(ColdRemovalResult::busy);
        ColdRemovalEntry *found = nullptr;
        for (uint32_t i = 0; i < count; ++i) {
            if (entries[i].map.iova == iova) {
                found = &entries[i];
                break;
            }
        }
        if (!found) return reply(ColdRemovalResult::not_found);
        auto &entry = *found;
        if (entry.map.bytes != bytes) return reply(ColdRemovalResult::bad_argument);
        if (entry.state == ColdRemovalEntryState::committed)
            return reply(ColdRemovalResult::success, 0, entry.commitSequence);
        if (closed()) return reply(ColdRemovalResult::closed);
        if (entry.state != ColdRemovalEntryState::enrolled &&
            entry.state != ColdRemovalEntryState::completion_unknown)
            return reply(ColdRemovalResult::busy);
        if (commitSequence == UINT64_MAX) return reply(ColdRemovalResult::capacity);

        inFlight = true;
        entry.state = ColdRemovalEntryState::completing;
        const int32_t native = operations.complete(entry);
        if (native != 0) {
            entry.nativeStatus = native;
            entry.state = ColdRemovalEntryState::completion_unknown;
            inFlight = false;
            return reply(ColdRemovalResult::completion_unknown, native);
        }
        // Complete succeeded, but no SUCCESS reply or commit exists yet.
        entry.nativeStatus = 0;
        entry.state = ColdRemovalEntryState::completed_refs_held;
        operations.release_command(entry);
        operations.release_descriptor(entry);
        entry.commitSequence = ++commitSequence;
        entry.state = ColdRemovalEntryState::committed;
        inFlight = false;
        return reply(ColdRemovalResult::success, 0, entry.commitSequence);
    }

    ColdRemovalReply finish(const ColdRemovalKey &candidate, uint64_t confirmations)
    {
        if (!started()) return reply(ColdRemovalResult::unavailable);
        if (!same_cold_removal_key(candidate, key)) return reply(ColdRemovalResult::stale_key);
        if (confirmations != ColdRemovalRequiredConfirmations)
            return reply(ColdRemovalResult::bad_argument);
        if (inFlight || remaining()) return reply(ColdRemovalResult::busy);
        if (!closed()) {
            if (commitSequence == UINT64_MAX) return reply(ColdRemovalResult::capacity);
            ++commitSequence; // FINISH has its own durable, nonzero receipt.
            state = ColdRemovalLedgerState::closed;
        }
        return reply(ColdRemovalResult::success, 0, commitSequence);
    }
};
}
#endif
