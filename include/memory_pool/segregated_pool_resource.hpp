#pragma once

#include "memory_pool/fixed_block_pool.hpp"

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <vector>

namespace memory_pool {
    struct segregated_pool_options {
        std::vector<std::size_t> size_classes = {16, 32, 64, 128, 256, 512, 1024, 2048};
        std::size_t blocks_per_slab = 1024;
        bool zero_on_allocate = false;
        bool scribble_on_deallocate = false;
        bool enable_tracking = tracking_enabled_by_default;
        std::pmr::memory_resource *upstream = std::pmr::get_default_resource();
    };

    struct size_class_stats {
        std::size_t size_class = 0;
        pool_stats stats;
    };

    class segregated_pool_resource final : public std::pmr::memory_resource {
    public:
        explicit segregated_pool_resource(segregated_pool_options options = {});

        [[nodiscard]] std::vector<size_class_stats> stats() const;

        [[nodiscard]] std::pmr::memory_resource &upstream_resource() noexcept { return *upstream_; }

    private:
        [[nodiscard]] void *do_allocate(std::size_t bytes, std::size_t alignment) override;

        void do_deallocate(void *pointer, std::size_t bytes, std::size_t alignment) override;

        [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override;

        [[nodiscard]] std::size_t select_size_class(std::size_t bytes, std::size_t alignment) const noexcept;

        std::vector<std::size_t> size_classes_;
        std::vector<std::unique_ptr<fixed_block_pool> > pools_;
        std::pmr::memory_resource *upstream_;
    };
} // namespace memory_pool
