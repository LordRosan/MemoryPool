# MemoryPool API 契约

本文档记录公共 API 的行为契约。README 面向快速使用；这里面向维护者和库使用方判断边界、异常和并发语义。

## 通用约定

- 所有公共类型位于 `memory_pool` namespace。
- 库不接管 `std::pmr::memory_resource* upstream` 的所有权；调用方必须保证 upstream 生命周期长于依赖它的 resource。
- `allocate()` 系列在无法满足请求时抛出 `std::bad_alloc` 或具体参数异常；`try_allocate()` 系列把失败转换为 `nullptr` 或实际完成数量。
- `deallocate(nullptr)` 与 `try_deallocate(nullptr)` 是 no-op。
- `pool_stats` 里的累计计数使用 saturating arithmetic；发生溢出时保持 `std::numeric_limits<std::size_t>::max()`。

## `fixed_block_pool`

- Thread safety: `allocate`、`try_allocate`、`deallocate`、`try_deallocate`、`reserve`、`release_free_slabs`、`clear`、`owns`、`stats` 内部同步，可被多个线程调用。
- Complexity: hot-path `allocate` / `deallocate` 为 O(1)；`owns` 为 O(slab_count)；`release_free_slabs` 为 O(slab_count * active_blocks)`。
- `pool_options::blocks_per_slab` 必须大于 0。
- `pool_options::block_alignment` 必须是 power-of-two；内部会提升到至少 `alignof(free_node)`。
- `pool_options::max_blocks == 0` 表示不设置逻辑上限。
- 默认 `enable_tracking=true`。这会拒绝 invalid pointer 和 double free，适合默认稳健配置；可信 hot path 可显式关闭。
- `clear()` 释放所有 slab，并使先前返回的所有 pointer 失效。
- `release_free_slabs()` 只释放没有 active block 的 slab；`enable_tracking=false` 时返回 0。

## `object_pool<T>`

- `create(args...)` 先从 `fixed_block_pool` 取得 storage，再调用 `std::construct_at`。
- 如果构造函数抛异常，storage 会归还给 pool，然后异常继续传播。
- `destroy(nullptr)` 是 no-op。
- 调用方必须只把由同一个 `object_pool<T>` 创建且仍存活的对象交给 `destroy()`。

## `pool_memory_resource`

- 小于等于 `pool.block_size()` 且 alignment 不超过 `pool.block_alignment()` 的请求进入 pool。
- 其他请求转发到 upstream。
- `do_deallocate` 会先通过 `pool.owns(pointer)` 判定是否归还 pool，否则转发 upstream。

## `segregated_pool_resource`

- `size_classes` 会排序并去重，且每个 size class 必须大于 0。
- alignment 大于 `alignof(std::max_align_t)` 的请求转发到 upstream。
- 默认 `enable_tracking` 跟随 `pool_options` 的默认稳健策略。
- 调用方必须用与 allocation 对应的 `bytes` 和 `alignment` 调用 deallocate；这是 PMR contract 的一部分。

## `sharded_fixed_block_pool`

- `max_blocks` 会被分配到各 shard，整体容量不应超过调用方指定的总上限。
- `allocate()` 从当前线程 hash 对应 shard 开始尝试，失败后探测其他 shard。
- `deallocate()` 会探测所有 shard 来找到 pointer owner；这使跨线程归还可行，但成本是 O(shard_count * owns_cost)。

## `thread_cached_fixed_block_pool`

- `local_cache` 设计为单线程拥有，不提供内部同步。
- parent pool 必须长于所有 `local_cache`。
- `local_cache` 析构时会把 retained blocks flush 回 upstream。
- `validate_ownership_on_deallocate=false` 是默认 fast path；调试误用时可以开启。
