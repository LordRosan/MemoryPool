#include <memory_pool/memory_pool.hpp>

#include <cstddef>
#include <list>
#include <memory_resource>

int main() {
    memory_pool::pool_options options;
    options.block_size = sizeof(int);
    options.block_alignment = alignof(int);
    options.blocks_per_slab = 8;
    options.max_blocks = 16;

    memory_pool::fixed_block_pool pool(options);
    void* pointer = pool.allocate();
    pool.deallocate(pointer);

    memory_pool::pool_memory_resource resource(pool);
    std::list<int, memory_pool::pool_allocator<int>> values{memory_pool::pool_allocator<int>(resource)};
    values.push_back(1);
    values.push_back(2);

    return values.size() == 2 && memory_pool::version() != nullptr ? 0 : 1;
}
