#include <DriverKit/IOLib.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/OSMetaClass.h>
#include <extgpu/extgpu_driver_abi.h>
#include "VFIOUserRecoveryBroker.h"
#include "VFIOUserRecoveryUserClient.h"
#include "VFIOUserPCIDriverUserClient.h"
#include "privateRecoveryBrokerSupport.hpp"

struct VFIOUserRecoveryBroker_IVars { IODispatchQueue *queue; };
struct VFIOUserRecoveryUserClient_IVars {
    IOLock *lock;
    uint64_t brokerID;
    bool negotiated, stopped;
};
namespace {
IOLock *registryLock;
IOLock *tableLock() { return __atomic_load_n(&registryLock,__ATOMIC_ACQUIRE); }
VFIOUserRecoveryBroker *activeBroker;
uint64_t brokerID;
bool brokerReady;
extgpu_driver::RecoveryBrokerRegistry registry;
struct TableLock {
    TableLock() { IOLockLock(tableLock()); }
    ~TableLock() { IOLockUnlock(tableLock()); }
};
struct ClientLock {
    IOLock *lock;
    explicit ClientLock(IOLock *value):lock(value) { IOLockLock(lock); }
    ~ClientLock() { IOLockUnlock(lock); }
};
extgpu_driver::ColdRemovalKey key(const uint64_t *in) {
    return {in[0],in[1],in[2],{in[3],in[4]}};
}
kern_return_t result(extgpu_driver::RecoveryBrokerResult value) {
    using R=extgpu_driver::RecoveryBrokerResult;
    switch(value) {
    case R::success:return kIOReturnSuccess;
    case R::bad_argument:return kIOReturnBadArgument;
    case R::capacity:return kIOReturnNoSpace;
    case R::busy:return kIOReturnBusy;
    case R::denied:return kIOReturnNotPermitted;
    case R::not_found:return kIOReturnNotFound;
    case R::unavailable:return kIOReturnNotReady;
    }
    return kIOReturnError;
}
void retainOriginal(uintptr_t value) {
    reinterpret_cast<VFIOUserPCIDriverUserClient *>(value)->retain();
}
void releaseOriginal(uintptr_t value) {
    if(value) reinterpret_cast<VFIOUserPCIDriverUserClient *>(value)->release();
}
void closeOwner(VFIOUserRecoveryUserClient *owner) {
    if(!tableLock()) return;
    uintptr_t drop;
    { TableLock lock; drop=registry.closeOwner(reinterpret_cast<uintptr_t>(owner)); }
    releaseOriginal(drop); // free() can take original operationLock: outside table lock.
}
}

kern_return_t ready_recovery_endpoint(uint64_t *out) {
    if(!out || !tableLock()) return kIOReturnNotReady;
    TableLock lock;
    if(!brokerReady || !activeBroker || !brokerID) return kIOReturnNotReady;
    *out=brokerID;return kIOReturnSuccess;
}
kern_return_t ready_recovery_reserve(VFIOUserPCIDriverUserClient *original,const uint64_t words[5],
    const uint64_t cap[2],uint32_t count,uint64_t *out) {
    if(!original || !out || !tableLock()) return kIOReturnNotReady;
    TableLock lock;
    if(!brokerReady || !activeBroker || !brokerID) return kIOReturnNotReady;
    auto ret=registry.prepare(reinterpret_cast<uintptr_t>(original),brokerID,key(words),cap,count,retainOriginal);
    if(ret==extgpu_driver::RecoveryBrokerResult::success) *out=brokerID;
    return result(ret);
}
void ready_recovery_publish(VFIOUserPCIDriverUserClient *original) {
    TableLock lock;
    if(auto *entry=registry.find(reinterpret_cast<uintptr_t>(original))) entry->published=true;
}
kern_return_t ready_recovery_prepared_reply(VFIOUserPCIDriverUserClient *original,const uint64_t words[5],
    const uint64_t cap[2],uint64_t *out) {
    if(!tableLock() || !out) return kIOReturnNotReady;
    TableLock lock;
    auto *entry=registry.find(reinterpret_cast<uintptr_t>(original));
    if(!entry) return kIOReturnNotFound;
    if(!entry->published || entry->disconnected || !extgpu_driver::same_cold_removal_key(entry->key,key(words)) ||
       !extgpu_driver::RecoveryBrokerRegistry::secret(*entry,cap)) return kIOReturnNotPermitted;
    *out=entry->brokerID;return kIOReturnSuccess;
}
bool ready_recovery_is_bound(VFIOUserPCIDriverUserClient *original,const uint64_t words[5]) {
    if(!tableLock()) return false;
    TableLock lock;auto *entry=registry.find(reinterpret_cast<uintptr_t>(original));
    return brokerReady && entry && entry->published && entry->owner && !entry->disconnected &&
        entry->brokerID==brokerID && extgpu_driver::same_cold_removal_key(entry->key,key(words));
}

kern_return_t IMPL(VFIOUserRecoveryBroker, Start) {
    kern_return_t ret=Start(provider,SUPERDISPATCH);
    if(ret!=kIOReturnSuccess) return ret;
    ivars=IONewZero(VFIOUserRecoveryBroker_IVars,1);
    if(!ivars) return kIOReturnNoMemory;
    ret=IODispatchQueue::Create("OpenPCIRecoveryBroker",0,0,&ivars->queue);
    if(ret!=kIOReturnSuccess || !ivars->queue) return ret!=kIOReturnSuccess?ret:kIOReturnNoMemory;
    ret=SetDispatchQueue(kIOServiceDefaultQueueName,ivars->queue);
    if(ret!=kIOReturnSuccess) return ret;
    if(!tableLock()) {
        IOLock *created=IOLockAlloc(), *expected=nullptr;
        if(!created) return kIOReturnNoMemory;
        if(!__atomic_compare_exchange_n(&registryLock,&expected,created,false,__ATOMIC_RELEASE,__ATOMIC_ACQUIRE))
            IOLockFree(created);
    }
    uint64_t identity=0;ret=GetRegistryEntryID(&identity);
    if(ret!=kIOReturnSuccess || !identity) return ret!=kIOReturnSuccess?ret:kIOReturnError;
    {
        TableLock lock;
        if(activeBroker) return kIOReturnExclusiveAccess;
        activeBroker=this;brokerID=identity;brokerReady=false;
        retain(); // One process-lifetime anchor. Stop intentionally quarantines
        // this incarnation/queue, even when empty: restart requires a new DEXT
        // process. It never adopts escrow into a replacement broker service.
    }
    ret=RegisterService();
    { TableLock lock;brokerReady=(ret==kIOReturnSuccess); }
    return ret;
}
kern_return_t IMPL(VFIOUserRecoveryBroker, Stop) {
    if(tableLock()) { TableLock lock;if(activeBroker==this) brokerReady=false; }
    // No completion, teardown of escrow, provider RPC or PCI access here.
    return Stop(provider,SUPERDISPATCH);
}
kern_return_t IMPL(VFIOUserRecoveryBroker, NewUserClient) {
    if(type!=0 || !ivars) return kIOReturnUnsupported;
    uint64_t identity=0;if(ready_recovery_endpoint(&identity)!=kIOReturnSuccess) return kIOReturnNotReady;
    IOService *service=nullptr;
    kern_return_t ret=Create(this,"RecoveryUserClientProperties",&service);
    if(ret!=kIOReturnSuccess) return ret;
    auto *client=OSDynamicCast(VFIOUserRecoveryUserClient,service);
    if(!client) { if(service) service->release();return kIOReturnUnsupported; }
    *userClient=client;return kIOReturnSuccess;
}
kern_return_t IMPL(VFIOUserRecoveryUserClient, Start) {
    kern_return_t ret=Start(provider,SUPERDISPATCH);
    if(ret!=kIOReturnSuccess) return ret;
    auto *broker=OSDynamicCast(VFIOUserRecoveryBroker,provider);
    if(!broker || !tableLock()) return kIOReturnNotReady;
    ivars=IONewZero(VFIOUserRecoveryUserClient_IVars,1);
    if(!ivars) return kIOReturnNoMemory;
    ivars->lock=IOLockAlloc();if(!ivars->lock) return kIOReturnNoMemory;
    TableLock lock;
    if(!brokerReady || broker!=activeBroker || !brokerID) return kIOReturnNotReady;
    ivars->brokerID=brokerID;return kIOReturnSuccess;
}
kern_return_t IMPL(VFIOUserRecoveryUserClient, Stop) {
    if(ivars && ivars->lock) {
        ClientLock lock(ivars->lock);ivars->stopped=true;closeOwner(this);
    }
    return Stop(provider,SUPERDISPATCH);
}
void VFIOUserRecoveryUserClient::free() {
    if(ivars) {
        closeOwner(this); // Idempotent; unfinished escrow is deliberately retained.
        if(ivars->lock) IOLockFree(ivars->lock);
        IOSafeDeleteNULL(ivars,VFIOUserRecoveryUserClient_IVars,1);
    }
    IOUserClient::free();
}
kern_return_t IMPL(VFIOUserRecoveryUserClient, CopyClientMemoryForType) {
    (void)type;(void)options;
    if(memory) *memory=nullptr;
    return kIOReturnUnsupported;
}
kern_return_t VFIOUserRecoveryUserClient::ExternalMethod(uint64_t selector,IOUserClientMethodArguments *arguments,
    const IOUserClientMethodDispatch *dispatch,OSObject *target,void *reference) {
    (void)dispatch;(void)target;(void)reference;
    if(!ivars || !ivars->lock || !arguments || !tableLock()) return kIOReturnNotAttached;
    ClientLock operation(ivars->lock);
    if(ivars->stopped) return kIOReturnNotAttached;
    { TableLock lock;if(!brokerReady || ivars->brokerID!=brokerID) return kIOReturnNotReady; }
    auto *a=arguments;
    if(selector==EXTGPU_SELECTOR_QUERY_PROTOCOL) {
        if(!a->scalarInput || a->scalarInputCount!=EXTGPU_QUERY_INPUT_COUNT || !a->scalarOutput ||
           a->scalarOutputCount!=EXTGPU_QUERY_OUTPUT_COUNT) return kIOReturnBadArgument;
        ivars->negotiated=false;
        if(a->scalarInput[0]!=EXTGPU_PROTOCOL_MAJOR || a->scalarInput[1]>EXTGPU_PROTOCOL_MINOR ||
           a->scalarInput[2]!=EXTGPU_PROTOCOL_FINGERPRINT) return kIOReturnUnsupported;
        const uint64_t out[8]={EXTGPU_PROTOCOL_MAJOR,EXTGPU_PROTOCOL_MINOR,ivars->brokerID,
            EXTGPU_RECOVERY_BROKER_CAPABILITIES,EXTGPU_MAX_DMA_REGIONS,EXTGPU_MAX_PREPARED_DMA_BYTES,
            EXTGPU_MAX_RETURNED_SEGMENTS,EXTGPU_PROTOCOL_FINGERPRINT};
        for(unsigned i=0;i<8;++i) a->scalarOutput[i]=out[i];
        a->scalarOutputCount=8;ivars->negotiated=true;return kIOReturnSuccess;
    }
    if(!ivars->negotiated) return kIOReturnNotPermitted;
    if(selector==EXTGPU_SELECTOR_BIND_RECOVERY_ESCROW) {
        if(!a->scalarInput || a->scalarInputCount!=EXTGPU_RECOVERY_ESCROW_INPUT_COUNT || !a->scalarOutput ||
           a->scalarOutputCount!=EXTGPU_RECOVERY_BIND_OUTPUT_COUNT) return kIOReturnBadArgument;
        uint32_t count=0;extgpu_driver::RecoveryBrokerResult bound;
        { TableLock lock;bound=registry.bind(reinterpret_cast<uintptr_t>(this),ivars->brokerID,key(a->scalarInput),a->scalarInput+5,&count); }
        if(bound!=extgpu_driver::RecoveryBrokerResult::success) return result(bound);
        for(unsigned i=0;i<5;++i) a->scalarOutput[i]=a->scalarInput[i];
        a->scalarOutput[5]=count;a->scalarOutput[6]=EXTGPU_COLD_STATE_BOUND;
        a->scalarOutputCount=7;return kIOReturnSuccess;
    }
    uint32_t inputs=0,outputs=0;
    if(selector==EXTGPU_SELECTOR_COLD_BEGIN) { inputs=5;outputs=7; }
    else if(selector==EXTGPU_SELECTOR_COLD_RETIRE) { inputs=8;outputs=10; }
    else if(selector==EXTGPU_SELECTOR_COLD_FINISH) { inputs=6;outputs=7; }
    else return kIOReturnUnsupported;
    if(!a->scalarInput || a->scalarInputCount!=inputs || !a->scalarOutput || a->scalarOutputCount!=outputs)
        return kIOReturnBadArgument;
    extgpu_driver::RecoveryBrokerLease lease;
    { TableLock lock;lease=registry.acquire(reinterpret_cast<uintptr_t>(this),ivars->brokerID,key(a->scalarInput),retainOriginal); }
    if(lease.result!=extgpu_driver::RecoveryBrokerResult::success) return result(lease.result);
    /* No registry lock held across original operationLock or CompleteDMA. */
    kern_return_t ret=ready_recovery_local_call(reinterpret_cast<VFIOUserPCIDriverUserClient *>(lease.original),
        selector,a->scalarInput,inputs,a->scalarOutput,&outputs);
    extgpu_driver::RecoveryBrokerDrop drop;
    { TableLock lock;drop=registry.release(lease,selector==EXTGPU_SELECTOR_COLD_FINISH && ret==kIOReturnSuccess); }
    releaseOriginal(drop.call);releaseOriginal(drop.escrow);
    if(ret==kIOReturnSuccess) a->scalarOutputCount=outputs;
    return ret;
}
