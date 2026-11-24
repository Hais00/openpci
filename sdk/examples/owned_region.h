/* SPDX-License-Identifier: MIT */
#ifndef EXTGPU_EXAMPLE_OWNED_REGION_H
#define EXTGPU_EXAMPLE_OWNED_REGION_H
#include <extgpu/extgpu.h>
typedef struct extgpu_example_region {
    void *host_backing;
    uint64_t bytes;
    extgpu_buffer *registration;
    extgpu_dma_coverage coverage;
} extgpu_example_region;
/* Device must already be claimed and have an explicitly selected DMA width. */
extgpu_status extgpu_example_region_create(extgpu_device *device, uint64_t key,
                                            extgpu_example_region *out_region);
/* Caller has stopped GPU references. On any failure the record remains owned;
 * do not drop it, free host_backing, close device, or pretend cleanup passed. */
extgpu_status extgpu_example_region_retire(extgpu_example_region *region);
#endif
