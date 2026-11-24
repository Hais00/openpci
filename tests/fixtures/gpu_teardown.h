#pragma once

#include <extgpu/extgpu.h>
#include <stdbool.h>

enum op_gpu_release_kind { OP_GPU_RELEASE_FLR, OP_GPU_RELEASE_VEGA, OP_GPU_RELEASE_RTX3090, OP_GPU_RELEASE_POLARIS };

/* The VMM owns RAM separately. Its stop path must retain the allocation
 * until op_gpu_ram_releasable says the driver acknowledged retirement. */
struct op_gpu_lease {
    extgpu_session *session;
    extgpu_device *device;
    extgpu_buffer *window;
    extgpu_irq *irq;
    bool stop_acknowledged;
    bool startup_reset_required;
    bool bus_master_clear_acknowledged;
    bool warm_release_acknowledged;
    enum op_gpu_release_kind release_kind;
};

extgpu_status op_gpu_preflight_release(struct op_gpu_lease *lease, enum op_gpu_release_kind flr_fallback);
extgpu_status op_gpu_disable_bus_master_before_irq_retirement(struct op_gpu_lease *lease);
extgpu_status op_gpu_shutdown(struct op_gpu_lease *lease);
bool op_gpu_shutdown_waiting(const struct op_gpu_lease *lease);
bool op_gpu_ram_releasable(const struct op_gpu_lease *lease);
