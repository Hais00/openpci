#include "gpu_teardown.h"

#include <IOKit/IOReturn.h>
#include <string.h>

#define PCI_COMMAND_OFFSET 4u
#define PCI_COMMAND_MASTER 4u

extgpu_status op_gpu_disable_bus_master_before_irq_retirement(struct op_gpu_lease *lease)
{
    uint32_t command = UINT16_MAX;
    extgpu_status st = extgpu_config_read(lease->device, PCI_COMMAND_OFFSET, 2, &command);
    if (st != EXTGPU_OK || command == UINT16_MAX) return EXTGPU_E_UNSAFE;
    if (command & PCI_COMMAND_MASTER) {
        st = extgpu_config_write(lease->device, PCI_COMMAND_OFFSET, 2, command & ~PCI_COMMAND_MASTER);
        if (st != EXTGPU_OK) return EXTGPU_E_UNSAFE;
        st = extgpu_config_read(lease->device, PCI_COMMAND_OFFSET, 2, &command);
        if (st != EXTGPU_OK || command == UINT16_MAX || (command & PCI_COMMAND_MASTER))
            return EXTGPU_E_UNSAFE;
    }
    lease->bus_master_clear_acknowledged = true;
    return EXTGPU_OK;
}

extgpu_status op_gpu_preflight_release(struct op_gpu_lease *lease, enum op_gpu_release_kind flr_fallback)
{
    if (lease->release_kind == OP_GPU_RELEASE_VEGA)
        return extgpu_warm_release_vm_vega_pin(lease->device);
    if (lease->release_kind != OP_GPU_RELEASE_FLR) return EXTGPU_OK;
    extgpu_status status = extgpu_warm_release_vm_flr_preflight(lease->device);
    if (status != EXTGPU_E_UNSUPPORTED || flr_fallback == OP_GPU_RELEASE_FLR) return status;
    extgpu_error_info detail;
    if (extgpu_device_last_error(lease->device, &detail) != EXTGPU_OK ||
        strcmp(detail.operation, "VMFLR.endpoint_not_advertised")) return status;
    switch (flr_fallback) {
    case OP_GPU_RELEASE_RTX3090: status = extgpu_warm_release_vm_rtx3090_preflight(lease->device); break;
    case OP_GPU_RELEASE_POLARIS: status = extgpu_warm_release_vm_polaris_preflight(lease->device); break;
    default: return status;
    }
    if (status == EXTGPU_OK) lease->release_kind = flr_fallback;
    return status;
}

extgpu_status op_gpu_shutdown(struct op_gpu_lease *lease)
{
    if (!lease) return EXTGPU_E_ARGUMENT;
    if (!lease->stop_acknowledged) return EXTGPU_E_BUSY;

    if (!lease->warm_release_acknowledged) {
        if (!lease->device || (!lease->window && !lease->startup_reset_required))
            return EXTGPU_E_UNSAFE;
        if (!lease->bus_master_clear_acknowledged) {
            extgpu_status ret = op_gpu_disable_bus_master_before_irq_retirement(lease);
            if (ret != EXTGPU_OK) return ret;
        }
        if (lease->irq) {
            extgpu_status ret = extgpu_irq_close(&lease->irq);
            if (ret != EXTGPU_OK) return ret;
        }
        extgpu_status ret;
        if (!lease->window) ret = extgpu_warm_release_vm_no_dma(lease->device);
        else switch (lease->release_kind) {
        case OP_GPU_RELEASE_FLR: ret = extgpu_warm_release_vm_flr(lease->device, &lease->window); break;
        case OP_GPU_RELEASE_VEGA: ret = extgpu_warm_release_vm_vega(lease->device, &lease->window); break;
        case OP_GPU_RELEASE_RTX3090: ret = extgpu_warm_release_vm_rtx3090(lease->device, &lease->window); break;
        case OP_GPU_RELEASE_POLARIS: ret = extgpu_warm_release_vm_polaris(lease->device, &lease->window); break;
        default: return EXTGPU_E_ARGUMENT;
        }
        if (ret != EXTGPU_OK) return ret;
        if (lease->window) return EXTGPU_E_PROTOCOL;
        lease->warm_release_acknowledged = true;
    }

    if (lease->device) {
        extgpu_status ret = extgpu_device_close(&lease->device);
        if (ret != EXTGPU_OK && lease->device) {
            extgpu_error_info detail;
            if (extgpu_device_last_error(lease->device, &detail) == EXTGPU_OK && detail.status == ret &&
                !strcmp(detail.operation, "IOServiceClose.local_reference_consumed")) {
                /* Release was already acknowledged. The SDK has detached the
                 * original IPC name, so this retry only disposes local state. */
                ret = extgpu_device_close(&lease->device);
            }
        }
        if (ret != EXTGPU_OK) return ret;
    }
    if (lease->session) return extgpu_session_destroy(&lease->session);
    return EXTGPU_OK;
}

bool op_gpu_shutdown_waiting(const struct op_gpu_lease *lease)
{
    if (!lease || !lease->stop_acknowledged ||
        lease->warm_release_acknowledged || (!lease->window && !lease->startup_reset_required) ||
        !lease->device) return false;
    extgpu_error_info detail;
    if (extgpu_device_last_error(lease->device, &detail) != EXTGPU_OK ||
        detail.native_code != kIOReturnNotReady) return false;
    if (!lease->bus_master_clear_acknowledged)
        return detail.status == EXTGPU_E_IO && (!strcmp(detail.operation, "ConfigRead") ||
                                               !strcmp(detail.operation, "ConfigWrite"));
    if (detail.status != EXTGPU_E_UNSAFE) return false;
    if (lease->irq)
        return !strcmp(detail.operation, "IRQ.close_mask") ||
               !strcmp(detail.operation, "IRQ.close_drain") ||
               !strcmp(detail.operation, "IRQ.close_retire");
    return !strcmp(detail.operation, "WarmVM.release_unacknowledged") ||
        !strcmp(detail.operation, "WarmVM.awaiting_original_scope");
}

bool op_gpu_ram_releasable(const struct op_gpu_lease *lease)
{
    return lease && lease->stop_acknowledged && lease->bus_master_clear_acknowledged &&
        lease->warm_release_acknowledged &&
        !lease->window && !lease->irq && !lease->device && !lease->session;
}
