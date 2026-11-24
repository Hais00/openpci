/* SPDX-License-Identifier: GPL-2.0-or-later
 * Platform-independent resource retirement used by the actual DriverKit code.
 * This does not emulate DriverKit: operations call the real DMA objects in the
 * extension, and tests inject only the success/failure of those operations. */
#ifndef EXTGPU_DMA_RETIREMENT_HPP
#define EXTGPU_DMA_RETIREMENT_HPP
#include <stddef.h>
#include <stdint.h>

namespace extgpu_driver {
inline void record_completion_failure(bool &failed, uint64_t &pending, uint64_t &history)
{
    ++history;
    if (!failed) {
        failed = true;
        ++pending;
    }
}

inline void record_completion_success(bool &failed, uint64_t &pending)
{
    if (failed) {
        failed = false;
        if (pending != 0) --pending;
    }
}

template <typename Chunk, typename Operations>
int32_t retire_prepared_chunks(Chunk *chunks, size_t count, Operations &operations)
{
    int32_t first_failure = 0;
    for (size_t index = 0; index < count; ++index) {
        Chunk &chunk = chunks[index];
        if (chunk.dmaCommand != nullptr) {
            const int32_t status = operations.complete(chunk);
            if (status != 0) {
                operations.failed(index, chunk, status);
                if (first_failure == 0) first_failure = status;
                // The command, descriptor and valid bit remain owned together.
                // In particular, a rollback is not allowed to unpin this chunk.
                continue;
            }
            operations.release_command(chunk);
        }
        operations.release_descriptor(chunk);
        chunk.valid = false;
    }
    return first_failure;
}
}
#endif
