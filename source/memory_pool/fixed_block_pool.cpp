#include "memory_pool/fixed_block_pool.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace memory_pool {
    namespace {
        [[nodiscard]] bool is_power_of_two(std::size_t value) noexcept {
            return value != 0 && (value & (value - 1)) == 0;
        }
    } // namespace

    fixed_block_pool::slab::slab(void *data_value, std::size_t byte_count, std::size_t align) noexcept
        : data(data_value), bytes(byte_count), alignment(align) {
    }

    fixed_block_pool::slab::~slab() {
        if (data != nullptr) {
            ::operator delete(data, std::align_val_t(alignment));
        }
    }

    fixed_block_pool::slab::slab(slab &&other) noexcept
        : data(other.data), bytes(other.bytes), alignment(other.alignment) {
        other.data = nullptr;
        other.bytes = 0;
    }

    fixed_block_pool::slab &fixed_block_pool::slab::operator=(slab &&other) noexcept {
        if (this != &other) {
            if (data != nullptr) {
                ::operator delete(data, std::align_val_t(alignment));
            }
            data = other.data;
            bytes = other.bytes;
            alignment = other.alignment;
            other.data = nullptr;
            other.bytes = 0;
        }
        return *this;
    }

    fixed_block_pool::fixed_block_pool(pool_options options)
        : options_(options),
          block_size_(std::max(options.block_size, sizeof(free_node))),
          block_alignment_(normalize_alignment(options.block_alignment)) {
        if (options_.blocks_per_slab == 0) {
            throw std::invalid_argument("memory_pool::fixed_block_pool requires blocks_per_slab > 0");
        }
        block_stride_ = align_up(block_size_, block_alignment_);
        stats_.block_size = options_.block_size;
        stats_.block_stride = block_stride_;
        stats_.block_alignment = block_alignment_;
        stats_.blocks_per_slab = options_.blocks_per_slab;
        if (options_.enable_tracking) {
            active_blocks_.reserve(options_.blocks_per_slab);
        }
    }

    fixed_block_pool::~fixed_block_pool() = default;

    void *fixed_block_pool::allocate() {
        std::lock_guard lock(mutex_);
        return allocate_unlocked();
    }

    void *fixed_block_pool::try_allocate() noexcept {
        try {
            return allocate();
        } catch (...) {
            return nullptr;
        }
    }

    void fixed_block_pool::allocate_bulk(void **output, std::size_t count) {
        if (output == nullptr && count != 0) {
            throw std::invalid_argument("memory_pool::fixed_block_pool allocate_bulk requires non-null output");
        }

        std::lock_guard lock(mutex_);
        std::size_t allocated = 0;
        try {
            for (; allocated < count; ++allocated) {
                output[allocated] = allocate_unlocked();
            }
        } catch (...) {
            for (std::size_t i = 0; i < allocated; ++i) {
                (void) try_deallocate_unlocked(output[i], false);
                output[i] = nullptr;
            }
            throw;
        }
    }

    std::size_t fixed_block_pool::try_allocate_bulk(void **output, std::size_t count) noexcept {
        if (output == nullptr && count != 0) {
            return 0;
        }

        std::lock_guard lock(mutex_);
        std::size_t allocated = 0;
        for (; allocated < count; ++allocated) {
            try {
                output[allocated] = allocate_unlocked();
            } catch (...) {
                break;
            }
        }
        for (std::size_t i = allocated; i < count; ++i) {
            output[i] = nullptr;
        }
        return allocated;
    }

    void *fixed_block_pool::allocate_unlocked() {
        if (free_list_ == nullptr) {
            try {
                add_slab_unlocked(options_.blocks_per_slab);
            } catch (...) {
                pool_stats::increment(stats_.failed_allocation_count);
                throw;
            }
        }

        free_node *node = free_list_;
        if (options_.enable_tracking) {
            try {
                active_blocks_.insert(node);
            } catch (...) {
                pool_stats::increment(stats_.failed_allocation_count);
                throw;
            }
        }
        free_list_ = node->next;
        if (options_.zero_on_allocate) {
            std::memset(static_cast<void *>(node), 0, block_size_);
        }

        pool_stats::increment(stats_.allocation_count);
        ++stats_.used_blocks;
        --stats_.free_blocks;
        stats_.peak_used_blocks = std::max(stats_.peak_used_blocks, stats_.used_blocks);
        return node;
    }

    void fixed_block_pool::deallocate(void *pointer) {
        if (!try_deallocate(pointer)) {
            throw std::invalid_argument(
                "memory_pool::fixed_block_pool deallocate received a pointer not owned by the pool");
        }
    }

    bool fixed_block_pool::try_deallocate(void *pointer) noexcept {
        if (pointer == nullptr) {
            return true;
        }
        std::lock_guard lock(mutex_);
        return try_deallocate_unlocked(pointer);
    }

    void fixed_block_pool::deallocate_bulk(void *const*pointers, std::size_t count) {
        if (pointers == nullptr && count != 0) {
            throw std::invalid_argument("memory_pool::fixed_block_pool deallocate_bulk requires non-null pointers");
        }

        std::lock_guard lock(mutex_);
        for (std::size_t i = 0; i < count; ++i) {
            if (!try_deallocate_unlocked(pointers[i])) {
                throw std::invalid_argument(
                    "memory_pool::fixed_block_pool deallocate_bulk received a pointer not owned by the pool");
            }
        }
    }

    std::size_t fixed_block_pool::try_deallocate_bulk(void *const*pointers, std::size_t count) noexcept {
        if (pointers == nullptr && count != 0) {
            return 0;
        }

        std::lock_guard lock(mutex_);
        std::size_t deallocated = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (try_deallocate_unlocked(pointers[i])) {
                ++deallocated;
            }
        }
        return deallocated;
    }

    void fixed_block_pool::reserve(std::size_t block_count) {
        std::lock_guard lock(mutex_);
        while (stats_.total_blocks < block_count) {
            add_slab_unlocked(block_count - stats_.total_blocks);
        }
    }

    std::size_t fixed_block_pool::release_free_slabs() {
        std::lock_guard lock(mutex_);
        if (!options_.enable_tracking || slabs_.empty()) {
            return 0;
        }

        std::vector<slab> retained;
        retained.reserve(slabs_.size());
        std::size_t released = 0;
        std::size_t retained_blocks = 0;

        for (slab &item: slabs_) {
            const auto begin = reinterpret_cast<std::uintptr_t>(item.data);
            const auto end = begin + item.bytes;
            bool has_active_block = false;
            for (void *active: active_blocks_) {
                const auto address = reinterpret_cast<std::uintptr_t>(active);
                if (address >= begin && address < end) {
                    has_active_block = true;
                    break;
                }
            }

            if (has_active_block) {
                retained_blocks += item.bytes / block_stride_;
                retained.push_back(std::move(item));
            } else {
                ++released;
            }
        }

        if (released == 0) {
            return 0;
        }

        slabs_ = std::move(retained);
        free_list_ = nullptr;
        for (slab &item: slabs_) {
            auto *begin = static_cast<std::byte *>(item.data);
            const std::size_t block_count = item.bytes / block_stride_;
            for (std::size_t i = 0; i < block_count; ++i) {
                auto *node = reinterpret_cast<free_node *>(begin + i * block_stride_);
                if (active_blocks_.find(node) == active_blocks_.end()) {
                    node->next = free_list_;
                    free_list_ = node;
                }
            }
        }

        stats_.slab_count = slabs_.size();
        stats_.total_blocks = retained_blocks;
        stats_.used_blocks = active_blocks_.size();
        stats_.free_blocks = retained_blocks - stats_.used_blocks;
        return released;
    }

    void fixed_block_pool::clear() {
        std::lock_guard lock(mutex_);
        slabs_.clear();
        free_list_ = nullptr;
        active_blocks_.clear();
        stats_.slab_count = 0;
        stats_.total_blocks = 0;
        stats_.used_blocks = 0;
        stats_.free_blocks = 0;
        stats_.peak_used_blocks = 0;
    }

    bool fixed_block_pool::owns(const void *pointer) const noexcept {
        if (pointer == nullptr) {
            return false;
        }
        std::lock_guard lock(mutex_);
        return owns_unlocked(pointer);
    }

    pool_stats fixed_block_pool::stats() const noexcept {
        std::lock_guard lock(mutex_);
        return stats_;
    }

    std::size_t fixed_block_pool::normalize_alignment(std::size_t alignment) {
        if (!is_power_of_two(alignment)) {
            throw std::invalid_argument("memory_pool::fixed_block_pool requires power-of-two alignment");
        }
        return std::max(alignment, alignof(free_node));
    }

    std::size_t fixed_block_pool::align_up(std::size_t value, std::size_t alignment) {
        if (value > std::numeric_limits<std::size_t>::max() - (alignment - 1)) {
            throw std::overflow_error("memory_pool::fixed_block_pool block stride overflow");
        }
        return (value + alignment - 1) & ~(alignment - 1);
    }

    void fixed_block_pool::add_slab_unlocked(std::size_t requested_blocks) {
        std::size_t block_count = std::max(options_.blocks_per_slab, requested_blocks);
        if (options_.max_blocks != 0) {
            if (stats_.total_blocks >= options_.max_blocks) {
                throw std::bad_alloc();
            }
            block_count = std::min(block_count, options_.max_blocks - stats_.total_blocks);
        }
        if (block_count == 0 || block_count > std::numeric_limits<std::size_t>::max() / block_stride_) {
            throw std::bad_alloc();
        }
        if (stats_.total_blocks > std::numeric_limits<std::size_t>::max() - block_count
            || stats_.free_blocks > std::numeric_limits<std::size_t>::max() - block_count) {
            throw std::bad_alloc();
        }

        const std::size_t total_bytes = block_count * block_stride_;
        void *storage = ::operator new(total_bytes, std::align_val_t(block_alignment_));
        slab new_slab(storage, total_bytes, block_alignment_);
        slabs_.push_back(std::move(new_slab));

        auto *begin = static_cast<std::byte *>(storage);
        for (std::size_t i = 0; i < block_count; ++i) {
            auto *node = reinterpret_cast<free_node *>(begin + i * block_stride_);
            node->next = free_list_;
            free_list_ = node;
        }

        pool_stats::increment(stats_.slab_count);
        stats_.total_blocks += block_count;
        stats_.free_blocks += block_count;
    }

    bool fixed_block_pool::owns_unlocked(const void *pointer) const noexcept {
        const auto address = reinterpret_cast<std::uintptr_t>(pointer);
        for (const slab &item: slabs_) {
            const auto begin = reinterpret_cast<std::uintptr_t>(item.data);
            const auto end = begin + item.bytes;
            if (address >= begin && address < end && ((address - begin) % block_stride_) == 0) {
                return true;
            }
        }
        return false;
    }

    bool fixed_block_pool::try_deallocate_unlocked(void *pointer, bool count_stats) noexcept {
        if (pointer == nullptr) {
            return true;
        }
        if (!owns_unlocked(pointer)) {
            return false;
        }
        if (options_.enable_tracking && active_blocks_.erase(pointer) == 0) {
            return false;
        }
        if (options_.scribble_on_deallocate) {
            std::memset(static_cast<void *>(pointer), 0xDD, block_size_);
        }
        auto *node = static_cast<free_node *>(pointer);
        node->next = free_list_;
        free_list_ = node;
        --stats_.used_blocks;
        ++stats_.free_blocks;
        if (count_stats) {
            pool_stats::increment(stats_.deallocation_count);
        }
        return true;
    }
} // namespace memory_pool
