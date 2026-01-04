#include <benchmark/benchmark.h>
#include <cell/arena.h>
#include <cell/context.h>
#include <cell/pool.h>

#include <algorithm>
#include <cstdlib>
#include <numeric>
#include <random>
#include <vector>

// =============================================================================
// Memory Access Pattern Benchmarks
// Test cache locality - the core value proposition of Cell
// =============================================================================

// Simple object for traversal tests (fits in cache line)
struct alignas(64) CacheLineObject {
    uint64_t data[8] = {}; // 64 bytes = 1 cache line

    void touch() {
        data[0]++;
        benchmark::DoNotOptimize(data);
    }

    uint64_t sum() const {
        uint64_t s = 0;
        for (auto v : data)
            s += v;
        return s;
    }
};

// Smaller object (multiple per cache line)
struct SmallObject {
    uint64_t value = 0;

    void touch() {
        value++;
        benchmark::DoNotOptimize(value);
    }
};

// =============================================================================
// Cell Pool: Sequential Traversal
// =============================================================================

static void BM_Cell_Pool_Sequential(benchmark::State &state) {
    const size_t count = state.range(0);
    Cell::Context ctx;
    Cell::Pool<SmallObject> pool(ctx);

    std::vector<SmallObject *> objects(count);
    for (size_t i = 0; i < count; ++i) {
        objects[i] = pool.alloc();
    }

    for (auto _ : state) {
        // Sequential traversal - should be cache-friendly
        for (size_t i = 0; i < count; ++i) {
            objects[i]->touch();
        }
    }

    state.SetItemsProcessed(state.iterations() * count);
    state.SetBytesProcessed(state.iterations() * count * sizeof(SmallObject));
}
BENCHMARK(BM_Cell_Pool_Sequential)->Arg(1000)->Arg(10000)->Arg(100000);

// =============================================================================
// malloc: Sequential Traversal
// =============================================================================

static void BM_Malloc_Sequential(benchmark::State &state) {
    const size_t count = state.range(0);

    std::vector<SmallObject *> objects(count);
    for (size_t i = 0; i < count; ++i) {
        objects[i] = static_cast<SmallObject *>(std::malloc(sizeof(SmallObject)));
        new (objects[i]) SmallObject();
    }

    for (auto _ : state) {
        // Sequential traversal
        for (size_t i = 0; i < count; ++i) {
            objects[i]->touch();
        }
    }

    state.SetItemsProcessed(state.iterations() * count);
    state.SetBytesProcessed(state.iterations() * count * sizeof(SmallObject));

    for (auto *obj : objects) {
        std::free(obj);
    }
}
BENCHMARK(BM_Malloc_Sequential)->Arg(1000)->Arg(10000)->Arg(100000);

// =============================================================================
// Cell Pool: Random Access (stress test for cache)
// =============================================================================

static void BM_Cell_Pool_Random(benchmark::State &state) {
    const size_t count = state.range(0);
    Cell::Context ctx;
    Cell::Pool<SmallObject> pool(ctx);

    std::vector<SmallObject *> objects(count);
    for (size_t i = 0; i < count; ++i) {
        objects[i] = pool.alloc();
    }

    // Create random access pattern (same for each run)
    std::vector<size_t> indices(count);
    std::iota(indices.begin(), indices.end(), 0);
    std::mt19937 rng(42);
    std::shuffle(indices.begin(), indices.end(), rng);

    for (auto _ : state) {
        // Random access - tests how scattered the memory is
        for (size_t i = 0; i < count; ++i) {
            objects[indices[i]]->touch();
        }
    }

    state.SetItemsProcessed(state.iterations() * count);
    state.SetBytesProcessed(state.iterations() * count * sizeof(SmallObject));
}
BENCHMARK(BM_Cell_Pool_Random)->Arg(1000)->Arg(10000)->Arg(100000);

// =============================================================================
// malloc: Random Access
// =============================================================================

static void BM_Malloc_Random(benchmark::State &state) {
    const size_t count = state.range(0);

    std::vector<SmallObject *> objects(count);
    for (size_t i = 0; i < count; ++i) {
        objects[i] = static_cast<SmallObject *>(std::malloc(sizeof(SmallObject)));
        new (objects[i]) SmallObject();
    }

    // Same random pattern
    std::vector<size_t> indices(count);
    std::iota(indices.begin(), indices.end(), 0);
    std::mt19937 rng(42);
    std::shuffle(indices.begin(), indices.end(), rng);

    for (auto _ : state) {
        for (size_t i = 0; i < count; ++i) {
            objects[indices[i]]->touch();
        }
    }

    state.SetItemsProcessed(state.iterations() * count);
    state.SetBytesProcessed(state.iterations() * count * sizeof(SmallObject));

    for (auto *obj : objects) {
        std::free(obj);
    }
}
BENCHMARK(BM_Malloc_Random)->Arg(1000)->Arg(10000)->Arg(100000);

// =============================================================================
// Arena: Linear Allocation + Access (best case for Cell)
// =============================================================================

static void BM_Arena_LinearAccess(benchmark::State &state) {
    const size_t count = state.range(0);
    Cell::Context ctx;

    for (auto _ : state) {
        Cell::Arena arena(ctx);

        // Allocate all objects linearly
        SmallObject *objects = arena.alloc_array<SmallObject>(count);
        benchmark::DoNotOptimize(objects);

        // Access them - should be perfectly cache-friendly
        for (size_t i = 0; i < count; ++i) {
            objects[i].touch();
        }

        // Arena auto-resets on destruction
    }

    state.SetItemsProcessed(state.iterations() * count);
    state.SetBytesProcessed(state.iterations() * count * sizeof(SmallObject));
}
BENCHMARK(BM_Arena_LinearAccess)->Arg(1000)->Arg(10000)->Arg(100000);

// =============================================================================
// Vector (baseline): Linear Access
// =============================================================================

static void BM_Vector_LinearAccess(benchmark::State &state) {
    const size_t count = state.range(0);

    for (auto _ : state) {
        std::vector<SmallObject> objects(count);
        benchmark::DoNotOptimize(objects.data());

        for (size_t i = 0; i < count; ++i) {
            objects[i].touch();
        }
    }

    state.SetItemsProcessed(state.iterations() * count);
    state.SetBytesProcessed(state.iterations() * count * sizeof(SmallObject));
}
BENCHMARK(BM_Vector_LinearAccess)->Arg(1000)->Arg(10000)->Arg(100000);

// =============================================================================
// Fragmented malloc: Worst case for cache
// =============================================================================

static void BM_Malloc_Fragmented(benchmark::State &state) {
    const size_t count = state.range(0);

    // Create fragmentation by interleaving allocations of different sizes
    std::vector<void *> frag_allocs;
    std::vector<SmallObject *> objects(count);

    for (size_t i = 0; i < count; ++i) {
        // Interleave with random-sized allocations to fragment heap
        frag_allocs.push_back(std::malloc(rand() % 4096 + 1));
        objects[i] = static_cast<SmallObject *>(std::malloc(sizeof(SmallObject)));
        new (objects[i]) SmallObject();
    }

    // Free the fragmenting allocations
    for (auto *p : frag_allocs) {
        std::free(p);
    }

    for (auto _ : state) {
        for (size_t i = 0; i < count; ++i) {
            objects[i]->touch();
        }
    }

    state.SetItemsProcessed(state.iterations() * count);
    state.SetBytesProcessed(state.iterations() * count * sizeof(SmallObject));

    for (auto *obj : objects) {
        std::free(obj);
    }
}
BENCHMARK(BM_Malloc_Fragmented)->Arg(1000)->Arg(10000)->Arg(100000);

// =============================================================================
// Cache Line Object Tests (64-byte aligned)
// =============================================================================

static void BM_Cell_CacheLine_Sequential(benchmark::State &state) {
    const size_t count = state.range(0);
    Cell::Context ctx;
    Cell::Pool<CacheLineObject> pool(ctx);

    std::vector<CacheLineObject *> objects(count);
    for (size_t i = 0; i < count; ++i) {
        objects[i] = pool.alloc();
    }

    for (auto _ : state) {
        for (size_t i = 0; i < count; ++i) {
            objects[i]->touch();
        }
    }

    state.SetItemsProcessed(state.iterations() * count);
    state.SetBytesProcessed(state.iterations() * count * sizeof(CacheLineObject));
}
BENCHMARK(BM_Cell_CacheLine_Sequential)->Arg(1000)->Arg(10000);

static void BM_Malloc_CacheLine_Sequential(benchmark::State &state) {
    const size_t count = state.range(0);

    std::vector<CacheLineObject *> objects(count);
    for (size_t i = 0; i < count; ++i) {
#if defined(_WIN32)
        objects[i] = static_cast<CacheLineObject *>(_aligned_malloc(sizeof(CacheLineObject), 64));
#else
        objects[i] = static_cast<CacheLineObject *>(aligned_alloc(64, sizeof(CacheLineObject)));
#endif
        new (objects[i]) CacheLineObject();
    }

    for (auto _ : state) {
        for (size_t i = 0; i < count; ++i) {
            objects[i]->touch();
        }
    }

    state.SetItemsProcessed(state.iterations() * count);
    state.SetBytesProcessed(state.iterations() * count * sizeof(CacheLineObject));

    for (auto *obj : objects) {
#if defined(_WIN32)
        _aligned_free(obj);
#else
        std::free(obj);
#endif
    }
}
BENCHMARK(BM_Malloc_CacheLine_Sequential)->Arg(1000)->Arg(10000);
