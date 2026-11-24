#pragma once
#include <stdint.h>
#include "ColdRemovalLedger.hpp"

namespace extgpu_driver {
/* Same-process metadata only. All calls require the broker table lock. No
 * callback, completion, reference release or original operation lock here. */
enum class RecoveryBrokerResult { success, bad_argument, unavailable, capacity, denied, busy, not_found };
struct RecoveryBrokerEntry {
    uintptr_t original, owner;
    uint64_t brokerID;
    ColdRemovalKey key;
    uint64_t capability[2];
    uint32_t count, calls;
    bool published, disconnected, closed;
};
struct RecoveryBrokerLease {
    RecoveryBrokerEntry *entry;
    uintptr_t original;
    RecoveryBrokerResult result;
};
struct RecoveryBrokerDrop { uintptr_t call, escrow; };
class RecoveryBrokerRegistry {
public:
    static constexpr uint32_t capacity = 64;
    RecoveryBrokerEntry entries[capacity]{};
    bool occupied() const {
        for (const auto &e : entries) if (e.original) return true;
        return false;
    }
    RecoveryBrokerEntry *find(uintptr_t original) {
        for (auto &e : entries) if (e.original == original && original) return &e;
        return nullptr;
    }
    static bool secret(const RecoveryBrokerEntry &e, const uint64_t cap[2]) {
        return cap && e.capability[0] == cap[0] && e.capability[1] == cap[1];
    }
    template<class Retain>
    RecoveryBrokerResult prepare(uintptr_t original, uint64_t brokerID,
        const ColdRemovalKey &key, const uint64_t cap[2], uint32_t count, Retain retain) {
        if (!original || !brokerID || !valid_cold_removal_key(key) || !cap ||
            !(cap[0] | cap[1]) || (cap[0] == key.nonce[0] && cap[1] == key.nonce[1]) ||
            count > 4096) return RecoveryBrokerResult::bad_argument;
        if (auto *e = find(original))
            return e->brokerID == brokerID && same_cold_removal_key(e->key,key) && secret(*e,cap) &&
                e->count == count && !e->disconnected ? RecoveryBrokerResult::success : RecoveryBrokerResult::denied;
        RecoveryBrokerEntry *empty = nullptr;
        for (auto &e : entries) {
            if (!e.original) { if (!empty) empty=&e; continue; }
            if (same_cold_removal_key(e.key,key) || secret(e,cap)) return RecoveryBrokerResult::denied;
        }
        if (!empty) return RecoveryBrokerResult::capacity;
        retain(original); // Strong escrow reference BEFORE table publication/freeze.
        *empty={original,0,brokerID,key,{cap[0],cap[1]},count,0,false,false,false};
        return RecoveryBrokerResult::success;
    }
    RecoveryBrokerResult bind(uintptr_t owner, uint64_t brokerID, const ColdRemovalKey &key,
                               const uint64_t cap[2], uint32_t *count) {
        if (!owner || !count) return RecoveryBrokerResult::bad_argument;
        for (auto &e : entries) {
            if (!e.original || !same_cold_removal_key(e.key,key)) continue;
            if (!e.published) return RecoveryBrokerResult::unavailable;
            if (e.brokerID != brokerID || !secret(e,cap) || e.disconnected ||
                (e.owner && e.owner != owner)) return RecoveryBrokerResult::denied;
            for (const auto &other : entries)
                if (other.original && &other != &e && other.owner == owner) return RecoveryBrokerResult::denied;
            e.owner=owner; *count=e.count; return RecoveryBrokerResult::success;
        }
        return RecoveryBrokerResult::not_found;
    }
    template<class Retain>
    RecoveryBrokerLease acquire(uintptr_t owner, uint64_t brokerID, const ColdRemovalKey &key, Retain retain) {
        for (auto &e : entries) if (e.original && e.owner == owner && e.brokerID == brokerID &&
                same_cold_removal_key(e.key,key) && e.published && !e.disconnected) {
            if (e.calls == UINT32_MAX) return {nullptr,0,RecoveryBrokerResult::busy};
            retain(e.original); ++e.calls;
            return {&e,e.original,RecoveryBrokerResult::success};
        }
        return {nullptr,0,RecoveryBrokerResult::denied};
    }
    RecoveryBrokerDrop release(const RecoveryBrokerLease &lease, bool completed) {
        auto &e=*lease.entry;
        if (completed) e.closed=true;
        --e.calls;
        RecoveryBrokerDrop drop{lease.original,0};
        if (e.closed && e.disconnected && !e.calls) { drop.escrow=e.original; e={}; }
        return drop; // Caller releases BOTH references after dropping registry lock.
    }
    uintptr_t closeOwner(uintptr_t owner) {
        for (auto &e : entries) if (e.original && e.owner == owner) {
            e.disconnected=true;
            if (e.closed && !e.calls) { auto original=e.original; e={}; return original; }
            return 0; // Unfinished or in-flight: preserve bounded escrow, never auto-drain.
        }
        return 0;
    }
};
}

#ifndef EXTGPU_RECOVERY_BROKER_CPU_ONLY
#include <DriverKit/IOTypes.h>
class VFIOUserPCIDriverUserClient;
kern_return_t ready_recovery_endpoint(uint64_t *brokerID);
kern_return_t ready_recovery_reserve(VFIOUserPCIDriverUserClient *, const uint64_t key[5],
                                    const uint64_t cap[2], uint32_t count, uint64_t *brokerID);
void ready_recovery_publish(VFIOUserPCIDriverUserClient *);
kern_return_t ready_recovery_prepared_reply(VFIOUserPCIDriverUserClient *, const uint64_t key[5],
                                           const uint64_t cap[2], uint64_t *brokerID);
bool ready_recovery_is_bound(VFIOUserPCIDriverUserClient *, const uint64_t key[5]);
/* Defined beside the original DMA ledger. Takes original operationLock, never
 * GetProvider/ordinary ExternalMethod, PCI/config, BME retry or PrepareForDMA. */
kern_return_t ready_recovery_local_call(VFIOUserPCIDriverUserClient *, uint64_t selector,
    const uint64_t *input, uint32_t inputCount, uint64_t *output, uint32_t *outputCount);
#endif
