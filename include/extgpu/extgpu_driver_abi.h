#ifndef EXTGPU_DRIVER_ABI_H
#define EXTGPU_DRIVER_ABI_H
#include <stdint.h>
#define EXTGPU_DRIVER_CLASS "VFIOUserPCIDriver"
#define EXTGPU_DRIVER_BUNDLE_ID "org.openpci.driver"
#define EXTGPU_RECOVERY_BROKER_CLASS "VFIOUserRecoveryBroker"
#define EXTGPU_PROTOCOL_MAJOR UINT64_C(1)
#define EXTGPU_PROTOCOL_MINOR UINT64_C(30)
/* Cold recovery is optional; ordinary SDK operations retain v1.1 support. */
#define EXTGPU_PROTOCOL_MINIMUM_MINOR UINT64_C(1)
#define EXTGPU_PROTOCOL_FINGERPRINT UINT64_C(0x4558475055310001)
#define EXTGPU_QUERY_INPUT_COUNT 3u
#define EXTGPU_QUERY_OUTPUT_COUNT 8u
/* QueryProtocol inputs: expectedMajor, minimumMinor, ABI fingerprint.
 * Outputs: major, minor, per-provider generation, feature/state bits,
 * maxDMARegions, maxPreparedDMABytes, maxReturnedSegments, fingerprint.
 * A successful exact handshake is required before all other operations.
 * Feature bits describe implemented APIs, not successful hardware tests.
 */
enum {
 EXTGPU_SELECTOR_GET_IDENTITY=0, EXTGPU_SELECTOR_CLAIM=1,
 EXTGPU_SELECTOR_TERMINATE=2, /* Deprecated: refused, never service-terminate. */
 EXTGPU_SELECTOR_ALLOCATE_DMA_BUFFER=3, EXTGPU_SELECTOR_FREE_DMA_BUFFER=4,
 EXTGPU_SELECTOR_REGISTER_DMA_REGION=5, EXTGPU_SELECTOR_UNREGISTER_DMA_REGION=6,
 EXTGPU_SELECTOR_PROBE_DMA_REGION=7, EXTGPU_SELECTOR_CONFIG_READ=8,
 EXTGPU_SELECTOR_CONFIG_WRITE=9, EXTGPU_SELECTOR_GET_BAR_INFO=10,
 EXTGPU_SELECTOR_MMIO_READ=11, EXTGPU_SELECTOR_MMIO_WRITE=12,
 EXTGPU_SELECTOR_SETUP_INTERRUPTS=13, EXTGPU_SELECTOR_CHECK_INTERRUPT=14,
 EXTGPU_SELECTOR_WAIT_INTERRUPT=15, EXTGPU_SELECTOR_SET_IRQ_MASK=16,
 EXTGPU_SELECTOR_RESET_DEVICE=17, EXTGPU_SELECTOR_QUERY_PROTOCOL=18,
 EXTGPU_SELECTOR_RELEASE_OWNER=19, EXTGPU_SELECTOR_SET_DMA_MASK=20,
 EXTGPU_SELECTOR_RETIRE_INTERRUPTS=21,
 EXTGPU_SELECTOR_COLD_BEGIN=22, EXTGPU_SELECTOR_COLD_RETIRE=23,
 EXTGPU_SELECTOR_COLD_FINISH=24,
 EXTGPU_SELECTOR_GET_RECOVERY_ENDPOINT=25,
 EXTGPU_SELECTOR_PREPARE_RECOVERY_ESCROW=26,
 EXTGPU_SELECTOR_BIND_RECOVERY_ESCROW=27,
 EXTGPU_SELECTOR_WARM_RELEASE_VM_FLR=28,
 EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA=29,
 EXTGPU_SELECTOR_PROBE_VEGA_PERST=30,
 EXTGPU_SELECTOR_VM_FLR_PREFLIGHT=31,
 EXTGPU_SELECTOR_RESTORE_VEGA_BARS=32,
 EXTGPU_SELECTOR_BAR_WORDS=33,
 EXTGPU_SELECTOR_VEGA_RELEASE_RECEIPT=34,
 EXTGPU_SELECTOR_WARM_RELEASE_VM_RTX3090=35,
 EXTGPU_SELECTOR_RTX3090_RELEASE_RECEIPT=36,
 EXTGPU_SELECTOR_WARM_RELEASE_VM_POLARIS=37,
 EXTGPU_SELECTOR_POLARIS_RELEASE_RECEIPT=38,
 EXTGPU_SELECTOR_MMIO_WINDOW=39,
 EXTGPU_SELECTOR_IRQ_DRAIN_AND_REARM=41, EXTGPU_SELECTOR_IRQ_SUBSCRIBE_SHARED=42,
 EXTGPU_SELECTOR_SET_DMA_BUDGET=43, EXTGPU_SELECTOR_REGISTER_VM_DMA=44,
 EXTGPU_SELECTOR_GET_VM_DMA_SEGMENT=45, EXTGPU_SELECTOR_ENABLE_VM_DYNAMIC_DMA=46,
 EXTGPU_SELECTOR_REGISTER_VM_DYNAMIC_DMA=47, EXTGPU_SELECTOR_MMIO_RING_KICK=48,
 EXTGPU_SELECTOR_VM_GROUP_PREFLIGHT=49,
 EXTGPU_SELECTOR_ARM_SURPRISE_REMOVAL=50, EXTGPU_SELECTOR_RELEASE_SURPRISE_REMOVAL=51,
 EXTGPU_SELECTOR_REGISTER_VM_DMA_CONSTRAINED=52
};
#define EXTGPU_SELECTOR_SET_DMA_ADDRESS_BITS EXTGPU_SELECTOR_SET_DMA_MASK
enum {
 EXTGPU_MEMORY_DMA_BUFFER=0, EXTGPU_MEMORY_BAR0=1, EXTGPU_MEMORY_BAR1=2,
 EXTGPU_MEMORY_BAR2=3, EXTGPU_MEMORY_BAR3=4, EXTGPU_MEMORY_BAR4=5,
 EXTGPU_MEMORY_BAR5=6, EXTGPU_MEMORY_IRQ_STATE=7, EXTGPU_MEMORY_MMIO_WINDOW=8,
 EXTGPU_MEMORY_IRQ_MAILBOX=9, EXTGPU_MEMORY_MMIO_RING=10
};
#define EXTGPU_CAP_CONFIG_READ (UINT64_C(1)<<0)
#define EXTGPU_CAP_CONFIG_WRITE (UINT64_C(1)<<1)
#define EXTGPU_CAP_BAR (UINT64_C(1)<<2)
#define EXTGPU_CAP_DMA_ALLOCATE (UINT64_C(1)<<3)
#define EXTGPU_CAP_DMA_REGISTER (UINT64_C(1)<<4)
#define EXTGPU_CAP_IRQ_ASYNC (UINT64_C(1)<<5)
#define EXTGPU_CAP_FLR_API (UINT64_C(1)<<6)
#define EXTGPU_CAP_EXCLUSIVE_CLAIM (UINT64_C(1)<<7)
#define EXTGPU_CAP_GENERATION (UINT64_C(1)<<8)
#define EXTGPU_CAP_SAFE_RELEASE (UINT64_C(1)<<9)
#define EXTGPU_CAP_CONFIGURABLE_DMA_MASK (UINT64_C(1)<<10)
#define EXTGPU_CAP_IRQ_RETIRE (UINT64_C(1)<<11)
#define EXTGPU_CAP_COLD_REMOVAL (UINT64_C(1)<<12)
#define EXTGPU_CAP_RECOVERY_BROKER (UINT64_C(1)<<13)
#define EXTGPU_CAP_WARM_VM_FLR_RELEASE (UINT64_C(1)<<14)
#define EXTGPU_CAP_WARM_VM_VEGA_RELEASE (UINT64_C(1)<<15)
#define EXTGPU_CAP_VEGA_PERST_PROBE (UINT64_C(1)<<16)
#define EXTGPU_CAP_VM_FLR_PREFLIGHT (UINT64_C(1)<<17)
#define EXTGPU_CAP_RESTORE_VEGA_BARS (UINT64_C(1)<<18)
#define EXTGPU_CAP_BAR_WORDS (UINT64_C(1)<<19)
#define EXTGPU_CAP_VEGA_RELEASE_RECEIPT (UINT64_C(1)<<20)
#define EXTGPU_CAP_WARM_VM_STARTUP_RELEASE (UINT64_C(1)<<21)
#define EXTGPU_CAP_WARM_VM_RTX3090_RELEASE (UINT64_C(1)<<22)
#define EXTGPU_CAP_WARM_VM_POLARIS_RELEASE (UINT64_C(1)<<23)
#define EXTGPU_CAP_PREFETCHABLE_BAR_WORDS (UINT64_C(1)<<24)
#define EXTGPU_CAP_WARM_VM_NO_DMA_RELEASE (UINT64_C(1)<<25)
#define EXTGPU_CAP_MMIO_WINDOW (UINT64_C(1)<<26)
#define EXTGPU_CAP_IRQ_DRAIN_AND_REARM (UINT64_C(1)<<28)
#define EXTGPU_CAP_IRQ_SHARED_NOTIFICATION (UINT64_C(1)<<29)
#define EXTGPU_CAP_CONFIGURABLE_DMA_BUDGET (UINT64_C(1)<<30)
#define EXTGPU_CAP_VM_DMA_SEGMENTS (UINT64_C(1)<<31)
#define EXTGPU_CAP_VM_DYNAMIC_DMA (UINT64_C(1)<<32)
#define EXTGPU_CAP_MMIO_WINDOW_WORD_WRITES (UINT64_C(1)<<33)
#define EXTGPU_MMIO_WINDOW_WORD_WRITES_MINIMUM_MINOR UINT64_C(22)
#define EXTGPU_CAP_MMIO_RING (UINT64_C(1)<<34)
#define EXTGPU_MMIO_RING_MINIMUM_MINOR UINT64_C(23)
#define EXTGPU_CAP_MMIO_RING_POLL (UINT64_C(1)<<35)
#define EXTGPU_MMIO_RING_POLL_MINIMUM_MINOR UINT64_C(24)
#define EXTGPU_CAP_MMIO_RING_ADAPTIVE (UINT64_C(1)<<36)
#define EXTGPU_MMIO_RING_ADAPTIVE_MINIMUM_MINOR UINT64_C(25)
#define EXTGPU_CAP_MMIO_RING_IRQ_ARM (UINT64_C(1)<<37)
#define EXTGPU_MMIO_RING_IRQ_ARM_MINIMUM_MINOR UINT64_C(26)
#define EXTGPU_CAP_MMIO_RING_EVENT (UINT64_C(1)<<38)
#define EXTGPU_MMIO_RING_EVENT_MINIMUM_MINOR UINT64_C(27)
#define EXTGPU_CAP_MMIO_RING_GROUPED_STORES (UINT64_C(1)<<39)
#define EXTGPU_MMIO_RING_GROUPED_STORES_MINIMUM_MINOR UINT64_C(28)
#define EXTGPU_CAP_VM_GROUP_PREFLIGHT (UINT64_C(1)<<40)
#define EXTGPU_CAP_SURPRISE_REMOVAL (UINT64_C(1)<<41)
#define EXTGPU_CAP_VM_DMA_CONSTRAINED (UINT64_C(1)<<42)
#define EXTGPU_DMA_BUDGET_MINIMUM_MINOR UINT64_C(19)
#define EXTGPU_VM_DMA_MINIMUM_MINOR UINT64_C(20)
#define EXTGPU_VM_DYNAMIC_DMA_MINIMUM_MINOR UINT64_C(21)
#define EXTGPU_VM_DMA_SEGMENT_LIMIT 32u
/* Register VM DMA uses the legacy 3-word input and returns segment count,
 * first bus address and total coverage. Read a segment on the same claim
 * with generation, registration key and index. The reply is bus and bytes. */
/* SetDMA Budget request/reply: original generation and byte budget. */
#define EXTGPU_DMA_BUDGET_WORDS 2u
#define EXTGPU_CAP_DMA_ADDRESS_BITS EXTGPU_CAP_CONFIGURABLE_DMA_MASK
#define EXTGPU_STATE_QUARANTINED (UINT64_C(1)<<63)
#define EXTGPU_MAX_DMA_REGIONS UINT64_C(4096)
#define EXTGPU_DEFAULT_DMA_BUDGET_BYTES (UINT64_C(512)*1024*1024)
/* Limit advertised by pre-1.19 drivers and by the recovery broker. */
#define EXTGPU_MAX_PREPARED_DMA_BYTES EXTGPU_DEFAULT_DMA_BUDGET_BYTES
#define EXTGPU_MAX_RETURNED_SEGMENTS UINT64_C(1)
#define EXTGPU_IRQ_BITMAP_WORDS 4u
/* Function-scoped warm retirement of a single registered guest RAM window.
 * Request: generation, registration key, registered bytes.
 * Reply: exact request words followed by CLOSED. The driver records a
 * tombstone on the original connection for lost-reply replay. No group
 * reset is implied. Once armed, failure retains the matching window and
 * refuses ordinary commands. UNSUPPORTED is reserved for preflight.
 * STARTUP_RELEASE also accepts the original first registration before guest
 * access, including a retained incomplete or already rolled back window.
 * NO_DMA_RELEASE accepts key=0 and bytes=0 only before any DMA request on
 * the same claim. It uses the same reset scope and closed receipt. */
#define EXTGPU_WARM_VM_REQUEST_WORDS 3u
#define EXTGPU_WARM_VM_REPLY_WORDS 4u
#define EXTGPU_WARM_VM_CLOSED UINT64_C(1)
/* FLR admission on a pristine exclusive claim. Request current generation.
 * Reply exact generation, provider PCI registry ID, and physical FLR bit. */
#define EXTGPU_FLR_PREFLIGHT_REPLY_WORDS 3u
/* Restore one idle Vega function's memory BARs from its IOKit assignments.
 * No caller-supplied addresses. Echo generation and provider ID on success. */
#define EXTGPU_RESTORE_VEGA_BARS_WORDS 2u
/* BAR_WORDS inputs: memory index, byte offset, op, word count, then
 * WRITE payload or one FILL word. READ outputs exactly count words.
 * PREFETCHABLE_BAR_WORDS extends the same request beyond BAR0. */
#define EXTGPU_BAR_WORDS_HEADER 4u
/* Vega group request: the FLR key, then bridge, display and audio PCI
 * registry IDs from the trusted host's immediate IOKit topology snapshot.
 * Reply echoes all six words then CLOSED. Exact scope is part of replay. */
#define EXTGPU_WARM_VEGA_REQUEST_WORDS 6u
#define EXTGPU_WARM_VEGA_REPLY_WORDS 7u
/* Read-only group preflight: generation, release selector, bridge, display, audio. */
#define EXTGPU_VM_GROUP_PREFLIGHT_WORDS 5u
#define EXTGPU_RTX3090_VENDOR 0x10deu
#define EXTGPU_RTX3090_DISPLAY 0x2204u
#define EXTGPU_RTX3090_AUDIO 0x1aefu
#define EXTGPU_POLARIS_VENDOR 0x1002u
#define EXTGPU_POLARIS_DISPLAY 0x67dfu
#define EXTGPU_POLARIS_DISPLAY_2048SP 0x6fdfu
#define EXTGPU_POLARIS_AUDIO 0xaaf0u
/* The RTX 3090 and Polaris selectors use the same six-word request and seven-word
 * success reply as Vega. The selector is part of the original ledger. */
/* VEGA_RELEASE_RECEIPT uses the same request and success reply as the
 * active Vega release. It can only read the exact closed ledger on the
 * original user client and does not consult or mutate any provider. */
/* Idle Vega PERST# probe: same six request words with key and bytes zero.
 * Reply echoes them, CLOSE, then an explicit reset outcome. Unsupported
 * means DriverKit declined PERST and the original pair is verified idle. */
#define EXTGPU_VEGA_PERST_REPLY_WORDS 8u
#define EXTGPU_VEGA_PERST_RESET UINT64_C(1)
#define EXTGPU_VEGA_PERST_UNSUPPORTED UINT64_C(2)
/* A native failure whose transport quiescence was checked. Low 32 bits
 * are the exact kern_return_t. It does not confirm a platform reset. */
#define EXTGPU_VEGA_PERST_NATIVE_ERROR (UINT64_C(1)<<32)
/* Terminal cold-removal prototype: old authenticated endpoint only.
 * Key words: frozen generation, driver registry ID, PCI registry ID, nonce[2].
 * BEGIN in5=key; out7=key,enrolledCount,FROZEN. FROZEN reply also requires
 * an acknowledged live-provider BME clear. A lost reply never permits IO.
 * RETIRE in8=key,iova,size,confirmations; out10=key,iova,size,nativeStatus,
 * SUCCESS_TOMBSTONE,commitSequence. nativeStatus must be zero and sequence>0.
 * FINISH in6=key,confirmations; out7=key,CLOSED,commitSequence.
 * Confirmations assert BOTH explicit owner-attested physical power-off and
 * SDK-observed termination of the exact old PCI provider. Stop/IPC loss alone
 * supplies neither. Native failures, unknown keys and malformed replies retain.
 * No new IO, mapping, registration, reset, claim or ordinary release after
 * arming. Only enrolled registered RAM is supported, without BAR-export
 * history, legacy allocations, live IRQ sources or local IRQ callbacks.
 * SDK >=1.4 requires protocol1.3 RECOVERY_BROKER as well as COLD_REMOVAL.
 * The separately registered broker retains the original userclient context;
 * it cannot reconstruct driver objects after IOUserServer process death.
 */
#define EXTGPU_COLD_KEY_WORDS 5u
#define EXTGPU_COLD_BEGIN_INPUT_COUNT 5u
#define EXTGPU_COLD_BEGIN_OUTPUT_COUNT 7u
#define EXTGPU_COLD_RETIRE_INPUT_COUNT 8u
#define EXTGPU_COLD_RETIRE_OUTPUT_COUNT 10u
#define EXTGPU_COLD_FINISH_INPUT_COUNT 6u
#define EXTGPU_COLD_FINISH_OUTPUT_COUNT 7u
#define EXTGPU_COLD_CONFIRM_PHYSICAL_OFF UINT64_C(1)
#define EXTGPU_COLD_CONFIRM_PROVIDER_TERMINATED UINT64_C(2)
#define EXTGPU_COLD_CONFIRM_REQUIRED UINT64_C(3)
#define EXTGPU_COLD_STATE_FROZEN UINT64_C(1)
#define EXTGPU_COLD_STATE_SUCCESS_TOMBSTONE UINT64_C(2)
#define EXTGPU_COLD_STATE_CLOSED UINT64_C(3)
#define EXTGPU_COLD_STATE_BOUND UINT64_C(4)
/* Recovery bootstrap, before the old PCI provider disappears:
 * GPU25 in1=expectedGeneration; out3=exactBrokerRegistryID,major1,minor>=3.
 * Open that exact VFIOUserRecoveryBroker and Query18 before mutable setup.
 * Broker Query18 uses its own registryID as generation and exposes only
 * GENERATION|COLD_REMOVAL|RECOVERY_BROKER; no GPU ownership/IO capabilities.
 * GPU26 in7=key5,privateCapabilityNonce[2]; out6=key5,brokerRegistryID.
 * The private capability is random, one-use and distinct from public keynonce.
 * Broker27 in7=key5,privateCapabilityNonce[2]; out7=key5,count,BOUND4.
 * Binding fixes one broker userclient instance. Retry uses the same original
 * connections/key/capability, never a new provider/epoch or a reopened broker.
 * GPU22 freezes/BME-clears; Broker22 is read-only snapshot in5/out7, identical
 * FROZEN1 acknowledgement/count. It never retries hardware access. Once bound,
 * RETIRE23 and FINISH24 use only this broker connection and original escrow.
 */
#define EXTGPU_RECOVERY_ENDPOINT_INPUT_COUNT 1u
#define EXTGPU_RECOVERY_ENDPOINT_OUTPUT_COUNT 3u
#define EXTGPU_RECOVERY_ESCROW_INPUT_COUNT 7u
#define EXTGPU_RECOVERY_PREPARE_OUTPUT_COUNT 6u
#define EXTGPU_RECOVERY_BIND_OUTPUT_COUNT 7u
#define EXTGPU_RECOVERY_PRIVATE_CAP_WORDS 2u
/* ARM uses the same private-key transport as PREPARE with a count of zero.
 * RELEASE is broker-only. Input: key, detach flag, and BAR proof (eight words
 * from the owner, zero when no proof is needed). Reply echoes key + flag.
 * Detach requires the exact original PCI provider to have disappeared and
 * the original DriverKit client to have stopped. Flag zero drops escrow after
 * acknowledged ordinary release, without operating on the PCI provider.
 */
#define EXTGPU_SURPRISE_RELEASE_INPUT_COUNT 14u
#define EXTGPU_SURPRISE_RELEASE_OUTPUT_COUNT 6u
#define EXTGPU_SURPRISE_BAR_DOMAIN UINT64_C(0x4f504155544835)
#define EXTGPU_RECOVERY_PROTOCOL_MINIMUM_MINOR UINT64_C(3)
#define EXTGPU_RECOVERY_BROKER_CAPABILITIES (EXTGPU_CAP_GENERATION | EXTGPU_CAP_COLD_REMOVAL | EXTGPU_CAP_RECOVERY_BROKER)
/* ReleaseOwner: scalarIn[0]=expected generation; no scalarOut.
 * SUCCESS acknowledges proven release. BUSY/NOT_READY preserve ownership.
 * UNSUPPORTED preserves ownership where live BAR mappings or unproven FLR
 * prevent safe handoff. Quarantined providers cannot be claimed again.
 * Close is not a safe-release acknowledgement. No ownership sharing exists.
 * Reset17 requires no live resources; success changes generation, so requery.
 * Register5 out: segmentCount, firstIOVA, firstLength; full single segment only.
 * Allocate3 out: size, flags, segmentCount, firstIOVA, firstLength, classCode.
 * RetireInterrupts21: no scalar input/output. Idempotent acknowledged source
 * cancellation/drain. NOT_READY retains all client state for retry; errors
 * never authorize freeing an asynchronous callback target.
 */
#endif
