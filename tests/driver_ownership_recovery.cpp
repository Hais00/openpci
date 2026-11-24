#include "OwnershipRecovery.hpp"
#include "WarmVMRelease.hpp"
#include "VegaBarAssignments.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); std::exit(1); } } while (0)

int main()
{
    using extgpu_driver::preserves_pristine_claim;
    for (uint64_t selector = 0; selector <= EXTGPU_SELECTOR_RETIRE_INTERRUPTS; ++selector) {
        const bool observational = selector == EXTGPU_SELECTOR_QUERY_PROTOCOL ||
            selector == EXTGPU_SELECTOR_GET_IDENTITY || selector == EXTGPU_SELECTOR_GET_BAR_INFO ||
            selector == EXTGPU_SELECTOR_CLAIM || selector == EXTGPU_SELECTOR_RELEASE_OWNER;
        CHECK(preserves_pristine_claim(selector, nullptr, 0) == observational);
    }
    using Ledger = extgpu_driver::WarmVMRelease;
    const extgpu_driver::WarmVMKey key = {71, 0x40000000, 0x20000000};
    Ledger warm{};
    CHECK(!warm.advance(Ledger::reset_verified));
    CHECK(!warm.begin(key, 0));
    CHECK(warm.begin(key, 42));
    CHECK(!warm.begin(key, 42));
    CHECK(!warm.advance(Ledger::dma_retired)); // Cannot free when reset outcome is unknown.
    CHECK(!warm.key.matches({72, key.registration, key.bytes}));
    CHECK(!warm.key.matches({71, key.registration, key.bytes - 16384}));
    CHECK(!warm.arm_reset(true)); // No reset before both masters are disabled.
    CHECK(!warm.advance(Ledger::draining));
    CHECK(warm.advance(Ledger::quiescing));
    CHECK(!warm.arm_reset(true)); // The owner is known and command reads may still be pending.
    CHECK(warm.advance(Ledger::draining));
    CHECK(!warm.arm_reset(false));
    CHECK(warm.phase == Ledger::draining);
    CHECK(!warm.advance(Ledger::armed));
    CHECK(warm.arm_reset(true));
    CHECK(!warm.arm_reset(true)); // Only one reset on this ticket.
    CHECK(!warm.advance(Ledger::reset_verified));
    CHECK(warm.advance(Ledger::reset_returned));
    CHECK(!warm.advance(Ledger::dma_retired)); // A native reset return alone does not release RAM.
    CHECK(warm.advance(Ledger::reset_verified));
    CHECK(!warm.advance(Ledger::closed)); // CompleteDMA ACK is required.
    CHECK(warm.advance(Ledger::dma_retired));
    CHECK(warm.advance(Ledger::closed));
    CHECK(warm.key.matches(key) && warm.pci_registry == 42);
    CHECK(!warm.advance(Ledger::closed));
    const extgpu_driver::PCIResetScope scope{40, 41, 42};
    CHECK(!warm.has_group_release_receipt(key, scope, EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA));
    Ledger startup{};
    CHECK(!startup.begin({71, 1, 0}, scope.display, scope, EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA));
    CHECK(startup.begin({71, 0, 0}, scope.display, scope, EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA));
    CHECK(!startup.has_group_release_receipt({71, 0, 0}, scope, EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA));
    CHECK(!startup.key.matches(key));
    Ledger vega{};
    CHECK(!vega.has_group_release_receipt(key, scope, EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA));
    CHECK(vega.begin(key, scope.display, scope, EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA));
    CHECK(!vega.advance(Ledger::draining)); // No retryable quiesce while audio Open is unproven.
    CHECK(vega.advance(Ledger::quiescing));
    CHECK(!vega.arm_reset(true));
    CHECK(vega.advance(Ledger::draining));
    CHECK(!vega.has_group_release_receipt(key, scope, EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA));
    CHECK(vega.arm_reset(true));
    CHECK(vega.advance(Ledger::reset_returned));
    CHECK(vega.advance(Ledger::reset_verified));
    CHECK(vega.advance(Ledger::dma_retired));
    CHECK(!vega.has_group_release_receipt(key, scope, EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA));
    CHECK(vega.advance(Ledger::closed));
    CHECK(vega.has_group_release_receipt(key, scope, EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA));
    CHECK(!vega.has_group_release_receipt(key, scope, EXTGPU_SELECTOR_WARM_RELEASE_VM_RTX3090));
    CHECK(!vega.has_group_release_receipt(key, scope, EXTGPU_SELECTOR_WARM_RELEASE_VM_POLARIS));
    CHECK(!vega.has_group_release_receipt({72, key.registration, key.bytes}, scope, EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA));
    CHECK(!vega.has_group_release_receipt({71, key.registration + 16384, key.bytes}, scope, EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA));
    CHECK(!vega.has_group_release_receipt({71, key.registration, key.bytes - 16384}, scope, EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA));
    CHECK(!vega.has_group_release_receipt(key, {40, 41, 43}, EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA));
    Ledger probe{};
    const extgpu_driver::WarmVMKey probe_key = {71, 0, 0};
    CHECK(probe.begin(probe_key, scope.display, scope, EXTGPU_SELECTOR_PROBE_VEGA_PERST));
    CHECK(probe.advance(Ledger::quiescing));
    CHECK(probe.advance(Ledger::draining));
    CHECK(probe.arm_reset(true));
    CHECK(probe.advance(Ledger::reset_returned));
    CHECK(probe.advance(Ledger::reset_verified));
    CHECK(probe.advance(Ledger::dma_retired));
    CHECK(probe.advance(Ledger::closed));
    CHECK(!probe.has_group_release_receipt(probe_key, scope, EXTGPU_SELECTOR_WARM_RELEASE_VM_VEGA));
    const extgpu_driver::ResetFunction gpu{41, 0x1002, 0x687f, 0x030000, 5, 0, 0};
    extgpu_driver::ResetFunction audio{42, 0x1002, 0xaaf8, 0x040300, 5, 0, 1};
    CHECK(scope.is_pair(gpu, audio, extgpu_driver::VegaResetPair));
    audio.bus++;
    CHECK(!scope.is_pair(gpu, audio, extgpu_driver::VegaResetPair));
    audio.bus--;
    audio.registry++;
    CHECK(!scope.is_pair(gpu, audio, extgpu_driver::VegaResetPair));
    CHECK(!scope.matches({43, 41, 42}));
    const auto &rtx = extgpu_driver::RTX3090ResetPair;
    const extgpu_driver::ResetFunction rtx_gpu{41, rtx.vendor, rtx.display, 0x030000, 5, 0, 0};
    extgpu_driver::ResetFunction rtx_audio{42, rtx.vendor, rtx.audio, 0x040300, 5, 0, 1};
    CHECK(scope.is_pair(rtx_gpu, rtx_audio, rtx));
    CHECK(!scope.is_pair(gpu, rtx_audio, rtx));
    CHECK(!scope.is_pair(rtx_gpu, rtx_audio, extgpu_driver::VegaResetPair));
    rtx_audio.slot++;
    CHECK(!scope.is_pair(rtx_gpu, rtx_audio, rtx));
    rtx_audio.slot--;
    rtx_audio.function++;
    CHECK(!scope.is_pair(rtx_gpu, rtx_audio, rtx));

    const auto &polaris = extgpu_driver::PolarisResetPair;
    extgpu_driver::ResetFunction polaris_gpu{41, polaris.vendor, polaris.display, 0x030000, 5, 0, 0};
    extgpu_driver::ResetFunction polaris_audio{42, polaris.vendor, polaris.audio, 0x040300, 5, 0, 1};
    CHECK(scope.is_pair(polaris_gpu, polaris_audio, polaris));
    polaris_gpu.device = EXTGPU_POLARIS_DISPLAY_2048SP;
    CHECK(scope.is_pair(polaris_gpu, polaris_audio, polaris));
    polaris_audio.device = 0xaaf8;
    CHECK(!scope.is_pair(polaris_gpu, polaris_audio, polaris));
    polaris_audio.device = EXTGPU_POLARIS_AUDIO;
    polaris_gpu.device = 0x67ef;
    CHECK(!scope.is_pair(polaris_gpu, polaris_audio, polaris));

    using Assignments = extgpu_driver::VegaBarAssignments;
    // Native Apple Silicon cells, as observed on the physical Vega.
    uint32_t assignments[] = {
        0xc2050010, 0xc0000000, 0, 0x10000000, 0,
        0xc2050018, 0xd0000000, 0, 0x00200000, 0,
        0x82050024, 0x80100000, 0, 0x00080000, 0,
        0x82050030, 0x80180000, 0, 0x00020000, 0,
    };
    auto bytes = reinterpret_cast<uint8_t *>(assignments);
    Assignments bars{};
    CHECK(bars.parse(bytes, sizeof assignments, 0x00050000));
    CHECK(bars.ranges[0].address == 0xc0000000 && bars.ranges[2].address == 0x80100000);
    CHECK(!bars.parse(bytes, sizeof assignments - 1, 0x00050000));
    CHECK(!bars.parse(bytes, sizeof assignments, 0x00060000));
    assignments[16] = 0x80100000;
    CHECK(!bars.parse(bytes, sizeof assignments, 0x00050000)); // Overlapping ROM.
    assignments[16] = 0x80180000;
    assignments[7] = 1;
    CHECK(!bars.parse(bytes, sizeof assignments, 0x00050000)); // Unknown address domain.
    CHECK(!preserves_pristine_claim(UINT64_MAX, nullptr, 0));
    const uint64_t widths[] = {0, 1, 2, 3, 4, 8, UINT64_MAX};
    for (uint64_t width : widths) {
        for (uint64_t offset = 0; offset <= 68; ++offset) {
            const uint64_t inputs[] = {offset, width};
            const bool expected = (width == 1 || width == 2 || width == 4) &&
                offset < 64 && offset + width <= 64 && offset % width == 0;
            CHECK(preserves_pristine_claim(EXTGPU_SELECTOR_CONFIG_READ, inputs, 2) == expected);
            CHECK(!preserves_pristine_claim(EXTGPU_SELECTOR_CONFIG_READ, inputs, 1));
        }
        const uint64_t overflow[] = {UINT64_MAX, width};
        CHECK(!preserves_pristine_claim(EXTGPU_SELECTOR_CONFIG_READ, overflow, 2));
    }
    std::puts("PASS claim history, exact Vega assignment bounds and warm release ledger; no hardware");
}
