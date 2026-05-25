#pragma once

#include "memory_pool/fixed_block_pool.hpp"

#include <memory>
#include <type_traits>
#include <utility>

namespace memory_pool {

template <typename T>
class object_pool {
public:
    explicit object_pool(std::size_t objects_per_slab = 1024, std::size_t max_objects = 0)
        : object_pool(make_options(objects_per_slab, max_objects)) {}

    explicit object_pool(pool_options options)
        : pool_(normalize_options(options)) {}

    template <typename... Args>
    [[nodiscard]] T* create(Args&&... args) {
        void* storage = pool_.allocate();
        try {
            return std::construct_at(static_cast<T*>(storage), std::forward<Args>(args)...);
        } catch (...) {
            pool_.deallocate(storage);
            throw;
        }
    }

    void destroy(T* object) {
        if (object == nullptr) {
            return;
        }
        std::destroy_at(object);
        pool_.deallocate(object);
    }

    [[nodiscard]] fixed_block_pool& pool() noexcept { return pool_; }
    [[nodiscard]] const fixed_block_pool& pool() const noexcept { return pool_; }
    [[nodiscard]] pool_stats stats() const noexcept { return pool_.stats(); }

private:
    static pool_options make_options(std::size_t objects_per_slab, std::size_t max_objects) {
        pool_options options;
        options.block_size = sizeof(T);
        options.block_alignment = alignof(T);
        options.blocks_per_slab = objects_per_slab;
        options.max_blocks = max_objects;
        return options;
    }

    static pool_options normalize_options(pool_options options) {
        options.block_size = sizeof(T);
        options.block_alignment = alignof(T);
        return options;
    }

    fixed_block_pool pool_;
};

} // namespace memory_pool
