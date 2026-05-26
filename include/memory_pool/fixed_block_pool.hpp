#pragma once

#include "memory_pool/config.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace memory_pool {

struct pool_options {
    std::size_t block_size = 64;
    std::size_t block_alignment = alignof(std::max_align_t);
    std::size_t blocks_per_slab = 1024;
    std::size_t max_blocks = 0;
    bool zero_on_allocate = false;
    bool scribble_on_deallocate = false;
    bool enable_tracking = tracking_enabled_by_default;
};

struct pool_stats {
    std::size_t block_size = 0;
    std::size_t block_stride = 0;
    std::size_t block_alignment = 0;
    std::size_t blocks_per_slab = 0;
    std::size_t slab_count = 0;
    std::size_t total_blocks = 0;
    std::size_t used_blocks = 0;
    std::size_t free_blocks = 0;
    std::size_t peak_used_blocks = 0;
    std::size_t allocation_count = 0;
    std::size_t deallocation_count = 0;
    std::size_t failed_allocation_count = 0;

    [[nodiscard]] std::size_t reserved_bytes() const noexcept {
        if (block_stride != 0 && total_blocks > std::numeric_limits<std::size_t>::max() / block_stride) {
            return std::numeric_limits<std::size_t>::max();
        }
        return total_blocks * block_stride;
    }

    static void increment(std::size_t& counter) noexcept {
        if (counter != std::numeric_limits<std::size_t>::max()) {
            ++counter;
        }
    }

    [[nodiscard]] static constexpr std::size_t saturating_add(std::size_t left, std::size_t right) noexcept {
        return left > std::numeric_limits<std::size_t>::max() - right
            ? std::numeric_limits<std::size_t>::max()
            : left + right;
    }
};

class fixed_block_pool {
public:
    explicit fixed_block_pool(pool_options options);
    ~fixed_block_pool();

    fixed_block_pool(const fixed_block_pool&) = delete;
    fixed_block_pool& operator=(const fixed_block_pool&) = delete;

    [[nodiscard]] void* allocate();
    [[nodiscard]] void* try_allocate() noexcept;
    void allocate_bulk(void** output, std::size_t count);
    [[nodiscard]] std::size_t try_allocate_bulk(void** output, std::size_t count) noexcept;
    void deallocate(void* pointer);
    [[nodiscard]] bool try_deallocate(void* pointer) noexcept;
    void deallocate_bulk(void* const* pointers, std::size_t count);
    [[nodiscard]] std::size_t try_deallocate_bulk(void* const* pointers, std::size_t count) noexcept;

    void reserve(std::size_t block_count);
    [[nodiscard]] std::size_t release_free_slabs();
    void clear();

    [[nodiscard]] bool owns(const void* pointer) const noexcept;
    [[nodiscard]] pool_stats stats() const noexcept;

    [[nodiscard]] std::size_t block_size() const noexcept { return block_size_; }
    [[nodiscard]] std::size_t block_stride() const noexcept { return block_stride_; }
    [[nodiscard]] std::size_t block_alignment() const noexcept { return block_alignment_; }

private:
    struct free_node {
        free_node* next = nullptr;
    };

    struct slab {
        void* data = nullptr;
        std::size_t bytes = 0;
        std::size_t alignment = alignof(std::max_align_t);

        slab() = default;
        slab(void* data, std::size_t bytes, std::size_t alignment) noexcept;
        ~slab();
        slab(slab&& other) noexcept;
        slab& operator=(slab&& other) noexcept;
        slab(const slab&) = delete;
        slab& operator=(const slab&) = delete;
    };

    static std::size_t normalize_alignment(std::size_t alignment);
    static std::size_t align_up(std::size_t value, std::size_t alignment);

    void add_slab_unlocked(std::size_t requested_blocks);
    [[nodiscard]] void* allocate_unlocked();
    [[nodiscard]] bool owns_unlocked(const void* pointer) const noexcept;
    [[nodiscard]] bool try_deallocate_unlocked(void* pointer, bool count_stats = true) noexcept;

    pool_options options_;
    std::size_t block_size_ = 0;
    std::size_t block_stride_ = 0;
    std::size_t block_alignment_ = 0;

    mutable std::mutex mutex_;
    std::vector<slab> slabs_;
    free_node* free_list_ = nullptr;
    pool_stats stats_;
    std::unordered_set<void*> active_blocks_;
};

} // namespace memory_pool
