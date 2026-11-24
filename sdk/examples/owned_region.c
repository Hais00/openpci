/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Embedding example; no main, device discovery/claim or GPU submission. */
#include "owned_region.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extgpu_status extgpu_example_region_create(extgpu_device *device, uint64_t key,
                                            extgpu_example_region *out)
{
    if (!device || !out) return EXTGPU_E_ARGUMENT;
    memset(out, 0, sizeof(*out));
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) return EXTGPU_E_IO;
    out->bytes = (uint64_t)page;
    if (posix_memalign(&out->host_backing, (size_t)page, (size_t)page)) return EXTGPU_E_MEMORY;
    memset(out->host_backing, 0xa5, (size_t)page);
    extgpu_status st = extgpu_dma_register(device, key, out->host_backing, out->bytes,
                                          &out->registration, &out->coverage);
    if (st != EXTGPU_OK && !out->registration) {
        /* Only absence of a retained handle permits releasing this backing. */
        free(out->host_backing); memset(out, 0, sizeof(*out));
    }
    return st;
}
extgpu_status extgpu_example_region_retire(extgpu_example_region *region)
{
    if (!region) return EXTGPU_E_ARGUMENT;
    extgpu_status st = region->registration ? extgpu_buffer_unregister(&region->registration) : EXTGPU_OK;
    if (st != EXTGPU_OK) return st;
    free(region->host_backing); memset(region, 0, sizeof(*region));
    return EXTGPU_OK;
}
