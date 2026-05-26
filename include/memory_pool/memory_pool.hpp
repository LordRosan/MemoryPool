#pragma once

#include "memory_pool/config.hpp"
#include "memory_pool/fixed_block_pool.hpp"
#include "memory_pool/object_pool.hpp"
#include "memory_pool/pool_allocator.hpp"
#include "memory_pool/pool_memory_resource.hpp"
#include "memory_pool/segregated_pool_resource.hpp"
#include "memory_pool/sharded_fixed_block_pool.hpp"
#include "memory_pool/thread_cached_fixed_block_pool.hpp"

namespace memory_pool {

[[nodiscard]] const char* version() noexcept;

} // namespace memory_pool
