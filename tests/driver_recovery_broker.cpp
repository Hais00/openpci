// CPU-only actual private registry/ledger fault fixture. No DriverKit or hardware.
// Usage: driver_recovery_broker /absolute/path/to/ext-GPU
// Assertions execute the fault scenarios, including in RelWithDebInfo builds.
#ifdef NDEBUG
#undef NDEBUG
#endif
#define EXTGPU_RECOVERY_BROKER_CPU_ONLY 1
#include "../driver/privateRecoveryBrokerSupport.hpp"
#include <cassert>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

using namespace extgpu_driver;
struct Fixture {
    RecoveryBrokerRegistry registry;
    ColdRemovalKey key{11,12,13,{14,15}};
    uint64_t cap[2]{16,17};
    bool tableLocked=false, operationLocked=false;
    std::map<uintptr_t,unsigned> refs{{1,1},{2,1}};
    unsigned retains=0,releases=0;
    void retain(uintptr_t original) { assert(tableLocked);++refs[original];++retains; }
    void drop(uintptr_t original) {
        if(!original) return;
        assert(!tableLocked && !operationLocked && refs[original]>0);
        --refs[original];++releases;
    }
    RecoveryBrokerResult prepare(uintptr_t original=1,uint32_t count=2) {
        tableLocked=true;
        auto result=registry.prepare(original,100,key,cap,count,[&](uintptr_t object){retain(object);});
        tableLocked=false;return result;
    }
    void publish(uintptr_t original=1) { registry.find(original)->published=true; }
    RecoveryBrokerResult bind(uintptr_t owner=20,uint64_t incarnation=100) {
        uint32_t count=999;
        auto result=registry.bind(owner,incarnation,key,cap,&count);
        if(result==RecoveryBrokerResult::success) assert(count==registry.find(1)->count);
        return result;
    }
    RecoveryBrokerLease acquire(uintptr_t owner=20) {
        tableLocked=true;
        auto lease=registry.acquire(owner,100,key,[&](uintptr_t original){retain(original);});
        tableLocked=false;return lease;
    }
    void release(const RecoveryBrokerLease &lease,bool finished=false) {
        tableLocked=true;auto drops=registry.release(lease,finished);tableLocked=false;
        drop(drops.call);drop(drops.escrow);
    }
    void close(uintptr_t owner=20) {
        tableLocked=true;auto original=registry.closeOwner(owner);tableLocked=false;drop(original);
    }
};

static void escrow_before_publish_and_exact_retry() {
    Fixture f;
    assert(f.prepare()==RecoveryBrokerResult::success);
    assert(f.refs[1]==2 && f.retains==1 && !f.registry.find(1)->published);
    assert(f.bind()==RecoveryBrokerResult::unavailable);
    f.publish();assert(f.prepare()==RecoveryBrokerResult::success);
    assert(f.refs[1]==2 && f.retains==1); // Lost PREPARE reply does not retain twice.
    assert(f.bind()==RecoveryBrokerResult::success);
    assert(f.bind()==RecoveryBrokerResult::success); // Same-connection lost BIND reply.
    assert(f.refs[1]==2 && f.registry.find(1)->owner==20);
}
static void capability_owner_key_and_incarnation() {
    Fixture f;assert(f.prepare()==RecoveryBrokerResult::success);f.publish();
    uint32_t count=99;uint64_t bad[2]{16,18};
    assert(f.registry.bind(20,100,f.key,bad,&count)==RecoveryBrokerResult::denied);
    assert(count==99);assert(f.bind(20,101)==RecoveryBrokerResult::denied);
    auto wrong=f.key;++wrong.generation;
    assert(f.registry.bind(20,100,wrong,f.cap,&count)==RecoveryBrokerResult::not_found);
    assert(f.bind()==RecoveryBrokerResult::success);
    assert(f.bind(21)==RecoveryBrokerResult::denied); // Entitlement equality is not ownership.
    assert(f.acquire(21).result==RecoveryBrokerResult::denied);
    assert(f.prepare(2)==RecoveryBrokerResult::denied); // Duplicate key/cap cannot adopt.
    f.cap[0]=f.key.nonce[0];f.cap[1]=f.key.nonce[1];
    assert(f.prepare(2)==RecoveryBrokerResult::bad_argument); // Private cap != public nonce.
    assert(f.refs[2]==1);
}
static void callback_pin_stop_and_failed_completion_retain() {
    Fixture f;f.prepare();f.publish();f.bind();
    auto lease=f.acquire();assert(lease.result==RecoveryBrokerResult::success && f.refs[1]==3);
    // Actual callback is invoked only after dropping the table lock.
    assert(!f.tableLocked);f.operationLocked=true;
    bool prepared=true,frozen=true,bmeACK=false;
    assert(!(prepared&&frozen&&bmeACK)); // Failed BEGIN has no authoritative snapshot ACK.
    f.operationLocked=false;
    f.release(lease,false);assert(f.refs[1]==2 && !f.registry.find(1)->closed);
    f.close();assert(f.refs[1]==2 && f.registry.occupied()); // Stop never auto-unpins.
    assert(f.acquire().result==RecoveryBrokerResult::denied);
    assert(f.bind(21)==RecoveryBrokerResult::denied); // No reconnect after unfinished owner loss.
}
static void finish_receipts_lifetime_and_close_during_callback() {
    Fixture f;f.prepare();f.publish();f.bind();
    auto lease=f.acquire();f.release(lease,true);
    assert(f.registry.find(1)->closed && f.refs[1]==2); // FINISH reply may be lost.
    auto retry=f.acquire();assert(retry.result==RecoveryBrokerResult::success);
    f.close();assert(f.refs[1]==3 && f.registry.find(1)->calls==1);
    f.release(retry,true);assert(f.refs[1]==1 && !f.registry.occupied());
    f.close();assert(f.refs[1]==1); // No double release on Stop+free.
}
static void ledger_native_failure_and_immutable_tombstones() {
    Fixture f;f.prepare();f.publish();f.bind();
    ColdRemovalEntry entries[2]{};entries[0].map={0x1000,0x4000,1};entries[1].map={0x9000,0x4000,2};
    ColdRemovalLedger ledger{};
    assert(ledger.arm(f.key,{11,12,13},entries,2,2)==ColdRemovalResult::success);
    struct Operations {
        unsigned calls=0,commands=0,descriptors=0;bool fail=true;
        int32_t complete(ColdRemovalEntry &) {++calls;return fail?-77:0;}
        void release_command(ColdRemovalEntry &e) {assert(e.state==ColdRemovalEntryState::completed_refs_held);++commands;}
        void release_descriptor(ColdRemovalEntry &e) {assert(e.state==ColdRemovalEntryState::completed_refs_held);++descriptors;}
    } operations;
    auto lease=f.acquire();
    auto failed=ledger.retire(f.key,0x1000,0x4000,3,operations);
    assert(failed.result==ColdRemovalResult::completion_unknown && failed.nativeStatus==-77);
    f.release(lease,false);assert(f.refs[1]==2 && operations.commands==0 && operations.descriptors==0);
    operations.fail=false;auto a=ledger.retire(f.key,0x1000,0x4000,3,operations);
    assert(a.result==ColdRemovalResult::success && a.commitSequence>0);
    auto duplicate=ledger.retire(f.key,0x1000,0x4000,3,operations);
    assert(duplicate.commitSequence==a.commitSequence && operations.calls==2);
    assert(ledger.finish(f.key,3).result!=ColdRemovalResult::success);
    auto b=ledger.retire(f.key,0x9000,0x4000,3,operations);assert(b.result==ColdRemovalResult::success);
    auto finished=ledger.finish(f.key,3);assert(finished.result==ColdRemovalResult::success);
    auto finishRetry=ledger.finish(f.key,3);assert(finishRetry.commitSequence==finished.commitSequence);
    assert(ledger.retire(f.key,0x1000,0x4000,3,operations).commitSequence==a.commitSequence);
    assert(operations.commands==2 && operations.descriptors==2);
    lease=f.acquire();f.release(lease,true);f.close();assert(f.refs[1]==1);
}
static void bounded_capacity_and_successful_slot_reuse() {
    Fixture f;
    for(uint32_t i=0;i<RecoveryBrokerRegistry::capacity;++i) {
        auto key=f.key;key.generation+=i;
        uint64_t cap[2]{1000+i,2000+i};
        assert(f.registry.prepare(100+i,100,key,cap,1,[](uintptr_t){})==RecoveryBrokerResult::success);
    }
    auto key=f.key;key.generation+=1000;uint64_t cap[2]{9000,9001};
    assert(f.registry.prepare(999,100,key,cap,1,[](uintptr_t){})==RecoveryBrokerResult::capacity);
    Fixture reuse;
    for(unsigned i=0;i<200;++i) {
        assert(reuse.prepare()==RecoveryBrokerResult::success);reuse.publish();reuse.bind();
        auto lease=reuse.acquire();reuse.release(lease,true);reuse.close();
        assert(!reuse.registry.occupied() && reuse.refs[1]==1);
    }
}
static std::string read(const std::string &path) {
    std::ifstream input(path);assert(input.good());std::ostringstream out;out<<input.rdbuf();return out.str();
}
static std::string body(const std::string &source,const std::string &name) {
    auto at=source.find(name);assert(at!=std::string::npos);at=source.find('{',at);assert(at!=std::string::npos);
    size_t end=at;unsigned depth=0;
    do {if(source[end]=='{')++depth;else if(source[end]=='}')--depth;++end;} while(depth && end<source.size());
    assert(!depth);return source.substr(at,end-at);
}
static void actual_source_recovery_boundaries(const std::string &root) {
    auto source=read(root+"/driver/VFIOUserPCIDriver.cpp");
    auto local=body(source,"kern_return_t ready_recovery_local_call(");
    assert(local.find("ready_cold_begin_reply(")==std::string::npos);
    assert(local.find("GetProvider(")==std::string::npos && local.find("ConfigurationRead")==std::string::npos);
    assert(local.find("PrepareForDMA(")==std::string::npos);
    assert(local.find("ReadyProviderLock operation")<local.find("ready_cold_retire("));
    assert(local.find("ready_cold_finish(")!=std::string::npos);
    auto prepare=body(source,"static kern_return_t ready_cold_prepare(");
    assert(prepare.find("ready_cold_disable_bus_master(")==std::string::npos);
    assert(prepare.find("ColdRemovalLedger prepared{};")!=std::string::npos);
    assert(prepare.find("ready_recovery_reserve(")<prepare.find("state->coldPrepared = true"));
    assert(prepare.find("state->coldPrepared = true")<prepare.find("ready_recovery_publish("));
    auto stop=body(source,"IMPL(VFIOUserPCIDriverUserClient, Stop)");
    assert(stop.find("state->coldPrepared || state->coldFrozen")!=std::string::npos);
    auto broker=read(root+"/driver/VFIOUserRecoveryBroker.cpp");
    assert(broker.find("GetProvider(")==std::string::npos && broker.find("PrepareForDMA(")==std::string::npos);
    assert(broker.find("ConfigurationRead")==std::string::npos && broker.find("ConfigurationWrite")==std::string::npos);
    assert(broker.find("IOPCIDevice")==std::string::npos && broker.find("CompleteDMA(")==std::string::npos);
    assert(broker.find("ready_recovery_local_call(")!=std::string::npos);
}
int main(int argc,char **argv) {
    escrow_before_publish_and_exact_retry();capability_owner_key_and_incarnation();
    callback_pin_stop_and_failed_completion_retain();finish_receipts_lifetime_and_close_during_callback();
    ledger_native_failure_and_immutable_tombstones();bounded_capacity_and_successful_slot_reuse();
    if(argc!=2) {std::cerr<<"repository root argument required\n";return 2;}
    actual_source_recovery_boundaries(argv[1]);
    std::cout<<"PASS recovery broker: escrow/capability/owner/key/stop/failure/receipts/capacity/source boundaries\n";
}
