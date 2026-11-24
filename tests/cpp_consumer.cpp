#include <extgpu/extgpu.h>
#include <cstdint>
#include <type_traits>

static_assert(sizeof(extgpu_status) == sizeof(std::int32_t), "status is a fixed-width public ABI");
static_assert(std::is_standard_layout<extgpu_device_info>::value, "device ABI must be C compatible");
static_assert(std::is_standard_layout<extgpu_protocol_info>::value, "protocol ABI must be C compatible");

int main()
{
    extgpu_session *session = nullptr;
    if (extgpu_session_create(&session) != EXTGPU_OK) return 1;
    return extgpu_session_destroy(&session) == EXTGPU_OK && session == nullptr ? 0 : 2;
}
