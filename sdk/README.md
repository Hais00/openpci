# OpenPCI host C SDK

Current SDK source and binaries are offered under PolyForm Noncommercial 1.0.0. The SDK package includes the license and copyright notice.

This library calls the OpenPCI DriverKit user client directly through public macOS IOKit APIs. It has no socket service, broker, QEMU dependency, or frame-copy path. The SDK builds with the macOS CLT SDK; building the separate driver requires DriverKit.

The public header is C11/C++17, with fixed-width status values and opaque ownership handles. Public structure layouts in ABI 1 are frozen. `extgpu_sdk_abi_version()` must equal `EXTGPU_SDK_ABI_VERSION`; the shared library has ABI version 1. Future minor releases use new types/functions instead of extending these structures.

`extgpu_enumerate` only reads IORegistry. It never opens or claims a device. Select the exact driver IORegistry ID, then use `extgpu_device_open`. Open verifies protocol major, minimum minor, generation, limits and ABI fingerprint with selector 18 before admitting other calls. An unversioned donor driver is rejected. Claim is explicit and exclusive. Registry BDF/root metadata is available for integrations, but an ambiguous BDF is never selected by the SDK.

The implemented APIs cover PCI configuration, BAR information, direct process BAR mapping, checked MMIO RPCs, host-memory DMA registration, driver-owned DMA allocation, interrupt notification/mask/drain/rearm, reset and safe owner release. Capability bits describe implemented driver APIs, not completed hardware tests. `CONFIG_WRITE` follows the driver's restricted policy: only command-register bytes 0x04..0x05 and MSE/BME changes (mask 0x0006), preserving other bits. Physical BAR placement, MSI routing and PCIe transport parameters remain host-owned. Disallowed writes return their actual driver error; the SDK does not emulate success. DMA starts with a 32-bit address limit; applications explicitly select 32..64 bits before resources are created. The SDK checks one segment has exactly the requested length and fits that limit. The requested IOVA is a registration key; use the returned bus address for hardware programming. The SDK does not manufacture a guest-IOMMU mapping.

SDK 1.2 adds `extgpu_bar_map_with_cache_policy`; it requires a 1.2 library even
though all earlier ABI-1 layouts/symbols remain compatible. The old `extgpu_bar_map`
delegates to `EXTGPU_BAR_CACHE_AUTOMATIC`, preserving its existing requests.

For latency-sensitive clients using a DEXT with wire protocol 1.24 and the
extended polling capability, set `OPENPCI_MMIO_RING_POLL_US` before the first
MMIO operation. Values from 32 through 1000 select the maximum idle polling
interval after activity. Zero leaves this mode disabled. The consumer still
checks every access and publishes completion in order. Longer polling reduces
IOKit notifications under a sustained workload and uses more host CPU.
With wire protocol 1.25, `OPENPCI_MMIO_RING_ADAPTIVE=1` shortens the next
polling interval after arrivals near the configured limit.
With wire protocol 1.26, `OPENPCI_MMIO_RING_IRQ_ARM=1` allows an idle consumer
to prepare a short polling window when it sends a shared interrupt notification.
This option requires an extended polling interval and shared interrupt delivery.
With wire protocol 1.27, `OPENPCI_MMIO_RING_EVENT=1` selects the experimental
event-only transport. Each batch uses a synchronous IOKit notification, and
the DEXT completes the batch before returning to an idle state. This mode
takes precedence over the extended polling setting.

| Policy | Process mapping request |
| --- | --- |
| `AUTOMATIC` | Write combining for prefetchable BARs, descriptor default otherwise |
| `DEFAULT` | No cache override, including for prefetchable BARs |
| `INHIBIT` | `kIOMapInhibitCache` |
| `WRITE_COMBINE` | `kIOMapWriteCombineCache`; non-prefetchable and I/O-port BARs rejected |

The SDK never retries a failed mapping with another cache policy. A BAR's first
export attempt fixes its requested native option bits for the device handle's
lifetime, even if that export fails. A later request with different bits returns
`BUSY`, including after unmap or reset; equivalent policy names with identical
bits are allowed. One live mapping per BAR is enforced. This history prevents
SDK-driven hot switching, but cannot establish absence of mappings outside this
handle. Closing/reopening a client alone does not establish that changing a
shared descriptor's cache mode is safe.

These are requested host-process mapping options, not effective CPU or HVF
stage-2 cache attributes. In the reference Apple XNU 12377 implementation,
[nondefault requests can change the underlying memory entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.121.6/osfmk/vm/vm_memory_entry.c#L209-L240),
and [ARM write combining distinguishes DRAM from non-DRAM](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.121.6/osfmk/arm/pmap/pmap.c#L5971-L6043).
Those internals are not a guarantee for the running kernel. Public ARM HVF
mapping APIs expose permissions, not a cache-type selector/query. No policy
here has been qualified for direct physical GPU mappings through HVF after the
host panic. QEMU's direct-BAR quarantine remains in force. None of these options
change native DMA registration or introduce a copy/broker into the data path.

Host-memory registrations reserve non-overlapping IOVA ranges before the RPC, including uncertain registrations retained after failures. A rejected duplicate cannot unregister another buffer. Unknown negative registration replies retain a reservation and backing handle; a malformed success or incomplete segment can retire only its uniquely reserved region. A failed retirement leaves the handle live. Applications must keep their backing mapping alive until `extgpu_buffer_unregister` succeeds. The caller must stop all GPU references before unregister/free; neither DMA coverage nor an IOKit return value proves an engine has drained.

Known canonical claim preflight failures, such as allocation failure before ownership is published, leave the connection unclaimed and closable. Unknown transport or malformed-success claim replies retain uncertain ownership and block owner operations. Release revalidates the handshake before resolving that state; only the driver's fresh owner reply can establish release or absence of ownership. A failed post-reset query leaves protocol information stale, and release can retry that query. `extgpu_device_protocol` never reports stale cached metadata as a validated epoch.

Once an unclaimed device has no children, a failed ordinary `IOServiceClose`
still consumes its local Mach reference. The SDK retains the device object,
blocks further operations and lets the next close finish local cleanup without
calling the consumed name again. This does not acknowledge hardware retirement.

SDK 1.3 introduced the three terminal cold-removal functions. SDK 1.4 keeps their
signatures and all ABI-1 public layouts, while requiring a separately registered
recovery broker. Ordinary operations still accept protocol 1.1; cold recovery
requires protocol 1.3 and both `EXTGPU_CAP_COLD_REMOVAL` and
`EXTGPU_CAP_RECOVERY_BROKER`. `extgpu_device_open` captures the exact old PCI provider
registry ID before opening its driver service. `extgpu_cold_removal_begin` admits
only a certain claim with registered host RAM, no BAR export history, no live
mapping, allocation, or IRQ handle. The SDK first asks the original GPU endpoint
for its exact broker registry ID, verifies `VFIOUserRecoveryBroker`, opens that
service and negotiates its restricted broker protocol. Before sending PREPARE it permanently freezes
ordinary operations on that device handle, even when the reply is lost or
malformed. The generation, driver ID, PCI ID, and random nonce form one immutable
retry key. A separate random, one-use private capability is passed only to
PREPARE and BIND; the public information type never exposes it. PREPARE retains
the original userclient context, and BIND fixes one broker userclient instance.
The original GPU endpoint performs BEGIN/BME-clear, followed by an authoritative
read-only broker snapshot of that exact key/count/FROZEN state. Lost PREPARE,
BIND or GPU replies are reconciled on the same connections/key/capability;
the SDK never reopens a broker or adopts a newly discovered provider/epoch.
No buffer or backing is freed by beginning recovery.

After physically removing power, the caller explicitly attests
`EXTGPU_COLD_PHYSICAL_POWER_OFF` to `extgpu_cold_removal_confirm`. The SDK separately
requires a successful registry query with a valid empty iterator for the exact
old PCI provider ID. Driver endpoint disappearance, BDF reuse, IPC failure,
missing match dictionaries and invalid iterators never establish that fact.
IOKit's documented successful NULL iterator is an empty match; native failure
with a NULL iterator remains an error, and contradictory validity flags fail.
Confirmation performs no driver RPC or power action. It cannot verify the
caller's physical-power attestation itself.

Only after both confirmations does `extgpu_buffer_unregister` use broker-only COLD_RETIRE.
It accepts an exact key/IOVA/size reply with native status zero, a success
tombstone and nonzero commit sequence. Any other result returns `UNSAFE` and
retains the handle, reservation and backing; retry on the same handle without
falling back to ordinary unregister. Keep the application's original host-memory
pointer alive: the SDK's buffer address accessor returns NULL while frozen.
With zero buffers, mappings and IRQs, `extgpu_cold_removal_finish` requires an exact
CLOSED acknowledgement before allowing `extgpu_device_close`. The broker retains
the original driver objects, not reconstructed mappings or new PCI objects.
Once every native RETIRE and FINISH ACK has arrived and zero children remain,
close drops the original local Mach reference without calling the revoked GPU
port, then closes the broker. In the inspected primary
[IOKitLib.c implementation](https://github.com/apple-oss-distributions/IOKitUser/blob/IOKitUser-100231.120.3/IOKitLib.c#L1479-L1487),
`IOServiceClose` consumes its local connect reference even when its RPC fails;
the SDK records that consumption and retains the device object for close retry,
avoiding reuse/double-deallocation of that port name. Local IPC cleanup is never
hardware retirement evidence. Broker survival beyond PCI removal requires the
same IOUserServer process to remain alive. Process death has no reconstruction
fallback: missing broker/native acknowledgements retain the obligation.
This API performs no automatic reset, restart, power action or teardown.

Destroy children first: stop application IRQ activity, mask/drain/close IRQ, unmap BARs, stop GPU references and unregister buffers, release owner, close device, destroy session. Cleanup errors preserve handles and ownership. `extgpu_device_close` refuses a claimed device or live child. Release uses the explicit selector-19 acknowledgement, never deprecated service-terminate selector 2 and never a guessed success from `IOServiceClose`. The current driver conservatively rejects release after any BAR mapping has been issued because it cannot observe authoritative revocation of those mappings. After DMA/hardware mutation, safe release additionally depends on the driver's actual reset/quiescence result. Unsupported or failed release means keep the device handle; it is not an invitation to force-close it.

IRQ delivery uses a private serial dispatch source receiving IOKit asynchronous Mach messages. With a one-shot IRQ, the callback explicitly sets up the next notification. SDK 1.5 adds `extgpu_irq_drain_and_rearm`, which takes the pending bits and installs the next callback in one request on compatible drivers. On earlier drivers it issues the original drain and rearm requests. The individual functions remain available for callers that want to defer rearming.

SDK 1.6 adds `extgpu_irq_create_shared` for entitled clients of wire protocol 1.18. It installs a persistent notification backed by a CPU-only bitmap for the current claim. The callback drains pending bits with `extgpu_irq_drain` or `extgpu_irq_drain_and_rearm`. A shared handle needs no rearm request. Use `extgpu_irq_create` when the shared form returns `EXTGPU_E_UNSUPPORTED`. An uncertain setup or retirement preserves the returned IRQ handle for `extgpu_irq_close`.

Close from the owning application thread. Close masks and drains the driver, cancels the receive source, and waits for its cancellation handler before freeing the callback context. If masking or draining fails, the IRQ handle and source remain retained. Synchronous IOKit calls and callback draining are not cancellable through this SDK. Application callbacks must return promptly.

The CLI `extgpu-info` defaults to registry enumeration only. `--inspect-id ID` opens that exact service and queries version/identity without claiming it. It contains no mutation, install, BAR, DMA or reset mode. No hardware operation is run by the build.

`extgpu_test.h` is a private, non-installed test interface enabled only by `EXTGPU_TESTING`. It injects scalar/map/unmap/close operations and still exercises the real SDK handshake, validation, accounting and lifetime logic. Production builds always use IOKit; there is no fake backend fallback.

Examples live under `sdk/examples`. The inspect program is read-only. The owned-region example is an embedding pattern whose returned reservation remains owned by the caller even when registration/retirement cannot be proven. It submits no GPU work.
