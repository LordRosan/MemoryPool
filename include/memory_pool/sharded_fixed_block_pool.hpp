#pragma once

#include "memory_pool/fixed_block_pool.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace memory_pool {

class sharded_fixed_block_pool {
public:
    explicit sharded_fixed_block_pool(pool_options options, std::size_t shard_count = 0) {
        if (shard_count == 0) {
            shard_count = std::max(2u, std::thread::hardware_concurrency());
        }
        if (options.max_blocks != 0) {
            shard_count = std::min<std::size_t>(shard_count, options.max_blocks);
        }
        shard_count = std::max<std::size_t>(1, shard_count);

        shards_.reserve(shard_count);
        shard_limits_.reserve(shard_count);
        const std::size_t base_limit = options.max_blocks == 0 ? 0 : options.max_blocks / shard_count;
        const std::size_t extra = options.max_blocks == 0 ? 0 : options.max_blocks % shard_count;

        for (std::size_t i = 0; i < shard_count; ++i) {
            pool_options shard_options = options;
            if (options.max_blocks != 0) {
                shard_options.max_blocks = base_limit + (i < extra ? 1 : 0);
            }
            shard_limits_.push_back(shard_options.max_blocks);
            shards_.push_back(std::make_unique<fixed_block_pool>(shard_options));
        }
    }

    [[nodiscard]] void* allocate() {
        const std::size_t start = shard_index_for_current_thread();
        for (std::size_t offset = 0; offset < shards_.size(); ++offset) {
            void* pointer = shards_[(start + offset) % shards_.size()]->try_allocate();
            if (pointer != nullptr) {
                return pointer;
            }
        }
        throw std::bad_alloc();
    }

    [[nodiscard]] void* try_allocate() noexcept {
        try {
            return allocate();
        } catch (...) {
            return nullptr;
        }
    }

    void deallocate(void* pointer) {
        if (!try_deallocate(pointer)) {
            throw std::invalid_argument("memory_pool::sharded_fixed_block_pool deallocate received a pointer not owned by the pool");
        }
    }

    [[nodiscard]] bool try_deallocate(void* pointer) noexcept {
        if (pointer == nullptr) {
            return true;
        }
        const std::size_t start = shard_index_for_current_thread();
        for (std::size_t offset = 0; offset < shards_.size(); ++offset) {
            if (shards_[(start + offset) % shards_.size()]->try_deallocate(pointer)) {
                return true;
            }
        }
        return false;
    }

    void reserve(std::size_t block_count) {
        const std::size_t max_blocks = total_max_blocks();
        if (max_blocks != 0 && block_count > max_blocks) {
            throw std::bad_alloc();
        }

        const std::size_t base = block_count / shards_.size();
        const std::size_t extra = block_count % shards_.size();
        for (std::size_t i = 0; i < shards_.size(); ++i) {
            std::size_t target = base + (i < extra ? 1 : 0);
            if (shard_limits_[i] != 0) {
                target = std::min(target, shard_limits_[i]);
            }
            shards_[i]->reserve(target);
        }
    }

    [[nodiscard]] bool owns(const void* pointer) const noexcept {
        return std::any_of(shards_.begin(), shards_.end(), [pointer](const auto& shard) {
            return shard->owns(pointer);
        });
    }

    [[nodiscard]] pool_stats stats() const noexcept {
        pool_stats total;
        if (!shards_.empty()) {
            total = shards_.front()->stats();
            total.slab_count = 0;
            total.total_blocks = 0;
            total.used_blocks = 0;
            total.free_blocks = 0;
            total.peak_used_blocks = 0;
            total.allocation_count = 0;
            total.deallocation_count = 0;
            total.failed_allocation_count = 0;
        }
        for (const auto& shard : shards_) {
            const pool_stats current = shard->stats();
            total.slab_count = pool_stats::saturating_add(total.slab_count, current.slab_count);
            total.total_blocks = pool_stats::saturating_add(total.total_blocks, current.total_blocks);
            total.used_blocks = pool_stats::saturating_add(total.used_blocks, current.used_blocks);
            total.free_blocks = pool_stats::saturating_add(total.free_blocks, current.free_blocks);
            total.peak_used_blocks = pool_stats::saturating_add(total.peak_used_blocks, current.peak_used_blocks);
            total.allocation_count = pool_stats::saturating_add(total.allocation_count, current.allocation_count);
            total.deallocation_count = pool_stats::saturating_add(total.deallocation_count, current.deallocation_count);
            total.failed_allocation_count =
                pool_stats::saturating_add(total.failed_allocation_count, current.failed_allocation_count);
        }
        return total;
    }

    [[nodiscard]] std::size_t shard_count() const noexcept {
        return shards_.size();
    }

private:
    [[nodiscard]] std::size_t shard_index_for_current_thread() const noexcept {
        return std::hash<std::thread::id>{}(std::this_thread::get_id()) % shards_.size();
    }

    [[nodiscard]] std::size_t total_max_blocks() const noexcept {
        std::size_t total = 0;
        for (std::size_t limit : shard_limits_) {
            if (limit == 0) {
                return 0;
            }
            total = pool_stats::saturating_add(total, limit);
        }
        return total;
    }

    std::vector<std::unique_ptr<fixed_block_pool>> shards_;
    std::vector<std::size_t> shard_limits_;
};

} // namespace memory_pool
