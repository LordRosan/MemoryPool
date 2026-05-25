#ifndef NDEBUG
#error "MemoryPool benchmark must be built in Release mode."
#endif

#include "memory_pool/memory_pool.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory_resource>
#include <new>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#ifndef MEMORY_POOL_RESULTS_DIR
#define MEMORY_POOL_RESULTS_DIR "results"
#endif

namespace {

using clock_type = std::chrono::steady_clock;

struct benchmark_result {
    std::string name;
    std::size_t operations = 0;
    double total_ms = 0.0;
    std::string notes;

    [[nodiscard]] double ns_per_op() const noexcept {
        return operations == 0 ? 0.0 : (total_ms * 1'000'000.0) / static_cast<double>(operations);
    }
};

struct correctness_check {
    std::string name;
    bool passed = false;
    std::string detail;
};

template <typename T>
void do_not_optimize(const T& value) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "m"(value) : "memory");
#elif defined(_MSC_VER)
    _ReadWriteBarrier();
    (void)value;
#else
    const volatile auto* sink = &value;
    (void)sink;
#endif
}

void clobber_memory() {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : : "memory");
#elif defined(_MSC_VER)
    _ReadWriteBarrier();
#endif
}

template <typename Warmup, typename Work>
benchmark_result run_case(std::string name, std::size_t operations, Warmup warmup, Work work, std::string notes = {}) {
    warmup();
    clobber_memory();
    const auto start = clock_type::now();
    work();
    clobber_memory();
    const auto finish = clock_type::now();
    const std::chrono::duration<double, std::milli> elapsed = finish - start;
    return {std::move(name), operations, elapsed.count(), std::move(notes)};
}

std::string timestamp_for_file() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t raw = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &raw);
#else
    localtime_r(&raw, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%d-%H-%M-%S");
    return out.str();
}

std::string timestamp_pretty() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t raw = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &raw);
#else
    localtime_r(&raw, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y/%m/%d-%H/%M/%S");
    return out.str();
}

std::string bar(double value, double best) {
    if (value <= 0.0 || best <= 0.0) {
        return {};
    }
    const auto width = static_cast<std::size_t>(std::clamp(best / value, 0.05, 1.0) * 32.0);
    return std::string(width, '#');
}

struct payload {
    std::uint64_t a = 0;
    std::uint64_t b = 0;
    std::uint64_t c = 0;
    std::uint64_t d = 0;
};

void require_correctness(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Func>
correctness_check run_correctness_check(std::string name, Func&& func) {
    try {
        func();
        return {std::move(name), true, "通过"};
    } catch (const std::exception& error) {
        return {std::move(name), false, error.what()};
    } catch (...) {
        return {std::move(name), false, "捕获到未知异常"};
    }
}

struct throwing_object {
    explicit throwing_object(int) {
        throw std::runtime_error("construction failed");
    }
};

struct over_aligned_payload {
    alignas(64) std::byte bytes[64]{};
};

class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t allocations = 0;
    std::size_t deallocations = 0;

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocations;
        return ::operator new(bytes, std::align_val_t(alignment));
    }

    void do_deallocate(void* pointer, std::size_t, std::size_t alignment) override {
        ++deallocations;
        ::operator delete(pointer, std::align_val_t(alignment));
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

std::vector<correctness_check> run_correctness_tests() {
    std::vector<correctness_check> checks;

    checks.push_back(run_correctness_check("fixed_block_pool default double free", [] {
        memory_pool::pool_options options;
        options.block_size = sizeof(int);
        options.block_alignment = alignof(int);
        options.blocks_per_slab = 2;
        options.max_blocks = 2;

        memory_pool::fixed_block_pool pool(options);
        void* pointer = pool.allocate();
        pool.deallocate(pointer);

        require_correctness(!pool.try_deallocate(pointer), "默认 fixed_block_pool 必须拒绝 double free");
        const auto stats = pool.stats();
        require_correctness(stats.used_blocks == 0, "double free 不应导致 used_blocks 下溢");
        require_correctness(stats.free_blocks == 2, "double free 不应向 free list 重复插入 block");
    }));

    checks.push_back(run_correctness_check("fixed_block_pool exhaustion", [] {
        memory_pool::pool_options options;
        options.block_size = sizeof(int);
        options.block_alignment = alignof(int);
        options.blocks_per_slab = 1;
        options.max_blocks = 1;

        memory_pool::fixed_block_pool pool(options);
        void* pointer = pool.allocate();
        require_correctness(pool.try_allocate() == nullptr, "try_allocate 在耗尽时必须返回 nullptr");
        pool.deallocate(pointer);
    }));

    checks.push_back(run_correctness_check("fixed_block_pool bulk allocation", [] {
        memory_pool::pool_options options;
        options.block_size = sizeof(payload);
        options.block_alignment = alignof(payload);
        options.blocks_per_slab = 2;
        options.max_blocks = 3;

        memory_pool::fixed_block_pool pool(options);
        std::vector<void*> pointers(5);
        const std::size_t allocated = pool.try_allocate_bulk(pointers.data(), pointers.size());
        require_correctness(allocated == 3, "try_allocate_bulk 必须返回实际分配数量");
        require_correctness(pointers[3] == nullptr && pointers[4] == nullptr, "try_allocate_bulk 失败尾部必须写入 nullptr");
        require_correctness(pool.stats().used_blocks == 3, "bulk allocation 必须更新 used_blocks");

        pool.deallocate_bulk(pointers.data(), allocated);
        require_correctness(pool.stats().used_blocks == 0, "deallocate_bulk 必须归还所有 block");
    }));

    checks.push_back(run_correctness_check("thread_cached_fixed_block_pool local cache", [] {
        memory_pool::pool_options pool_options;
        pool_options.block_size = sizeof(payload);
        pool_options.block_alignment = alignof(payload);
        pool_options.blocks_per_slab = 8;
        pool_options.max_blocks = 16;

        memory_pool::thread_cache_options cache_options;
        cache_options.max_cached_blocks = 4;
        cache_options.refill_count = 4;
        cache_options.release_count = 2;

        memory_pool::thread_cached_fixed_block_pool pool(pool_options, cache_options);
        {
            auto cache = pool.make_cache();
            void* pointer = cache.allocate();
            cache.deallocate(pointer);
            require_correctness(cache.cached_blocks() == 4, "local_cache 应批量 refill 并缓存归还的 block");
            require_correctness(pool.stats().used_blocks == 4, "local_cache refill 后 cached blocks 仍应计为 upstream used");
            require_correctness(!cache.try_deallocate(pointer), "local_cache 必须拒绝同一线程 cache 内的 duplicate pointer");
        }
        require_correctness(pool.stats().used_blocks == 0, "local_cache 析构时必须 flush cached blocks");
    }));

    checks.push_back(run_correctness_check("object_pool constructor rollback", [] {
        memory_pool::object_pool<throwing_object> pool(2);
        bool threw = false;
        try {
            (void)pool.create(1);
        } catch (const std::runtime_error&) {
            threw = true;
        }

        require_correctness(threw, "object_pool 必须传播构造函数异常");
        require_correctness(pool.stats().used_blocks == 0, "object_pool 在构造失败后必须归还 storage");
    }));

    checks.push_back(run_correctness_check("sharded_fixed_block_pool exact reserve", [] {
        memory_pool::pool_options options;
        options.block_size = sizeof(int);
        options.block_alignment = alignof(int);
        options.blocks_per_slab = 1;
        options.max_blocks = 5;

        memory_pool::sharded_fixed_block_pool pool(options, 4);
        pool.reserve(5);
        require_correctness(pool.stats().total_blocks == 5, "sharded_fixed_block_pool reserve 必须遵守分片后的 max_blocks");

        std::vector<void*> pointers;
        for (int i = 0; i < 5; ++i) {
            pointers.push_back(pool.allocate());
        }
        require_correctness(pool.try_allocate() == nullptr, "sharded_fixed_block_pool 超过 max_blocks 后必须拒绝分配");
        for (void* pointer : pointers) {
            pool.deallocate(pointer);
        }
    }));

    checks.push_back(run_correctness_check("segregated_pool_resource small size classes", [] {
        memory_pool::segregated_pool_options options;
        options.size_classes = {16, 32, 64};
        options.blocks_per_slab = 4;

        memory_pool::segregated_pool_resource resource(options);
        std::pmr::vector<std::pmr::string> values(&resource);
        values.emplace_back("short");
        values.emplace_back(24, 'x');

        const auto stats = resource.stats();
        bool used_pool = false;
        for (const auto& entry : stats) {
            used_pool = used_pool || entry.stats.allocation_count > 0;
        }
        require_correctness(used_pool, "segregated_pool_resource 必须把小型 PMR allocation 路由到 size class");
    }));

    checks.push_back(run_correctness_check("segregated_pool_resource over-aligned fallback", [] {
        counting_resource upstream;
        memory_pool::segregated_pool_options options;
        options.size_classes = {64, 128};
        options.blocks_per_slab = 4;
        options.upstream = &upstream;

        memory_pool::segregated_pool_resource resource(options);
        void* pointer = resource.allocate(sizeof(over_aligned_payload), alignof(over_aligned_payload));
        resource.deallocate(pointer, sizeof(over_aligned_payload), alignof(over_aligned_payload));

        require_correctness(upstream.allocations == 1, "over-aligned allocation 必须转发到 upstream resource");
        require_correctness(upstream.deallocations == 1, "over-aligned deallocation 必须转发到 upstream resource");
        for (const auto& entry : resource.stats()) {
            require_correctness(entry.stats.allocation_count == 0, "over-aligned allocation 不应污染 size class stats");
        }
    }));

    checks.push_back(run_correctness_check("pool_stats reserved bytes saturation", [] {
        memory_pool::pool_stats stats;
        stats.total_blocks = std::numeric_limits<std::size_t>::max();
        stats.block_stride = 2;

        require_correctness(
            stats.reserved_bytes() == std::numeric_limits<std::size_t>::max(),
            "pool_stats::reserved_bytes 溢出时必须返回饱和值");
    }));

    return checks;
}

bool correctness_passed(const std::vector<correctness_check>& checks) {
    return std::all_of(checks.begin(), checks.end(), [](const correctness_check& check) {
        return check.passed;
    });
}

std::vector<std::size_t> shuffled_order(std::size_t count) {
    std::vector<std::size_t> order(count);
    std::iota(order.begin(), order.end(), 0);
    std::mt19937_64 rng(0xC0FFEE);
    std::shuffle(order.begin(), order.end(), rng);
    return order;
}

benchmark_result raw_new_delete_case(const std::vector<std::size_t>& order) {
    constexpr std::size_t rounds = 256;
    const std::size_t batch = order.size();
    std::vector<void*> pointers(batch);
    auto body = [&] {
        for (std::size_t round = 0; round < rounds; ++round) {
            for (std::size_t i = 0; i < batch; ++i) {
                pointers[i] = ::operator new(sizeof(payload), std::align_val_t(alignof(payload)));
                do_not_optimize(pointers[i]);
            }
            for (std::size_t index : order) {
                ::operator delete(pointers[index], std::align_val_t(alignof(payload)));
            }
        }
    };
    return run_case("raw operator new/delete 32B", rounds * batch * 2, [] {}, body, "baseline allocator 对照组");
}

benchmark_result fixed_pool_case(const std::vector<std::size_t>& order) {
    constexpr std::size_t rounds = 256;
    const std::size_t batch = order.size();
    memory_pool::pool_options options;
    options.block_size = sizeof(payload);
    options.block_alignment = alignof(payload);
    options.blocks_per_slab = batch;
    options.enable_tracking = false;
    memory_pool::fixed_block_pool pool(options);
    std::vector<void*> pointers(batch);
    auto warmup = [&] {
        void* p = pool.allocate();
        pool.deallocate(p);
    };
    auto body = [&] {
        for (std::size_t round = 0; round < rounds; ++round) {
            for (std::size_t i = 0; i < batch; ++i) {
                pointers[i] = pool.allocate();
                do_not_optimize(pointers[i]);
            }
            for (std::size_t index : order) {
                pool.deallocate(pointers[index]);
            }
        }
    };
    return run_case("fixed_block_pool allocate/deallocate", rounds * batch * 2, warmup, body, "单线程复用 slab，production fast mode");
}

benchmark_result pmr_pool_case(const std::vector<std::size_t>& order) {
    constexpr std::size_t rounds = 256;
    const std::size_t batch = order.size();
    std::pmr::unsynchronized_pool_resource resource;
    std::vector<void*> pointers(batch);
    auto body = [&] {
        for (std::size_t round = 0; round < rounds; ++round) {
            for (std::size_t i = 0; i < batch; ++i) {
                pointers[i] = resource.allocate(sizeof(payload), alignof(payload));
                do_not_optimize(pointers[i]);
            }
            for (std::size_t index : order) {
                resource.deallocate(pointers[index], sizeof(payload), alignof(payload));
            }
        }
    };
    return run_case("std::pmr::unsynchronized_pool_resource", rounds * batch * 2, [] {}, body, "standard library 对照组");
}

benchmark_result object_pool_case(const std::vector<std::size_t>& order) {
    constexpr std::size_t rounds = 192;
    const std::size_t batch = order.size();
    memory_pool::pool_options options;
    options.blocks_per_slab = batch;
    options.enable_tracking = false;
    memory_pool::object_pool<payload> pool(options);
    std::vector<payload*> pointers(batch);
    auto body = [&] {
        for (std::size_t round = 0; round < rounds; ++round) {
            for (std::size_t i = 0; i < batch; ++i) {
                pointers[i] = pool.create(payload{round, i, round ^ i, i + 1});
                do_not_optimize(pointers[i]);
            }
            for (std::size_t index : order) {
                pool.destroy(pointers[index]);
            }
        }
    };
    return run_case("object_pool create/destroy", rounds * batch * 2, [] {}, body, "构造真实对象，production fast mode");
}

benchmark_result shared_pool_threaded_case() {
    const std::size_t threads = std::max(2u, std::min(8u, std::thread::hardware_concurrency()));
    constexpr std::size_t iterations = 120'000;
    memory_pool::pool_options options;
    options.block_size = sizeof(payload);
    options.block_alignment = alignof(payload);
    options.blocks_per_slab = 4096;
    options.enable_tracking = false;
    memory_pool::fixed_block_pool pool(options);
    std::atomic<std::size_t> ready = 0;
    std::atomic<bool> start = false;
    auto worker = [&] {
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (std::size_t i = 0; i < iterations; ++i) {
            void* p = pool.allocate();
            do_not_optimize(p);
            pool.deallocate(p);
        }
    };
    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) {
        workers.emplace_back(worker);
    }
    while (ready.load(std::memory_order_acquire) != threads) {
        std::this_thread::yield();
    }
    const auto begin = clock_type::now();
    start.store(true, std::memory_order_release);
    for (auto& thread : workers) {
        thread.join();
    }
    const auto end = clock_type::now();
    const std::chrono::duration<double, std::milli> elapsed = end - begin;
    return {"fixed_block_pool shared concurrency", threads * iterations * 2, elapsed.count(), std::to_string(threads) + " threads"};
}

benchmark_result sharded_pool_threaded_case() {
    const std::size_t threads = std::max(2u, std::min(8u, std::thread::hardware_concurrency()));
    constexpr std::size_t iterations = 120'000;
    memory_pool::pool_options options;
    options.block_size = sizeof(payload);
    options.block_alignment = alignof(payload);
    options.blocks_per_slab = 4096;
    options.enable_tracking = false;
    memory_pool::sharded_fixed_block_pool pool(options, threads);
    std::atomic<std::size_t> ready = 0;
    std::atomic<bool> start = false;
    auto worker = [&] {
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (std::size_t i = 0; i < iterations; ++i) {
            void* p = pool.allocate();
            do_not_optimize(p);
            pool.deallocate(p);
        }
    };
    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) {
        workers.emplace_back(worker);
    }
    while (ready.load(std::memory_order_acquire) != threads) {
        std::this_thread::yield();
    }
    const auto begin = clock_type::now();
    start.store(true, std::memory_order_release);
    for (auto& thread : workers) {
        thread.join();
    }
    const auto end = clock_type::now();
    const std::chrono::duration<double, std::milli> elapsed = end - begin;
    return {"sharded_fixed_block_pool concurrency", threads * iterations * 2, elapsed.count(), std::to_string(threads) + " shards"};
}

benchmark_result thread_cached_pool_threaded_case() {
    const std::size_t threads = std::max(2u, std::min(8u, std::thread::hardware_concurrency()));
    constexpr std::size_t iterations = 120'000;
    memory_pool::pool_options options;
    options.block_size = sizeof(payload);
    options.block_alignment = alignof(payload);
    options.blocks_per_slab = 4096;
    options.enable_tracking = false;

    memory_pool::thread_cache_options cache_options;
    cache_options.max_cached_blocks = 256;
    cache_options.refill_count = 64;
    cache_options.release_count = 64;
    memory_pool::thread_cached_fixed_block_pool pool(options, cache_options);

    std::atomic<std::size_t> ready = 0;
    std::atomic<bool> start = false;
    auto worker = [&] {
        auto cache = pool.make_cache();
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (std::size_t i = 0; i < iterations; ++i) {
            void* p = cache.allocate();
            do_not_optimize(p);
            cache.deallocate(p);
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) {
        workers.emplace_back(worker);
    }
    while (ready.load(std::memory_order_acquire) != threads) {
        std::this_thread::yield();
    }
    const auto begin = clock_type::now();
    start.store(true, std::memory_order_release);
    for (auto& thread : workers) {
        thread.join();
    }
    const auto end = clock_type::now();
    const std::chrono::duration<double, std::milli> elapsed = end - begin;
    return {
        "thread_cached_fixed_block_pool concurrency",
        threads * iterations * 2,
        elapsed.count(),
        std::to_string(threads) + " local caches"};
}

benchmark_result raw_new_threaded_case() {
    const std::size_t threads = std::max(2u, std::min(8u, std::thread::hardware_concurrency()));
    constexpr std::size_t iterations = 120'000;
    std::atomic<std::size_t> ready = 0;
    std::atomic<bool> start = false;
    auto worker = [&] {
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (std::size_t i = 0; i < iterations; ++i) {
            void* p = ::operator new(sizeof(payload), std::align_val_t(alignof(payload)));
            do_not_optimize(p);
            ::operator delete(p, std::align_val_t(alignof(payload)));
        }
    };
    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) {
        workers.emplace_back(worker);
    }
    while (ready.load(std::memory_order_acquire) != threads) {
        std::this_thread::yield();
    }
    const auto begin = clock_type::now();
    start.store(true, std::memory_order_release);
    for (auto& thread : workers) {
        thread.join();
    }
    const auto end = clock_type::now();
    const std::chrono::duration<double, std::milli> elapsed = end - begin;
    return {"raw new/delete shared concurrency", threads * iterations * 2, elapsed.count(), std::to_string(threads) + " threads"};
}

benchmark_result segregated_pmr_case() {
    constexpr std::size_t rounds = 128;
    constexpr std::size_t batch = 4096;
    memory_pool::segregated_pool_options options;
    options.blocks_per_slab = 1024;
    memory_pool::segregated_pool_resource resource(options);
    std::pmr::vector<std::pmr::string> values(&resource);
    std::vector<std::size_t> lengths(batch);
    std::mt19937 rng(0xBADC0DE);
    std::uniform_int_distribution<int> length_dist(8, 160);
    for (std::size_t i = 0; i < batch; ++i) {
        lengths[i] = static_cast<std::size_t>(length_dist(rng));
    }

    auto warmup = [&] {
        values.clear();
        values.emplace_back("warmup");
        values.clear();
    };
    auto body = [&] {
        std::size_t checksum = 0;
        for (std::size_t round = 0; round < rounds; ++round) {
            values.clear();
            for (std::size_t i = 0; i < batch; ++i) {
                values.emplace_back(lengths[i], static_cast<char>('a' + (i % 26)));
                checksum += values.back().size();
            }
        }
        do_not_optimize(checksum);
    };
    return run_case("segregated_pool_resource pmr strings", rounds * batch, warmup, body, "8-160 byte PMR allocation");
}

void write_report(
    const std::vector<correctness_check>& checks,
    const std::vector<benchmark_result>& results,
    const memory_pool::pool_stats* stats) {
    std::filesystem::path results_dir = MEMORY_POOL_RESULTS_DIR;
    std::filesystem::create_directories(results_dir);
    const auto file_stamp = timestamp_for_file();
    const auto report_path = results_dir / ("test-" + file_stamp + ".md");

    const bool passed = correctness_passed(checks);
    double best = results.empty() ? 0.0 : results.front().ns_per_op();
    for (const auto& result : results) {
        if (result.ns_per_op() > 0.0) {
            best = std::min(best, result.ns_per_op());
        }
    }

    std::ofstream report(report_path);
    report << "# MemoryPool 正确性与性能测试报告\n\n";
    report << "## 元数据\n\n";
    report << "- Library: MemoryPool " << memory_pool::version() << '\n';
    report << "- Build: Release (`NDEBUG` defined)\n";
    report << "- Timestamp: " << timestamp_pretty() << '\n';
    report << "- Random seed: 0xC0FFEE / 0xBADC0DE\n";
    report << "- Result file: " << report_path.generic_string() << "\n\n";

    report << "## 正确性测试\n\n";
    report << "| 用例 | 状态 | 详情 |\n";
    report << "| --- | --- | --- |\n";
    for (const auto& check : checks) {
        report << "| " << check.name
               << " | " << (check.passed ? "pass" : "fail")
               << " | " << check.detail << " |\n";
    }

    if (!passed) {
        report << "\n## 性能测试结果\n\n";
        report << "- 状态: skipped\n";
        report << "- 原因: correctness tests 未全部通过；为避免产生无效性能数据，已终止后续 benchmark。\n";
        std::cout << "Wrote " << report_path << '\n';
        return;
    }

    report << "\n## 性能测试结果\n\n";
    report << "| 用例 | 操作次数 | 总耗时 ms | ns/op | 图示 | 备注 |\n";
    report << "| --- | ---: | ---: | ---: | --- | --- |\n";
    for (const auto& result : results) {
        report << "| " << result.name
               << " | " << result.operations
               << " | " << std::fixed << std::setprecision(3) << result.total_ms
               << " | " << std::fixed << std::setprecision(2) << result.ns_per_op()
               << " | `" << bar(result.ns_per_op(), best) << "`"
               << " | " << result.notes << " |\n";
    }

    if (stats != nullptr) {
        report << "\n## 资源快照\n\n";
        report << "- Slabs: " << stats->slab_count << '\n';
        report << "- Block size: " << stats->block_size << " bytes\n";
        report << "- Block stride: " << stats->block_stride << " bytes\n";
        report << "- Total blocks: " << stats->total_blocks << '\n';
        report << "- Peak used blocks: " << stats->peak_used_blocks << '\n';
        report << "- Reserved bytes: " << stats->reserved_bytes() << "\n\n";
    }

    report << "## 测试方法\n\n";
    report << "- 正确性测试在 benchmark 前执行；任何失败都会终止 benchmark。\n";
    report << "- Warmup 在计时区间前执行，避免冷启动状态主导结果。\n";
    report << "- 随机释放顺序和 PMR string 长度在计时前生成，不计入 allocator 热路径。\n";
    report << "- bulk allocation 和 thread-local cache paths 用于覆盖更接近生产 allocator 的热路径。\n";
    report << "- `do_not_optimize` 和 compiler memory barrier 用于降低编译器过度优化风险。\n";
    report << "- 对照组包含 raw aligned new/delete 和 `std::pmr::unsynchronized_pool_resource`。\n";

    std::cout << "Wrote " << report_path << '\n';
}

} // namespace

int main() {
    const auto correctness = run_correctness_tests();
    if (!correctness_passed(correctness)) {
        write_report(correctness, {}, nullptr);
        return 1;
    }

    const auto order = shuffled_order(4096);
    std::vector<benchmark_result> results;
    results.push_back(raw_new_delete_case(order));
    results.push_back(fixed_pool_case(order));
    results.push_back(pmr_pool_case(order));
    results.push_back(object_pool_case(order));
    results.push_back(raw_new_threaded_case());
    results.push_back(shared_pool_threaded_case());
    results.push_back(thread_cached_pool_threaded_case());
    results.push_back(sharded_pool_threaded_case());
    results.push_back(segregated_pmr_case());

    memory_pool::pool_options options;
    options.block_size = sizeof(payload);
    options.block_alignment = alignof(payload);
    options.blocks_per_slab = 4096;
    options.enable_tracking = false;
    memory_pool::fixed_block_pool stats_pool(options);
    stats_pool.reserve(8192);
    std::vector<void*> stats_sample;
    stats_sample.reserve(4096);
    for (std::size_t i = 0; i < 4096; ++i) {
        stats_sample.push_back(stats_pool.allocate());
    }
    for (void* pointer : stats_sample) {
        stats_pool.deallocate(pointer);
    }
    auto stats = stats_pool.stats();

    write_report(correctness, results, &stats);
}
