#pragma once

#include <cstddef>
#include <limits>
#include <memory_resource>
#include <new>
#include <type_traits>

namespace memory_pool {

template <typename T>
class pool_allocator {
public:
    using value_type = T;
    using pointer = T*;
    using const_pointer = const T*;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using propagate_on_container_move_assignment = std::true_type;
    using is_always_equal = std::false_type;

    template <typename U>
    struct rebind {
        using other = pool_allocator<U>;
    };

    pool_allocator() noexcept
        : resource_(std::pmr::get_default_resource()) {}

    explicit pool_allocator(std::pmr::memory_resource& resource) noexcept
        : resource_(&resource) {}

    explicit pool_allocator(std::pmr::memory_resource* resource) noexcept
        : resource_(resource == nullptr ? std::pmr::get_default_resource() : resource) {}

    template <typename U>
    pool_allocator(const pool_allocator<U>& other) noexcept
        : resource_(&other.resource()) {}

    [[nodiscard]] T* allocate(std::size_t count) {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            throw std::bad_array_new_length();
        }
        return static_cast<T*>(resource_->allocate(count * sizeof(T), alignof(T)));
    }

    void deallocate(T* pointer, std::size_t count) noexcept {
        resource_->deallocate(pointer, count * sizeof(T), alignof(T));
    }

    [[nodiscard]] std::pmr::memory_resource& resource() const noexcept {
        return *resource_;
    }

    template <typename U>
    [[nodiscard]] bool operator==(const pool_allocator<U>& other) const noexcept {
        return resource_->is_equal(other.resource());
    }

    template <typename U>
    [[nodiscard]] bool operator!=(const pool_allocator<U>& other) const noexcept {
        return !(*this == other);
    }

private:
    template <typename>
    friend class pool_allocator;

    std::pmr::memory_resource* resource_;
};

} // namespace memory_pool
