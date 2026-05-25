#include "memory_pool/pool_memory_resource.hpp"

namespace memory_pool {

pool_memory_resource::pool_memory_resource(
    fixed_block_pool& pool,
    std::pmr::memory_resource* upstream) noexcept
    : pool_(&pool), upstream_(upstream == nullptr ? std::pmr::get_default_resource() : upstream) {}

void* pool_memory_resource::do_allocate(std::size_t bytes, std::size_t alignment) {
    if (bytes <= pool_->block_size() && alignment <= pool_->block_alignment()) {
        return pool_->allocate();
    }
    return upstream_->allocate(bytes, alignment);
}

void pool_memory_resource::do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) {
    if (pointer == nullptr) {
        return;
    }
    if (pool_->owns(pointer)) {
        pool_->deallocate(pointer);
        return;
    }
    upstream_->deallocate(pointer, bytes, alignment);
}

bool pool_memory_resource::do_is_equal(const std::pmr::memory_resource& other) const noexcept {
    return this == &other;
}

} // namespace memory_pool
