#pragma once

#include "memory_pool/fixed_block_pool.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <utility>
#include <vector>

namespace memory_pool {

struct thread_cache_options {
    std::size_t max_cached_blocks = 256;
    std::size_t refill_count = 64;
    std::size_t release_count = 64;
    bool validate_ownership_on_deallocate = false;
};

class thread_cached_fixed_block_pool {
public:
    class local_cache {
    public:
        explicit local_cache(thread_cached_fixed_block_pool& owner)
            : owner_(&owner) {
            cached_.reserve(owner_->cache_options_.max_cached_blocks);
        }

        ~local_cache() {
            flush_noexcept();
        }

        local_cache(local_cache&& other) noexcept
            : owner_(other.owner_),
              cached_(std::move(other.cached_)) {
            other.owner_ = nullptr;
        }

        local_cache& operator=(local_cache&& other) noexcept {
            if (this != &other) {
                flush_noexcept();
                owner_ = other.owner_;
                cached_ = std::move(other.cached_);
                other.owner_ = nullptr;
            }
            return *this;
        }

        local_cache(const local_cache&) = delete;
        local_cache& operator=(const local_cache&) = delete;

        [[nodiscard]] void* allocate() {
            if (cached_.empty()) {
                refill();
            }
            if (cached_.empty()) {
                return owner().upstream_.allocate();
            }

            void* pointer = cached_.back();
            cached_.pop_back();
            return pointer;
        }

        [[nodiscard]] void* try_allocate() noexcept {
            try {
                return allocate();
            } catch (...) {
                return nullptr;
            }
        }

        void deallocate(void* pointer) {
            if (pointer == nullptr) {
                return;
            }
            if (owner().cache_options_.validate_ownership_on_deallocate && !owner().upstream_.owns(pointer)) {
                throw std::invalid_argument(
                    "memory_pool::thread_cached_fixed_block_pool local_cache received a pointer not owned by the pool");
            }
            if (std::find(cached_.begin(), cached_.end(), pointer) != cached_.end()) {
                throw std::invalid_argument(
                    "memory_pool::thread_cached_fixed_block_pool local_cache received a duplicate pointer");
            }
            if (cached_.size() >= owner().cache_options_.max_cached_blocks) {
                release_some();
            }
            cached_.push_back(pointer);
        }

        [[nodiscard]] bool try_deallocate(void* pointer) noexcept {
            try {
                deallocate(pointer);
                return true;
            } catch (...) {
                return false;
            }
        }

        void flush() {
            if (owner_ == nullptr || cached_.empty()) {
                return;
            }
            owner_->upstream_.deallocate_bulk(cached_.data(), cached_.size());
            cached_.clear();
        }

        [[nodiscard]] std::size_t cached_blocks() const noexcept {
            return cached_.size();
        }

    private:
        [[nodiscard]] thread_cached_fixed_block_pool& owner() const {
            if (owner_ == nullptr) {
                throw std::logic_error("memory_pool::thread_cached_fixed_block_pool local_cache has no owner");
            }
            return *owner_;
        }

        void refill() {
            const auto& options = owner().cache_options_;
            const std::size_t request = std::min(options.refill_count, options.max_cached_blocks - cached_.size());
            if (request == 0) {
                return;
            }

            const std::size_t offset = cached_.size();
            cached_.resize(offset + request, nullptr);
            const std::size_t allocated = owner_->upstream_.try_allocate_bulk(cached_.data() + offset, request);
            cached_.resize(offset + allocated);
        }

        void release_some() {
            const std::size_t release_count = std::min(owner().cache_options_.release_count, cached_.size());
            if (release_count == 0) {
                return;
            }

            const std::size_t offset = cached_.size() - release_count;
            owner_->upstream_.deallocate_bulk(cached_.data() + offset, release_count);
            cached_.resize(offset);
        }

        void flush_noexcept() noexcept {
            if (owner_ == nullptr || cached_.empty()) {
                return;
            }
            (void)owner_->upstream_.try_deallocate_bulk(cached_.data(), cached_.size());
            cached_.clear();
        }

        thread_cached_fixed_block_pool* owner_ = nullptr;
        std::vector<void*> cached_;
    };

    explicit thread_cached_fixed_block_pool(pool_options pool_config, thread_cache_options cache_config = {})
        : cache_options_(normalize_cache_options(cache_config)),
          upstream_(pool_config) {}

    [[nodiscard]] local_cache make_cache() {
        return local_cache(*this);
    }

    [[nodiscard]] fixed_block_pool& upstream_pool() noexcept {
        return upstream_;
    }

    [[nodiscard]] const fixed_block_pool& upstream_pool() const noexcept {
        return upstream_;
    }

    [[nodiscard]] pool_stats stats() const noexcept {
        return upstream_.stats();
    }

    [[nodiscard]] thread_cache_options cache_options() const noexcept {
        return cache_options_;
    }

private:
    [[nodiscard]] static thread_cache_options normalize_cache_options(thread_cache_options options) {
        if (options.max_cached_blocks == 0) {
            throw std::invalid_argument(
                "memory_pool::thread_cached_fixed_block_pool requires max_cached_blocks > 0");
        }
        if (options.refill_count == 0) {
            throw std::invalid_argument("memory_pool::thread_cached_fixed_block_pool requires refill_count > 0");
        }
        if (options.release_count == 0) {
            throw std::invalid_argument("memory_pool::thread_cached_fixed_block_pool requires release_count > 0");
        }
        options.refill_count = std::min(options.refill_count, options.max_cached_blocks);
        options.release_count = std::min(options.release_count, options.max_cached_blocks);
        return options;
    }

    thread_cache_options cache_options_;
    fixed_block_pool upstream_;
};

} // namespace memory_pool
