#include "DMARetirement.hpp"
#include <cstddef>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); std::exit(1); } } while (0)

struct Chunk {
    bool valid;
    unsigned *dmaCommand;
    unsigned *memoryDescriptor;
};
struct Operations {
    std::int32_t status[3] = {0, 0, 0};
    unsigned completed[3] = {0, 0, 0};
    unsigned commandsReleased[3] = {0, 0, 0};
    unsigned descriptorsReleased[3] = {0, 0, 0};
    unsigned failures = 0;
    std::int32_t complete(Chunk &chunk) {
        const unsigned id = *chunk.dmaCommand;
        ++completed[id];
        return status[id];
    }
    void failed(std::size_t, Chunk &, std::int32_t) { ++failures; }
    void release_command(Chunk &chunk) {
        ++commandsReleased[*chunk.dmaCommand];
        chunk.dmaCommand = nullptr;
    }
    void release_descriptor(Chunk &chunk) {
        if (chunk.memoryDescriptor) {
            ++descriptorsReleased[*chunk.memoryDescriptor];
            chunk.memoryDescriptor = nullptr;
        }
    }
};

int main()
{
    bool failed = false;
    std::uint64_t pending = 0, history = 0;
    extgpu_driver::record_completion_failure(failed, pending, history);
    extgpu_driver::record_completion_failure(failed, pending, history);
    CHECK(failed && pending == 1 && history == 2);
    extgpu_driver::record_completion_success(failed, pending);
    CHECK(!failed && pending == 0 && history == 2);
    extgpu_driver::record_completion_success(failed, pending);
    CHECK(pending == 0 && history == 2);

    unsigned ids[] = {0, 1, 2};
    Chunk chunks[] = {{true, &ids[0], &ids[0]}, {true, &ids[1], &ids[1]}, {true, &ids[2], &ids[2]}};
    Operations operations;
    operations.status[0] = -5;
    operations.status[2] = -9;
    CHECK(extgpu_driver::retire_prepared_chunks(chunks, 3, operations) == -5);
    CHECK(operations.failures == 2);
    CHECK(chunks[0].valid && chunks[0].dmaCommand && chunks[0].memoryDescriptor);
    CHECK(chunks[2].valid && chunks[2].dmaCommand && chunks[2].memoryDescriptor);
    CHECK(!chunks[1].valid && !chunks[1].dmaCommand && !chunks[1].memoryDescriptor);
    CHECK(operations.commandsReleased[0] == 0 && operations.descriptorsReleased[0] == 0);
    CHECK(operations.commandsReleased[2] == 0 && operations.descriptorsReleased[2] == 0);
    CHECK(operations.commandsReleased[1] == 1 && operations.descriptorsReleased[1] == 1);

    // Retry releases only the commands whose completion was previously denied.
    operations.status[0] = operations.status[2] = 0;
    CHECK(extgpu_driver::retire_prepared_chunks(chunks, 3, operations) == 0);
    for (unsigned id : ids) {
        CHECK(!chunks[id].valid && !chunks[id].dmaCommand && !chunks[id].memoryDescriptor);
        CHECK(operations.commandsReleased[id] == 1 && operations.descriptorsReleased[id] == 1);
    }
    CHECK(operations.completed[1] == 1);
    CHECK(operations.completed[0] == 2 && operations.completed[2] == 2);

    // A completed command may already be gone; its descriptor still needs one release.
    Chunk descriptorOnly{true, nullptr, &ids[0]};
    Operations only;
    CHECK(extgpu_driver::retire_prepared_chunks(&descriptorOnly, 1, only) == 0);
    CHECK(!descriptorOnly.valid && !descriptorOnly.memoryDescriptor);
    CHECK(only.completed[0] == 0 && only.commandsReleased[0] == 0 && only.descriptorsReleased[0] == 1);
    CHECK(extgpu_driver::retire_prepared_chunks<Chunk>(nullptr, 0, only) == 0);
    std::puts("PASS actual driver retirement algorithm: failed completion retains command/descriptor, retry releases once; no DriverKit/hardware claim");
    return 0;
}
