/**
 * @file test_fuzz.cpp
 * @brief Comprehensive fuzzing stress tests for Cell memory library.
 *
 * These tests use randomized inputs with seed-controlled reproducibility to
 * exercise edge cases, cross-tier transitions, and concurrent operations
 * under adversarial conditions.
 */

#include "cell/arena.h"
#include "cell/context.h"
#include "cell/pool.h"
#include "cell/stl_allocator.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

// =============================================================================
// Test Infrastructure
// =============================================================================

#define TEST(name)                                                                                 \
    void test_##name();                                                                            \
    struct Register##name {                                                                        \
        Register##name() { tests.push_back({#name, test_##name}); }                                \
    } reg_##name;                                                                                  \
    void test_##name()

struct TestCase {
    const char *name;
    void (*fn)();
};
std::vector<TestCase> tests;

// =============================================================================
// Fuzzing Utilities
// =============================================================================

/** @brief Calculate a simple checksum for memory content validation. */
inline uint64_t checksum(const void *ptr, size_t size) {
    uint64_t sum = 0;
    const uint8_t *bytes = static_cast<const uint8_t *>(ptr);
    for (size_t i = 0; i < size; ++i) {
        sum = sum * 31 + bytes[i];
    }
    return sum;
}

/** @brief Fill memory with a reproducible pattern based on seed. */
inline void fill_pattern(void *ptr, size_t size, uint64_t seed) {
    uint8_t *bytes = static_cast<uint8_t *>(ptr);
    for (size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<uint8_t>((seed * 31 + i) & 0xFF);
    }
}

/** @brief Verify memory pattern matches expected seed-based content. */
inline bool verify_pattern(const void *ptr, size_t size, uint64_t seed) {
    const uint8_t *bytes = static_cast<const uint8_t *>(ptr);
    for (size_t i = 0; i < size; ++i) {
        uint8_t expected = static_cast<uint8_t>((seed * 31 + i) & 0xFF);
        if (bytes[i] != expected) {
            return false;
        }
    }
    return true;
}

/** @brief Allocation record for tracking live allocations. */
struct AllocRecord {
    void *ptr;
    size_t size;
    uint64_t pattern_seed;
};

// =============================================================================
// Fuzzing Tests
// =============================================================================

/**
 * Test 1: Randomized size allocation fuzzing
 * Allocates blocks of random sizes across all tiers.
 */
TEST(RandomSizeFuzzing) {
    Cell::Context ctx;
    std::mt19937_64 rng(0xDEADBEEF);

    // Size distribution that covers all tiers:
    // Sub-cell: 1-16KB, Cell: 16KB, Buddy: 32KB-2MB, Large: >2MB
    std::vector<AllocRecord> live;
    constexpr int iterations = 5000;

    for (int i = 0; i < iterations; ++i) {
        // Generate random size with bias towards interesting boundaries
        size_t size;
        int tier = rng() % 100;
        if (tier < 40) {
            // Sub-cell tier (40%): 1 byte to 16KB
            size = 1 + (rng() % (16 * 1024));
        } else if (tier < 60) {
            // Cell tier (20%): near 16KB boundary
            size = 16 * 1024 + (rng() % 128) - 64;
        } else if (tier < 85) {
            // Buddy tier (25%): 32KB to 2MB
            size = 32 * 1024 + (rng() % (2 * 1024 * 1024 - 32 * 1024));
        } else {
            // Large tier (15%): 2MB to 8MB
            size = 2 * 1024 * 1024 + (rng() % (6 * 1024 * 1024));
        }

        void *p = ctx.alloc_bytes(size);
        if (p) {
            uint64_t seed = rng();
            fill_pattern(p, size, seed);
            live.push_back({p, size, seed});
        }

        // Randomly free some allocations
        if (!live.empty() && (rng() % 3 == 0)) {
            size_t idx = rng() % live.size();
            assert(verify_pattern(live[idx].ptr, live[idx].size, live[idx].pattern_seed) &&
                   "Memory corruption detected!");
            ctx.free_bytes(live[idx].ptr);
            live.erase(live.begin() + idx);
        }
    }

    // Verify and cleanup remaining
    for (auto &rec : live) {
        assert(verify_pattern(rec.ptr, rec.size, rec.pattern_seed) && "Memory corruption!");
        ctx.free_bytes(rec.ptr);
    }

    printf("  PASSED (%d iterations, all tiers covered)\n", iterations);
}

/**
 * Test 2: Cross-tier transition fuzzing
 * Specifically stresses allocation patterns that cross tier boundaries.
 */
TEST(CrossTierTransitionFuzzing) {
    Cell::Context ctx;
    std::mt19937_64 rng(0xCAFEBABE);

    // Boundary sizes to stress - focused on small/medium sizes for density
    size_t boundaries[] = {// Sub-cell boundaries
                           15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 255, 256, 257,
                           // Near cell boundary
                           16 * 1024 - 8, 16 * 1024, 16 * 1024 + 8,
                           // Near buddy min boundary
                           32 * 1024 - 8, 32 * 1024, 32 * 1024 + 8,
                           // Buddy power-of-2 boundaries (smaller ones)
                           64 * 1024, 128 * 1024, 256 * 1024};

    constexpr int rounds = 50;
    std::vector<AllocRecord> live;

    for (int r = 0; r < rounds; ++r) {
        // Shuffle allocation order each round
        std::shuffle(std::begin(boundaries), std::end(boundaries), rng);

        for (size_t size : boundaries) {
            void *p = ctx.alloc_bytes(size);
            if (p) {
                uint64_t seed = rng();
                // Only fill up to 1KB to avoid performance issues with large allocs
                size_t fill_size = std::min(size, size_t{1024});
                fill_pattern(p, fill_size, seed);
                live.push_back({p, fill_size, seed}); // Store fill_size for verification
            }
        }

        // Free in random order - more aggressively to avoid memory exhaustion
        std::shuffle(live.begin(), live.end(), rng);
        while (!live.empty() && (rng() % 2 == 0 || live.size() > 30)) {
            auto &rec = live.back();
            assert(verify_pattern(rec.ptr, rec.size, rec.pattern_seed) && "Memory corruption!");
            ctx.free_bytes(rec.ptr);
            live.pop_back();
        }
    }

    // Cleanup
    for (auto &rec : live) {
        assert(verify_pattern(rec.ptr, rec.size, rec.pattern_seed));
        ctx.free_bytes(rec.ptr);
    }

    printf("  PASSED (%d rounds, %zu boundary sizes)\n", rounds,
           sizeof(boundaries) / sizeof(size_t));
}

/**
 * Test 3: Concurrent fuzzing with multiple threads
 * Each thread performs random alloc/free operations.
 */
TEST(ConcurrentFuzzing) {
    Cell::Context ctx;
    constexpr int num_threads = 8;
    constexpr int ops_per_thread = 2000;

    std::atomic<int> success_count{0};
    std::atomic<int> corruption_count{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&ctx, &success_count, &corruption_count, t]() {
            std::mt19937_64 rng(t * 0x12345678);
            std::vector<AllocRecord> local_live;

            for (int i = 0; i < ops_per_thread; ++i) {
                int action = rng() % 10;

                if (action < 6 || local_live.empty()) {
                    // Allocate (60% or if empty)
                    size_t size = 1 + (rng() % 100000); // Up to ~100KB
                    void *p = ctx.alloc_bytes(size);
                    if (p) {
                        uint64_t seed = rng();
                        fill_pattern(p, size, seed);
                        local_live.push_back({p, size, seed});
                        success_count.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    // Free random allocation (40%)
                    size_t idx = rng() % local_live.size();
                    auto &rec = local_live[idx];
                    if (!verify_pattern(rec.ptr, rec.size, rec.pattern_seed)) {
                        corruption_count.fetch_add(1, std::memory_order_relaxed);
                    }
                    ctx.free_bytes(rec.ptr);
                    local_live.erase(local_live.begin() + idx);
                }
            }

            // Cleanup
            for (auto &rec : local_live) {
                if (!verify_pattern(rec.ptr, rec.size, rec.pattern_seed)) {
                    corruption_count.fetch_add(1, std::memory_order_relaxed);
                }
                ctx.free_bytes(rec.ptr);
            }
        });
    }

    for (auto &t : threads) {
        t.join();
    }

    assert(corruption_count.load() == 0 && "Memory corruption detected in concurrent test!");
    printf("  PASSED (%d threads, %d ops each, %d total allocs)\n", num_threads, ops_per_thread,
           success_count.load());
}

/**
 * Test 4: Realloc fuzzing with random size changes
 * Grows and shrinks allocations randomly while verifying data integrity.
 */
TEST(ReallocFuzzing) {
    Cell::Context ctx;
    std::mt19937_64 rng(0xBEEFCAFE);

    constexpr int num_blocks = 50;
    constexpr int iterations = 200;

    struct ReallocRecord {
        void *ptr;
        size_t size;
        uint64_t original_seed; // Seed for first min(size, 64) bytes
    };

    std::vector<ReallocRecord> blocks(num_blocks);

    // Initial allocations
    for (int i = 0; i < num_blocks; ++i) {
        size_t size = 64 + (rng() % 10000);
        void *p = ctx.alloc_bytes(size);
        assert(p != nullptr);
        uint64_t seed = rng();
        fill_pattern(p, size, seed);
        blocks[i] = {p, size, seed};
    }

    // Random realloc operations
    for (int i = 0; i < iterations; ++i) {
        size_t idx = rng() % num_blocks;
        auto &rec = blocks[idx];

        // Verify first 64 bytes still intact
        size_t check_size = std::min(rec.size, size_t{64});
        assert(verify_pattern(rec.ptr, check_size, rec.original_seed) &&
               "Data corruption before realloc!");

        // Random new size
        size_t new_size;
        int change = rng() % 4;
        if (change == 0) {
            new_size = rec.size * 2; // Double
        } else if (change == 1) {
            new_size = rec.size / 2; // Halve
        } else if (change == 2) {
            new_size = rec.size + (rng() % 10000); // Grow random
        } else {
            new_size = std::max(size_t{64}, rec.size - (rng() % (rec.size / 2 + 1))); // Shrink
        }

        new_size = std::max(new_size, size_t{64});               // Keep minimum
        new_size = std::min(new_size, size_t{10 * 1024 * 1024}); // Cap at 10MB

        void *new_ptr = ctx.realloc_bytes(rec.ptr, new_size);
        if (new_ptr) {
            // Verify preserved data
            check_size = std::min(std::min(rec.size, new_size), size_t{64});
            assert(verify_pattern(new_ptr, check_size, rec.original_seed) &&
                   "Data corruption after realloc!");

            rec.ptr = new_ptr;
            rec.size = new_size;
        }
    }

    // Cleanup
    for (auto &rec : blocks) {
        ctx.free_bytes(rec.ptr);
    }

    printf("  PASSED (%d blocks, %d realloc operations)\n", num_blocks, iterations);
}

/**
 * Test 5: Alignment fuzzing for large allocations
 * Tests various alignment requirements with random sizes.
 */
TEST(AlignmentFuzzing) {
    Cell::Context ctx;
    std::mt19937_64 rng(0xA11ACABC);

    // Alignments to test (must be power of 2)
    size_t alignments[] = {16, 32, 64, 128, 256, 512, 1024, 4096, 8192, 65536, 1024 * 1024};

    constexpr int iterations = 100;
    std::vector<AllocRecord> live;

    for (int i = 0; i < iterations; ++i) {
        size_t align = alignments[rng() % (sizeof(alignments) / sizeof(size_t))];
        // Size must be > 2MB for LargeAllocRegistry alignment support
        size_t size = 3 * 1024 * 1024 + (rng() % (5 * 1024 * 1024));

        void *p = ctx.alloc_aligned(size, align);
        if (p) {
            uintptr_t addr = reinterpret_cast<uintptr_t>(p);
            assert((addr % align) == 0 && "Alignment violated!");

            uint64_t seed = rng();
            fill_pattern(p, std::min(size, size_t{4096}), seed); // Only fill first 4KB
            live.push_back({p, size, seed});
        }

        // Random free
        if (!live.empty() && (rng() % 3 == 0)) {
            size_t idx = rng() % live.size();
            ctx.free_bytes(live[idx].ptr);
            live.erase(live.begin() + idx);
        }
    }

    // Cleanup
    for (auto &rec : live) {
        ctx.free_bytes(rec.ptr);
    }

    printf("  PASSED (%d iterations, %zu alignment values)\n", iterations,
           sizeof(alignments) / sizeof(size_t));
}

/**
 * Test 6: TLS cache stress fuzzing
 * Rapidly allocates and frees to stress thread-local caches.
 */
TEST(TlsCacheStressFuzzing) {
    Cell::Context ctx;
    std::mt19937_64 rng(0x71555EE5);

    constexpr int num_threads = 4;
    constexpr int rapid_cycles = 10000;

    std::atomic<int> total_ops{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&ctx, &total_ops, t]() {
            std::mt19937_64 local_rng(t * 0xABCDEF);

            for (int cycle = 0; cycle < rapid_cycles; ++cycle) {
                // Rapid burst of small allocations
                std::vector<void *> burst;
                int burst_size = 1 + (local_rng() % 32);

                for (int b = 0; b < burst_size; ++b) {
                    size_t size = 16 + (local_rng() % 4096);
                    void *p = ctx.alloc_bytes(size);
                    if (p) {
                        std::memset(p, 0xAB, size);
                        burst.push_back(p);
                    }
                }

                // Immediate free
                for (void *p : burst) {
                    ctx.free_bytes(p);
                }

                total_ops.fetch_add(burst_size, std::memory_order_relaxed);
            }
        });
    }

    for (auto &t : threads) {
        t.join();
    }

    printf("  PASSED (%d threads, %d total ops)\n", num_threads, total_ops.load());
}

/**
 * Test 7: Long-running stability fuzzing
 * Runs for extended period with mixed operations.
 */
TEST(LongRunningStabilityFuzzing) {
    Cell::Context ctx;
    std::mt19937_64 rng(0x57AB1E42);

    auto start = std::chrono::steady_clock::now();
    constexpr int target_duration_ms = 2000; // 2 seconds

    std::vector<AllocRecord> live;
    live.reserve(10000);

    int operations = 0;
    int peak_live = 0;

    while (true) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start);
        if (elapsed.count() >= target_duration_ms) {
            break;
        }

        int action = rng() % 100;

        if (action < 50 || live.empty()) {
            // Alloc (50%)
            size_t size = 1 + (rng() % 50000);
            void *p = ctx.alloc_bytes(size);
            if (p) {
                uint64_t seed = rng();
                fill_pattern(p, std::min(size, size_t{256}), seed);
                live.push_back({p, size, seed});
                peak_live = std::max(peak_live, static_cast<int>(live.size()));
            }
        } else if (action < 80) {
            // Free (30%)
            size_t idx = rng() % live.size();
            auto &rec = live[idx];
            size_t check = std::min(rec.size, size_t{256});
            assert(verify_pattern(rec.ptr, check, rec.pattern_seed) && "Memory corruption!");
            ctx.free_bytes(rec.ptr);
            live.erase(live.begin() + idx);
        } else if (action < 95) {
            // Realloc (15%)
            size_t idx = rng() % live.size();
            auto &rec = live[idx];
            size_t new_size = 1 + (rng() % 100000);
            void *new_ptr = ctx.realloc_bytes(rec.ptr, new_size);
            if (new_ptr) {
                rec.ptr = new_ptr;
                rec.size = new_size;
            }
        } else {
            // Zero-size allocation (5%)
            void *p = ctx.alloc_bytes(0);
            assert(p == nullptr && "Zero-size should return nullptr");
        }

        operations++;
    }

    // Cleanup
    for (auto &rec : live) {
        ctx.free_bytes(rec.ptr);
    }

    printf("  PASSED (%d ops in %dms, peak %d live)\n", operations, target_duration_ms, peak_live);
}

/**
 * Test 8: Deallocation order fuzzing
 * Tests various deallocation orders (LIFO, FIFO, random, reverse).
 */
TEST(DeallocationOrderFuzzing) {
    Cell::Context ctx;
    std::mt19937_64 rng(0xDEA110C0);

    constexpr int count = 1000;

    auto run_pattern = [&](const char *name, auto dealloc_fn) {
        std::vector<AllocRecord> live;
        for (int i = 0; i < count; ++i) {
            size_t size = 64 + (rng() % 8000);
            void *p = ctx.alloc_bytes(size);
            if (p) {
                uint64_t seed = i;
                fill_pattern(p, size, seed);
                live.push_back({p, size, seed});
            }
        }
        dealloc_fn(live, ctx, rng);
    };

    // LIFO pattern
    run_pattern("LIFO", [](std::vector<AllocRecord> &live, Cell::Context &ctx,
                           std::mt19937_64 & /* rng */) {
        while (!live.empty()) {
            auto &rec = live.back();
            assert(verify_pattern(rec.ptr, rec.size, rec.pattern_seed));
            ctx.free_bytes(rec.ptr);
            live.pop_back();
        }
    });

    // FIFO pattern
    run_pattern("FIFO", [](std::vector<AllocRecord> &live, Cell::Context &ctx,
                           std::mt19937_64 & /* rng */) {
        while (!live.empty()) {
            auto &rec = live.front();
            assert(verify_pattern(rec.ptr, rec.size, rec.pattern_seed));
            ctx.free_bytes(rec.ptr);
            live.erase(live.begin());
        }
    });

    // Random pattern
    run_pattern("Random",
                [](std::vector<AllocRecord> &live, Cell::Context &ctx, std::mt19937_64 &rng) {
                    while (!live.empty()) {
                        size_t idx = rng() % live.size();
                        auto &rec = live[idx];
                        assert(verify_pattern(rec.ptr, rec.size, rec.pattern_seed));
                        ctx.free_bytes(rec.ptr);
                        live.erase(live.begin() + idx);
                    }
                });

    // Odd-then-even pattern
    run_pattern("OddEven", [](std::vector<AllocRecord> &live, Cell::Context &ctx,
                              std::mt19937_64 & /* rng */) {
        // Free odd indices first
        for (size_t i = 1; i < live.size(); i += 2) {
            auto &rec = live[i];
            assert(verify_pattern(rec.ptr, rec.size, rec.pattern_seed));
            ctx.free_bytes(rec.ptr);
            live[i].ptr = nullptr;
        }
        // Then even indices
        for (size_t i = 0; i < live.size(); i += 2) {
            if (live[i].ptr) {
                auto &rec = live[i];
                assert(verify_pattern(rec.ptr, rec.size, rec.pattern_seed));
                ctx.free_bytes(rec.ptr);
            }
        }
        live.clear();
    });

    printf("  PASSED (4 deallocation patterns, %d allocations each)\n", count);
}

/**
 * Test 9: Size class boundary fuzzing
 * Intensively tests allocations near size class boundaries.
 */
TEST(SizeClassBoundaryFuzzing) {
    Cell::Context ctx;
    std::mt19937_64 rng(0xB00DADE1);

    // Generate near-boundary sizes
    std::vector<size_t> test_sizes;
    for (size_t base = 16; base <= 16384; base *= 2) {
        for (int delta = -3; delta <= 3; ++delta) {
            if (base + delta >= 1) {
                test_sizes.push_back(base + delta);
            }
        }
    }

    constexpr int rounds = 50;
    std::vector<AllocRecord> live;

    for (int r = 0; r < rounds; ++r) {
        std::shuffle(test_sizes.begin(), test_sizes.end(), rng);

        for (size_t size : test_sizes) {
            void *p = ctx.alloc_bytes(size);
            if (p) {
                uint64_t seed = rng();
                fill_pattern(p, size, seed);
                live.push_back({p, size, seed});
            }
        }

        // Random partial free
        std::shuffle(live.begin(), live.end(), rng);
        while (live.size() > 20) {
            auto &rec = live.back();
            assert(verify_pattern(rec.ptr, rec.size, rec.pattern_seed));
            ctx.free_bytes(rec.ptr);
            live.pop_back();
        }
    }

    // Final cleanup
    for (auto &rec : live) {
        ctx.free_bytes(rec.ptr);
    }

    printf("  PASSED (%d rounds, %zu boundary sizes)\n", rounds, test_sizes.size());
}

/**
 * Test 10: Pool and Arena fuzzing
 * Tests high-level abstractions under stress.
 */
TEST(PoolArenaFuzzing) {
    Cell::Context ctx;
    std::mt19937_64 rng(0xFEEDA1BE);

    struct TestObject {
        uint64_t data[8];

        void fill(uint64_t seed) {
            for (auto &d : data) {
                d = seed++;
            }
        }

        bool verify(uint64_t seed) const {
            for (auto d : data) {
                if (d != seed++) {
                    return false;
                }
            }
            return true;
        }
    };

    // Pool fuzzing
    {
        Cell::Pool<TestObject> pool(ctx);
        std::vector<std::pair<TestObject *, uint64_t>> live;

        for (int i = 0; i < 5000; ++i) {
            if (rng() % 3 != 0 || live.empty()) {
                TestObject *obj = pool.alloc();
                if (obj) {
                    uint64_t seed = rng();
                    obj->fill(seed);
                    live.push_back({obj, seed});
                }
            } else {
                size_t idx = rng() % live.size();
                auto &[obj, seed] = live[idx];
                assert(obj->verify(seed) && "Pool object corrupted!");
                pool.free(obj);
                live.erase(live.begin() + idx);
            }
        }

        for (auto &[obj, seed] : live) {
            assert(obj->verify(seed));
            pool.free(obj);
        }
    }

    // Arena fuzzing
    {
        for (int round = 0; round < 50; ++round) {
            Cell::Arena arena(ctx);
            std::vector<std::pair<TestObject *, uint64_t>> objects;

            int count = 10 + (rng() % 500);
            for (int i = 0; i < count; ++i) {
                TestObject *obj = arena.alloc<TestObject>();
                if (obj) {
                    uint64_t seed = rng();
                    obj->fill(seed);
                    objects.push_back({obj, seed});
                }
            }

            // Verify all before arena destruction
            for (auto &[obj, seed] : objects) {
                assert(obj->verify(seed) && "Arena object corrupted!");
            }
            // Arena auto-destructs and resets
        }
    }

    printf("  PASSED (Pool and Arena fuzzed)\n");
}

/**
 * Test 11: Adversarial allocation pattern
 * Specifically designed to stress fragmentation.
 */
TEST(AdversarialFragmentation) {
    Cell::Context ctx;
    std::mt19937_64 rng(0xADFE25E0);

    constexpr int waves = 20;
    constexpr int wave_size = 200;

    for (int wave = 0; wave < waves; ++wave) {
        std::vector<void *> small_allocs;
        std::vector<void *> large_allocs;

        // Interleave small and large allocations
        for (int i = 0; i < wave_size; ++i) {
            // Small allocation
            void *small = ctx.alloc_bytes(64 + (rng() % 256));
            if (small) {
                std::memset(small, 0xAA, 64);
                small_allocs.push_back(small);
            }

            // Large allocation
            void *large = ctx.alloc_bytes(50000 + (rng() % 100000));
            if (large) {
                std::memset(large, 0xBB, 1000);
                large_allocs.push_back(large);
            }
        }

        // Free only large allocations (creates holes)
        for (void *p : large_allocs) {
            ctx.free_bytes(p);
        }

        // Try to allocate large again (should find coalesced space)
        std::vector<void *> new_large;
        for (int i = 0; i < wave_size / 2; ++i) {
            void *p = ctx.alloc_bytes(100000 + (rng() % 200000));
            if (p) {
                new_large.push_back(p);
            }
        }

        // Cleanup
        for (void *p : small_allocs) {
            ctx.free_bytes(p);
        }
        for (void *p : new_large) {
            ctx.free_bytes(p);
        }
    }

    printf("  PASSED (%d fragmentation waves)\n", waves);
}

/**
 * Test 12: Seed reproducibility verification
 * Ensures that running with same seed produces same behavior.
 */
TEST(SeedReproducibility) {
    auto run_sequence = [](uint64_t seed) -> uint64_t {
        Cell::Context ctx;
        std::mt19937_64 rng(seed);
        uint64_t checksum = 0;

        for (int i = 0; i < 1000; ++i) {
            size_t size = 1 + (rng() % 10000);
            void *p = ctx.alloc_bytes(size);
            if (p) {
                checksum ^= reinterpret_cast<uintptr_t>(p) + size;
                ctx.free_bytes(p);
            }
        }
        return checksum;
    };

    uint64_t seed = 0x12345678;
    uint64_t result1 = run_sequence(seed);
    uint64_t result2 = run_sequence(seed);

    assert(result1 == result2 && "Non-deterministic behavior detected!");
    printf("  PASSED (reproducible with seed 0x%lX)\n", seed);
}

// =============================================================================
// Main
// =============================================================================

int main() {
    printf("Cell Library Fuzzing Stress Tests\n");
    printf("==================================\n");
    printf("These tests use randomized inputs to find edge cases.\n");
    printf("All tests use fixed seeds for reproducibility.\n\n");

    int passed = 0;
    int failed = 0;

    auto start = std::chrono::high_resolution_clock::now();

    for (const auto &test : tests) {
        printf("Running %s...\n", test.name);
        try {
            test.fn();
            ++passed;
        } catch (const std::exception &e) {
            printf("  FAILED: %s\n", e.what());
            ++failed;
        } catch (...) {
            printf("  FAILED (unknown exception)\n");
            ++failed;
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    printf("\n");
    printf("Results: %d passed, %d failed (%.2fs)\n", passed, failed, duration.count() / 1000.0);
    return failed == 0 ? 0 : 1;
}
