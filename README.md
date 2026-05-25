# MemoryPool

MemoryPool is a compact C++20 library for fixed-size allocation workloads. It is structured as a reusable package with explicit API contracts, installable CMake exports, release-only validation, and deterministic benchmark reports.

## What It Provides

- `memory_pool::fixed_block_pool`: thread-safe fixed-block slab allocator with ownership checks, optional allocation tracking, exhaustion limits, and runtime statistics.
- `memory_pool::sharded_fixed_block_pool`: fixed-block allocator split across shards to reduce lock contention in concurrent workloads.
- `memory_pool::thread_cached_fixed_block_pool`: explicit per-thread cache front-end for reducing hot-path contention.
- `memory_pool::object_pool<T>`: typed construction/destruction helper built on the fixed-block pool.
- `memory_pool::pool_memory_resource`: `std::pmr::memory_resource` adapter for PMR-aware containers.
- `memory_pool::segregated_pool_resource`: PMR resource with multiple small-object size classes and upstream fallback for larger allocations.
- `memory_pool::config.hpp`: version constants and build-time default policy switches.
- CMake package export under `MemoryPool::memory_pool` for `add_subdirectory` or installed `find_package` use.

## Layout

```text
include/memory_pool/        Public headers
source/memory_pool/         Non-template implementation
cmake/                      Package config template
test.cpp                    Optional standalone correctness and benchmark runner
results/                    Generated benchmark reports
CMakePresets.json           Standard Release and Debug-library configure presets
.clang-format               Formatting policy
.editorconfig               Editor defaults
```

## Use As A Dependency

```cmake
add_subdirectory(path/to/MemoryPool)
target_link_libraries(your_target PRIVATE MemoryPool::memory_pool)
```

```cpp
#include <memory_pool/memory_pool.hpp>

memory_pool::pool_options options;
options.block_size = 64;
options.blocks_per_slab = 1024;

memory_pool::fixed_block_pool pool(options);
void* pointer = pool.allocate();
pool.deallocate(pointer);
```

For mixed small-object PMR workloads:

```cpp
#include <memory_pool/memory_pool.hpp>

memory_pool::segregated_pool_resource resource;
std::pmr::vector<std::pmr::string> values(&resource);
values.emplace_back("cached string");
```

## Design Notes

The implementation intentionally uses mature allocator building blocks instead of a black-box malloc replacement:

- Slab allocation amortizes system allocation cost.
- Embedded free lists make fixed-size allocate/deallocate O(1).
- Bulk allocate/deallocate APIs reduce repeated lock acquisition for batch-style allocation patterns.
- Sharding reduces shared mutex contention without hiding the basic algorithm.
- Explicit `local_cache` objects provide a thread-local fast path while keeping lifetime ownership visible.
- Size classes route common small allocations to fixed pools while preserving an upstream fallback.
- Optional tracking catches invalid and double free paths during diagnostics.
- Slab ownership is RAII-managed during growth, so allocation failures do not leak partially-created slabs.

## API Contracts

- `fixed_block_pool` is internally synchronized; `allocate`, `try_allocate`, `deallocate`, `try_deallocate`, `reserve`, `owns`, and `stats` may be called from multiple threads.
- `pool_options::enable_tracking` defaults to `true`, so invalid and double-free attempts are rejected instead of corrupting the free list. Disable it only for trusted hot paths where allocator misuse is treated as caller error.
- `deallocate(nullptr)` and `try_deallocate(nullptr)` are no-ops.
- `deallocate` throws `std::invalid_argument` for pointers not owned by the pool, or for double-free when tracking is enabled.
- `allocate` throws `std::bad_alloc` on exhaustion or upstream allocation failure; `try_allocate` returns `nullptr`.
- `try_allocate_bulk()` is best-effort and returns the number of blocks actually obtained; `allocate_bulk()` is all-or-throw and rolls back blocks allocated in the same call if it cannot complete.
- `deallocate_bulk()` releases a batch under one lock; `try_deallocate_bulk()` returns the number of pointers accepted.
- `thread_cached_fixed_block_pool::local_cache` is intended to be owned by one thread. The parent pool must outlive every `local_cache`; each cache flushes its retained blocks on destruction.
- `thread_cache_options::validate_ownership_on_deallocate` can enable an extra upstream ownership check for diagnostics, but the default fast path avoids that shared-lock check.
- `clear()` releases all slabs and invalidates every outstanding pointer previously returned by the pool.
- Lifetime counters such as `allocation_count`, `deallocation_count`, and `failed_allocation_count` are not reset by `clear()`.
- `pool_stats::reserved_bytes()` and aggregated sharded statistics saturate at `std::numeric_limits<std::size_t>::max()` on arithmetic overflow.
- `pool_memory_resource` and `segregated_pool_resource` are non-owning with respect to their upstream resource; the upstream resource must outlive them.
- `segregated_pool_resource` routes requests with alignment greater than `alignof(std::max_align_t)` to the upstream resource.

## Benchmark

The benchmark is intentionally a removable root-level `test.cpp`. Deleting it does not affect the library target or installed package.

Configure and run the benchmark in Release:

```powershell
cmake --preset release
cmake --build --preset release
.\cmake-build-release\memory_pool_benchmark.exe
```

`test.cpp` runs correctness tests first. If any correctness check fails, it writes a report under `results/`, records the failing case and reason, skips benchmark execution, and exits with a non-zero code.

Reports are written to `results/test-YYYY-MM-DD-HH-MM-SS.md`. The report metadata also records the requested display form `YYYY/MM/DD-HH/MM/SS`; the filename uses dashes because `/` is a path separator on Windows.

Non-Release benchmark configuration fails fast. For Debug library-only work, use:

```powershell
cmake --preset debug-library
cmake --build --preset debug-library
```
