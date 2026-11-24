/* SPDX-License-Identifier: MIT
 * PRIVATE test injection. Not installed, not present in production builds. */
#ifndef EXTGPU_TEST_H
#define EXTGPU_TEST_H
#ifndef EXTGPU_TESTING
#error "extgpu_test.h requires EXTGPU_TESTING; never a production transport"
#endif
#include <extgpu/extgpu.h>
typedef struct extgpu_test_transport {
    int32_t (*scalar)(void *context, uint32_t selector, const uint64_t *input,
                       uint32_t input_count, uint64_t *output, uint32_t *output_count);
    int32_t (*map)(void *context, uint32_t type, uint64_t *address,
                   uint64_t *size, uint32_t options);
    int32_t (*unmap)(void *context, uint32_t type, uint64_t address);
    int32_t (*close)(void *context);
} extgpu_test_transport;
/* Runs the real protocol validator against a test-owned scalar transport. */
extgpu_status extgpu_test_open(const extgpu_test_transport *transport, void *context,
                               extgpu_session **out_session, extgpu_device **out_device);
struct openpci_mmio_window;
/* Adopt a test-owned window already being serviced by a test worker. */
extgpu_status extgpu_test_bind_mmio_window(extgpu_device *device, struct openpci_mmio_window *window);
/* Separate private injection preserves extgpu_test_transport positional ABI.
 * Test devices never fall back to native registry access without a snapshot. */
typedef struct extgpu_test_provider_snapshot {
    uint64_t registry_id;
    int32_t native_status;
    uint32_t dictionary_created, iterator_present;
    uint32_t iterator_valid_before, iterator_valid_after;
    uint64_t entry_count;
} extgpu_test_provider_snapshot;
extgpu_status extgpu_test_set_registry_ids(extgpu_device *device, uint64_t driver_id,
                                          uint64_t pci_id);
/* Changes only the observed topology. Zero IDs model an absent scope. */
extgpu_status extgpu_test_set_reset_scope(extgpu_device *device, uint64_t bridge, uint64_t audio);
extgpu_status extgpu_test_set_provider_snapshot(extgpu_device *device,
                                     const extgpu_test_provider_snapshot *snapshot);
/* Separate recovery endpoint injection, never a native fallback. Lookup
 * reports actual registry/class metadata; the SDK validates before open. */
typedef struct extgpu_test_recovery_hooks {
    int32_t (*lookup)(void *context, uint64_t expected_registry_id,
                      uint64_t *actual_registry_id, const char **actual_user_class);
    int32_t (*open)(void *context);
    int32_t (*drop_original_local)(void *context);
} extgpu_test_recovery_hooks;
extgpu_status extgpu_test_set_recovery_transport(extgpu_device *device,
    const extgpu_test_transport *broker_transport, const extgpu_test_recovery_hooks *hooks,
    void *broker_context);
#endif
