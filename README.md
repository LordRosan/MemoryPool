# MemoryPool

MemoryPool 是一个面向固定大小 allocation workload 的 C++20 库。项目按可复用 library 组织，包含明确的 API contracts、可安装的 CMake exports、Release-only validation，以及可重复生成的 benchmark reports。

## 功能范围

- `memory_pool::fixed_block_pool`: thread-safe fixed-block slab allocator，支持 ownership checks、可选 allocation tracking、耗尽限制和 runtime statistics。
- `memory_pool::sharded_fixed_block_pool`: 将 fixed-block allocator 拆分到多个 shard，用于降低并发 workload 下的 lock contention。
- `memory_pool::thread_cached_fixed_block_pool`: 显式 per-thread cache front-end，用于降低 hot path 上的共享锁访问。
- `memory_pool::object_pool<T>`: 基于 fixed-block pool 的 typed construction/destruction helper。
- `memory_pool::pool_allocator<T>`: 基于 `std::pmr::memory_resource` 的 standard allocator adapter。
- `memory_pool::pool_memory_resource`: 面向 PMR-aware containers 的 `std::pmr::memory_resource` adapter。
- `memory_pool::segregated_pool_resource`: 支持多个 small-object size classes 的 PMR resource，并为较大 allocation 提供 upstream fallback。
- `memory_pool::config.hpp`: version constants 和 build-time default policy switches。
- CMake package export: `MemoryPool::memory_pool`，支持 `add_subdirectory` 或安装后 `find_package` 使用。

## 目录结构

```text
include/memory_pool/        Public headers
source/memory_pool/         Non-template implementation
cmake/                      Package config template
documents/                  API contracts、architecture 和 testing notes
tests/package_validation/   Installed-package validation project
test.cpp                    Optional standalone correctness 和 benchmark runner
results/                    Generated benchmark reports
CMakePresets.json           Standard Release 和 Debug-library configure presets
.clang-format               Formatting policy
.editorconfig               Editor defaults
```

## 作为依赖使用

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

对于 mixed small-object PMR workloads：

```cpp
#include <memory_pool/memory_pool.hpp>

memory_pool::segregated_pool_resource resource;
std::pmr::vector<std::pmr::string> values(&resource);
values.emplace_back("cached string");
```

## 设计说明

实现上采用成熟 allocator 的核心 building blocks，而不是把项目做成黑盒 `malloc` replacement：

- Slab allocation 摊薄 system allocation cost。
- Embedded free lists 让固定大小 allocate/deallocate 保持 O(1)。
- Bulk allocate/deallocate APIs 降低 batch-style allocation pattern 中的重复 lock acquisition。
- Sharding 降低 shared mutex contention，同时保留基础算法的可读性。
- 显式 `local_cache` 对象提供 thread-local fast path，并让 lifetime ownership 保持可见。
- Tracking-enabled pools 可以通过 `release_free_slabs()` 把完全空闲的 slabs 归还给系统。
- Size classes 将常见 small allocations 路由到 fixed pools，同时保留 upstream fallback。
- Optional tracking 在 diagnostics 场景下捕获 invalid pointer 和 double free paths。
- Slab ownership 在 growth 过程中由 RAII 管理，allocation failures 不会泄漏 partially-created slabs。

## API 契约

- `fixed_block_pool` internally synchronized；`allocate`、`try_allocate`、`deallocate`、`try_deallocate`、`reserve`、`owns` 和 `stats` 可被多个线程调用。
- `pool_options::enable_tracking` 默认是 `true`，因此 invalid pointer 和 double-free 会被拒绝，而不是破坏 free list。只有在可信 hot path 且 allocator misuse 被视为 caller error 时才建议关闭。
- `deallocate(nullptr)` 和 `try_deallocate(nullptr)` 是 no-op。
- `deallocate` 对不属于 pool 的 pointer 抛出 `std::invalid_argument`；tracking 开启时，double free 也会抛出 `std::invalid_argument`。
- `allocate` 在耗尽或 upstream allocation failure 时抛出 `std::bad_alloc`；`try_allocate` 返回 `nullptr`。
- `try_allocate_bulk()` 是 best-effort，返回实际取得的 block 数量；`allocate_bulk()` 是 all-or-throw，无法完成时会回滚本次调用中已经分配的 blocks。
- `deallocate_bulk()` 在一次 lock 下释放 batch；`try_deallocate_bulk()` 返回被接受的 pointer 数量。
- `thread_cached_fixed_block_pool::local_cache` 设计为单线程拥有。Parent pool 必须长于每个 `local_cache`；每个 cache 析构时会 flush retained blocks。
- `thread_cache_options::validate_ownership_on_deallocate` 可为 diagnostics 启用额外 upstream ownership check；默认 fast path 避免这类 shared-lock check。
- `release_free_slabs()` 只释放没有 active allocations 的 slabs。它要求 `enable_tracking=true`；tracking 关闭时返回 `0`，因为 pool 没有足够 metadata 证明 slab 完全空闲。
- `clear()` 释放所有 slabs，并使此前由 pool 返回的 outstanding pointer 全部失效。
- `allocation_count`、`deallocation_count`、`failed_allocation_count` 等 lifetime counters 不会被 `clear()` 重置。
- `pool_stats::reserved_bytes()` 和 aggregated sharded statistics 在 arithmetic overflow 时饱和到 `std::numeric_limits<std::size_t>::max()`。
- `pool_memory_resource` 和 `segregated_pool_resource` 不拥有 upstream resource；upstream resource 必须长于它们。
- `segregated_pool_options::enable_tracking` 跟随 `pool_options::enable_tracking` 的默认值。
- `pool_allocator<T>` 不拥有其 `std::pmr::memory_resource`；resource 必须长于使用该 allocator 的所有 container。
- `segregated_pool_resource` 会把 alignment 大于 `alignof(std::max_align_t)` 的 requests 路由到 upstream resource。

## Benchmark

Benchmark 有意保留为根目录下可删除的 `test.cpp`。删除它不会影响 library target 或 installed package。

在 Release 下 configure、test 并运行 benchmark：

```powershell
cmake --preset release
cmake --build cmake-build-release
ctest --test-dir cmake-build-release --output-on-failure
.\cmake-build-release\memory_pool_benchmark.exe
```

`test.cpp` 会先运行 correctness tests。若任一 correctness check 失败，它会在 `results/` 下写入报告，记录失败用例和原因，跳过 benchmark execution，并以 non-zero code 退出。

Reports 写入 `results/test-YYYY-MM-DD-HH-MM-SS.md`。Report metadata 也会记录请求展示格式 `YYYY/MM/DD-HH/MM/SS`；文件名使用短横线，因为 `/` 是 path separator。性能表包含 `group`、parameters、`ns/op`、`ops/s`，以及基于同组最低 `ns/op` 的 per-group best comparison。

首次 benchmark 还会初始化 `results/performance-baseline.tsv`。后续 reports 会用当前 `ns/op` 对比该 baseline，记录 `delta ns/op`、`delta %`，并在 baseline comparison table 中解释 `ok`/`watch`/`new` 的判定。

Non-Release benchmark configuration 会 fail fast。Debug library-only 工作使用：

```powershell
cmake --preset debug-library
cmake --build cmake-build-debug
ctest --test-dir cmake-build-debug --output-on-failure
```

补充工程文档：

- [API 契约](documents/api-contracts.md)
- [架构与 invariants](documents/architecture.md)
- [测试与 Benchmark](documents/testing.md)
