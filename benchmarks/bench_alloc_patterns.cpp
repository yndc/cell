#include <benchmark/benchmark.h>
#include <cell/context.h>

#include <cstdlib>
#include <random>
#include <vector>

// =============================================================================
// Small Allocations (TLS Cache Hot Path: 16B - 128B)
// =============================================================================

static void BM_Cell_Small_16B(benchmark::State &state) {
    Cell::Context ctx;
    for (auto _ : state) {
        void *ptr = ctx.alloc_bytes(16);
        benchmark::DoNotOptimize(ptr);
        ctx.free_bytes(ptr);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Cell_Small_16B);

static void BM_Cell_Small_64B(benchmark::State &state) {
    Cell::Context ctx;
    for (auto _ : state) {
        void *ptr = ctx.alloc_bytes(64);
        benchmark::DoNotOptimize(ptr);
        ctx.free_bytes(ptr);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Cell_Small_64B);

static void BM_Cell_Small_128B(benchmark::State &state) {
    Cell::Context ctx;
    for (auto _ : state) {
        void *ptr = ctx.alloc_bytes(128);
        benchmark::DoNotOptimize(ptr);
        ctx.free_bytes(ptr);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Cell_Small_128B);

// =============================================================================
// Medium Allocations (Sub-Cell Bins: 256B - 8KB)
// =============================================================================

static void BM_Cell_Medium_512B(benchmark::State &state) {
    Cell::Context ctx;
    for (auto _ : state) {
        void *ptr = ctx.alloc_bytes(512);
        benchmark::DoNotOptimize(ptr);
        ctx.free_bytes(ptr);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Cell_Medium_512B);

static void BM_Cell_Medium_1KB(benchmark::State &state) {
    Cell::Context ctx;
    for (auto _ : state) {
        void *ptr = ctx.alloc_bytes(1024);
        benchmark::DoNotOptimize(ptr);
        ctx.free_bytes(ptr);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Cell_Medium_1KB);

static void BM_Cell_Medium_4KB(benchmark::State &state) {
    Cell::Context ctx;
    for (auto _ : state) {
        void *ptr = ctx.alloc_bytes(4096);
        benchmark::DoNotOptimize(ptr);
        ctx.free_bytes(ptr);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Cell_Medium_4KB);

static void BM_Cell_Medium_16KB(benchmark::State &state) {
    Cell::Context ctx;
    for (auto _ : state) {
        void *ptr = ctx.alloc_bytes(16 * 1024);
        benchmark::DoNotOptimize(ptr);
        ctx.free_bytes(ptr);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Cell_Medium_16KB);

// =============================================================================
// Buddy Allocations (32KB - 2MB)
// =============================================================================

static void BM_Cell_Buddy_64KB(benchmark::State &state) {
    Cell::Context ctx;
    for (auto _ : state) {
        void *ptr = ctx.alloc_bytes(64 * 1024);
        benchmark::DoNotOptimize(ptr);
        ctx.free_bytes(ptr);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Cell_Buddy_64KB);

static void BM_Cell_Buddy_256KB(benchmark::State &state) {
    Cell::Context ctx;
    for (auto _ : state) {
        void *ptr = ctx.alloc_bytes(256 * 1024);
        benchmark::DoNotOptimize(ptr);
        ctx.free_bytes(ptr);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Cell_Buddy_256KB);

static void BM_Cell_Buddy_1MB(benchmark::State &state) {
    Cell::Context ctx;
    for (auto _ : state) {
        void *ptr = ctx.alloc_bytes(1024 * 1024);
        benchmark::DoNotOptimize(ptr);
        ctx.free_bytes(ptr);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Cell_Buddy_1MB);

// =============================================================================
// Large Allocations (>2MB, Direct OS)
// =============================================================================

static void BM_Cell_Large_4MB(benchmark::State &state) {
    Cell::Context ctx;
    for (auto _ : state) {
        void *ptr = ctx.alloc_large(4 * 1024 * 1024);
        benchmark::DoNotOptimize(ptr);
        ctx.free_large(ptr);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Cell_Large_4MB);

// =============================================================================
// Batch Allocation Patterns
// =============================================================================

static void BM_Cell_BatchAlloc_64B(benchmark::State &state) {
    Cell::Context ctx;
    const size_t batch_size = 1000;
    std::vector<void *> ptrs(batch_size);

    for (auto _ : state) {
        // Allocate batch
        for (size_t i = 0; i < batch_size; ++i) {
            ptrs[i] = ctx.alloc_bytes(64);
        }
        benchmark::DoNotOptimize(ptrs.data());

        // Free batch
        for (size_t i = 0; i < batch_size; ++i) {
            ctx.free_bytes(ptrs[i]);
        }
    }
    state.SetItemsProcessed(state.iterations() * batch_size);
}
BENCHMARK(BM_Cell_BatchAlloc_64B);

// SIMD-optimized batch allocation using alloc_batch/free_batch API
static void BM_Cell_BatchAPI_64B(benchmark::State &state) {
    Cell::Context ctx;
    const size_t batch_size = 1000;
    std::vector<void *> ptrs(batch_size);

    for (auto _ : state) {
        size_t allocated = ctx.alloc_batch(64, ptrs.data(), batch_size);
        benchmark::DoNotOptimize(ptrs.data());
        benchmark::DoNotOptimize(allocated);

        ctx.free_batch(ptrs.data(), allocated);
    }
    state.SetItemsProcessed(state.iterations() * batch_size);
}
BENCHMARK(BM_Cell_BatchAPI_64B);

static void BM_Cell_BatchAPI_512B(benchmark::State &state) {
    Cell::Context ctx;
    const size_t batch_size = 1000;
    std::vector<void *> ptrs(batch_size);

    for (auto _ : state) {
        size_t allocated = ctx.alloc_batch(512, ptrs.data(), batch_size);
        benchmark::DoNotOptimize(ptrs.data());
        benchmark::DoNotOptimize(allocated);

        ctx.free_batch(ptrs.data(), allocated);
    }
    state.SetItemsProcessed(state.iterations() * batch_size);
}
BENCHMARK(BM_Cell_BatchAPI_512B);

static void BM_Cell_BatchAlloc_1KB(benchmark::State &state) {
    Cell::Context ctx;
    const size_t batch_size = 1000;
    std::vector<void *> ptrs(batch_size);

    for (auto _ : state) {
        for (size_t i = 0; i < batch_size; ++i) {
            ptrs[i] = ctx.alloc_bytes(1024);
        }
        benchmark::DoNotOptimize(ptrs.data());

        for (size_t i = 0; i < batch_size; ++i) {
            ctx.free_bytes(ptrs[i]);
        }
    }
    state.SetItemsProcessed(state.iterations() * batch_size);
}
BENCHMARK(BM_Cell_BatchAlloc_1KB);

// =============================================================================
// Mixed Size Patterns (Realistic Workload)
// =============================================================================

static void BM_Cell_MixedSizes(benchmark::State &state) {
    Cell::Context ctx;
    std::mt19937 rng(42);
    std::uniform_int_distribution<size_t> size_dist(16, 4096);

    const size_t batch_size = 100;
    std::vector<void *> ptrs(batch_size);
    std::vector<size_t> sizes(batch_size);

    for (auto _ : state) {
        state.PauseTiming();
        for (size_t i = 0; i < batch_size; ++i) {
            sizes[i] = size_dist(rng);
        }
        state.ResumeTiming();

        for (size_t i = 0; i < batch_size; ++i) {
            ptrs[i] = ctx.alloc_bytes(sizes[i]);
        }
        benchmark::DoNotOptimize(ptrs.data());

        for (size_t i = 0; i < batch_size; ++i) {
            ctx.free_bytes(ptrs[i]);
        }
    }
    state.SetItemsProcessed(state.iterations() * batch_size);
}
BENCHMARK(BM_Cell_MixedSizes);

// =============================================================================
// Realloc Pattern (Vector-like growth)
// =============================================================================

static void BM_Cell_Realloc_Growth(benchmark::State &state) {
    Cell::Context ctx;

    for (auto _ : state) {
        void *ptr = ctx.alloc_bytes(16);
        for (size_t size = 32; size <= 4096; size *= 2) {
            ptr = ctx.realloc_bytes(ptr, size);
            benchmark::DoNotOptimize(ptr);
        }
        ctx.free_bytes(ptr);
    }
    state.SetItemsProcessed(state.iterations() * 8); // 8 reallocs per iteration
}
BENCHMARK(BM_Cell_Realloc_Growth);
