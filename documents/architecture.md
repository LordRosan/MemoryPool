# MemoryPool 架构

## 目标

MemoryPool 是一个面向固定大小对象、小对象 PMR workload 和并发 allocator 实验的库。它不是通用 `malloc` 替代品，而是把成熟 allocator 的核心构件拆成可读、可测、可组合的模块。

## 模块结构

- `fixed_block_pool`: slab + embedded free list，是基础分配器。
- `object_pool<T>`: typed construction/destruction facade。
- `pool_memory_resource`: 把单个 `fixed_block_pool` 暴露为 PMR resource。
- `segregated_pool_resource`: 多 size class PMR resource，小对象进入 pool，大对象/over-aligned 转发 upstream。
- `sharded_fixed_block_pool`: 多 shard 降低共享锁竞争。
- `thread_cached_fixed_block_pool`: 显式 `local_cache` 降低 hot path 共享锁访问。

## 核心不变量

### `fixed_block_pool`

- 每个 slab 由 `slab` RAII 对象独占，析构时使用 matching aligned delete。
- `free_list_` 只包含当前未被使用的 block。
- `stats_.used_blocks + stats_.free_blocks == stats_.total_blocks`，除非正在持锁执行中间步骤。
- `enable_tracking=true` 时，`active_blocks_` 精确记录已分配且尚未归还的 block。
- `release_free_slabs()` 只能在 tracking 开启时证明 slab 是否完全空闲。

### PMR resources

- `pool_memory_resource` 不移动或拥有 upstream。
- `segregated_pool_resource` 的 `size_classes_` 与 `pools_` 下标一一对应。
- over-aligned request 不进入 fixed pools，避免返回 alignment 不足的 storage。

### 并发 front-end

- `sharded_fixed_block_pool` 的每个 shard 独立同步，跨 shard 没有全局锁。
- `thread_cached_fixed_block_pool::local_cache` 是显式局部缓存；它不是 TLS singleton，生命周期由调用方控制。

## 失败路径设计

- 参数错误使用 `std::invalid_argument` 或 `std::overflow_error`。
- 资源耗尽使用 `std::bad_alloc`。
- `try_*` API 不抛出，把失败转换为 `nullptr`、`false` 或实际完成数量。
- bulk allocation 采用 all-or-throw 语义；失败会回滚本次已分配 block。

## 复杂度

| 操作 | 复杂度 | 说明 |
| --- | --- | --- |
| `fixed_block_pool::allocate` | O(1) amortized | 需要增长 slab 时包含系统 allocation |
| `fixed_block_pool::deallocate` | O(slab_count) with ownership check | tracking 开启时再查 active set |
| `release_free_slabs` | O(slab_count * active_blocks) | 稳健性路径，不是 hot path |
| `sharded_fixed_block_pool::allocate` | O(shard_count) worst-case | 从本线程 shard 开始探测 |
| `local_cache::allocate` | O(1) amortized | cache miss 时批量 refill |
