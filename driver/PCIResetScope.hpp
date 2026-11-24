#pragma once
#include <extgpu/extgpu_driver_abi.h>

namespace extgpu_driver {
struct ResetFunction {
    uint64_t registry;
    uint32_t vendor, device, class_code;
    uint8_t bus, slot, function;
};

struct ResetPair {
    uint32_t vendor, display, audio;
    uint32_t alternate_display = 0;
    constexpr bool accepts_display(uint32_t device) const
    {
        return device == display || (alternate_display && device == alternate_display);
    }
    constexpr uint32_t audio_id() const { return audio << 16 | vendor; }
};

inline constexpr ResetPair VegaResetPair{0x1002, 0x687f, 0xaaf8};
inline constexpr ResetPair RTX3090ResetPair{EXTGPU_RTX3090_VENDOR, EXTGPU_RTX3090_DISPLAY, EXTGPU_RTX3090_AUDIO};
inline constexpr ResetPair PolarisResetPair{
    EXTGPU_POLARIS_VENDOR, EXTGPU_POLARIS_DISPLAY, EXTGPU_POLARIS_AUDIO, EXTGPU_POLARIS_DISPLAY_2048SP};

struct PCIResetScope {
    uint64_t bridge, display, audio;
    bool valid_ids() const
    {
        return bridge && display && audio && bridge != display &&
            bridge != audio && display != audio;
    }
    bool matches(const PCIResetScope &other) const
    {
        return bridge == other.bridge && display == other.display && audio == other.audio;
    }
    bool is_pair(const ResetFunction &gpu, const ResetFunction &hda, const ResetPair &pair) const
    {
        if (!valid_ids() || gpu.registry != display || hda.registry != audio ||
            gpu.bus != hda.bus || gpu.slot != hda.slot || gpu.function != 0 || hda.function != 1 ||
            (gpu.class_code >> 16) != 0x03 || (hda.class_code >> 8) != 0x0403 ||
            gpu.vendor != hda.vendor)
            return false;
        if (pair.vendor && (gpu.vendor != pair.vendor || !pair.accepts_display(gpu.device) || hda.device != pair.audio))
            return false;
        return true;
    }
    bool is_generic_pair(const ResetFunction &gpu, const ResetFunction &hda) const
    {
        return valid_ids() && gpu.registry == display && hda.registry == audio &&
            gpu.bus == hda.bus && gpu.slot == hda.slot && gpu.function == 0 && hda.function == 1 &&
            (gpu.class_code >> 16) == 0x03 && (hda.class_code >> 8) == 0x0403 &&
            gpu.vendor == hda.vendor;
    }
};
}
