#include "memory_pool/segregated_pool_resource.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace memory_pool {
    namespace {
        constexpr std::size_t no_size_class = std::numeric_limits<std::size_t>::max();
    } // namespace

    segregated_pool_resource::segregated_pool_resource(segregated_pool_options options)
        : size_classes_(std::move(options.size_classes)),
          upstream_(options.upstream == nullptr ? std::pmr::get_default_resource() : options.upstream) {
        if (size_classes_.empty()) {
            throw std::invalid_argument("memory_pool::segregated_pool_resource requires at least one size class");
        }
        if (options.blocks_per_slab == 0) {
            throw std::invalid_argument("memory_pool::segregated_pool_resource requires blocks_per_slab > 0");
        }

        std::sort(size_classes_.begin(), size_classes_.end());
        size_classes_.erase(std::unique(size_classes_.begin(), size_classes_.end()), size_classes_.end());
        if (size_classes_.front() == 0) {
            throw std::invalid_argument("memory_pool::segregated_pool_resource size classes must be greater than zero");
        }

        pools_.reserve(size_classes_.size());
        for (const std::size_t size_class: size_classes_) {
            pool_options pool_config;
            pool_config.block_size = size_class;
            pool_config.block_alignment = alignof(std::max_align_t);
            pool_config.blocks_per_slab = options.blocks_per_slab;
            pool_config.zero_on_allocate = options.zero_on_allocate;
            pool_config.scribble_on_deallocate = options.scribble_on_deallocate;
            pool_config.enable_tracking = options.enable_tracking;
            pools_.push_back(std::make_unique<fixed_block_pool>(pool_config));
        }
    }

    std::vector<size_class_stats> segregated_pool_resource::stats() const {
        std::vector<size_class_stats> snapshot;
        snapshot.reserve(pools_.size());
        for (std::size_t i = 0; i < pools_.size(); ++i) {
            snapshot.push_back(size_class_stats{size_classes_[i], pools_[i]->stats()});
        }
        return snapshot;
    }

    void *segregated_pool_resource::do_allocate(std::size_t bytes, std::size_t alignment) {
        bytes = std::max<std::size_t>(1, bytes);
        const std::size_t index = select_size_class(bytes, alignment);
        if (index == no_size_class) {
            return upstream_->allocate(bytes, alignment);
        }
        return pools_[index]->allocate();
    }

    void segregated_pool_resource::do_deallocate(void *pointer, std::size_t bytes, std::size_t alignment) {
        if (pointer == nullptr) {
            return;
        }
        bytes = std::max<std::size_t>(1, bytes);
        const std::size_t index = select_size_class(bytes, alignment);
        if (index == no_size_class) {
            upstream_->deallocate(pointer, bytes, alignment);
            return;
        }
        pools_[index]->deallocate(pointer);
    }

    bool segregated_pool_resource::do_is_equal(const std::pmr::memory_resource &other) const noexcept {
        return this == &other;
    }

    std::size_t segregated_pool_resource::select_size_class(std::size_t bytes, std::size_t alignment) const noexcept {
        if (alignment > alignof(std::max_align_t)) {
            return no_size_class;
        }
        const auto it = std::lower_bound(size_classes_.begin(), size_classes_.end(), bytes);
        if (it == size_classes_.end()) {
            return no_size_class;
        }
        return static_cast<std::size_t>(std::distance(size_classes_.begin(), it));
    }
} // namespace memory_pool
