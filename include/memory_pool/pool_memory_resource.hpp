#pragma once

#include "memory_pool/fixed_block_pool.hpp"

#include <memory_resource>

namespace memory_pool {
    class pool_memory_resource final : public std::pmr::memory_resource {
    public:
        explicit pool_memory_resource(
            fixed_block_pool &pool,
            std::pmr::memory_resource *upstream = std::pmr::get_default_resource()) noexcept;

        [[nodiscard]] fixed_block_pool &pool() noexcept { return *pool_; }
        [[nodiscard]] const fixed_block_pool &pool() const noexcept { return *pool_; }

    private:
        void *do_allocate(std::size_t bytes, std::size_t alignment) override;

        void do_deallocate(void *pointer, std::size_t bytes, std::size_t alignment) override;

        [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override;

        fixed_block_pool *pool_;
        std::pmr::memory_resource *upstream_;
    };
} // namespace memory_pool
