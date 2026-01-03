/**
 * @file test_stress.cpp
 * @brief Comprehensive stress tests and edge case verification for Cell library.
 *
 * These tests are designed to exercise edge cases, high concurrency,
 * and cross-tier transitions to ensure production readiness.
 */

#include "cell/context.h"
#include "cell/stl_allocator.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

// Simple test helper
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
// Edge Case Tests
// =============================================================================

// Test 1: Zero-size allocation handling
TEST(ZeroSizeAlloc) {
    Cell::Context ctx;

    void *p = ctx.alloc_bytes(0);
    assert(p == nullptr && "Zero-size allocation should return nullptr");

    p = ctx.alloc_large(0);
    assert(p == nullptr && "Zero-size large allocation should return nullptr");

    printf("  PASSED\n");
}

// Test 2: All size class boundaries
TEST(SizeClassBoundaries) {
    Cell::Context ctx;

    // Test exact size class boundaries and off-by-one
    size_t boundaries[] = {15,   16,   17,   31,   32,   33,   63,   64,   65,   127,
                           128,  129,  255,  256,  257,  511,  512,  513,  1023, 1024,
                           1025, 2047, 2048, 2049, 4095, 4096, 4097, 8191, 8192, 8193};

    std::vector<void *> ptrs;
    for (size_t size : boundaries) {
        void *p = ctx.alloc_bytes(size);
        assert(p != nullptr && "Boundary allocation should succeed");
        // Write to verify memory is usable
        std::memset(p, 0xAB, size);
        ptrs.push_back(p);
    }

    for (void *p : ptrs) {
        ctx.free_bytes(p);
    }

    printf("  PASSED (%zu boundary sizes tested)\n", sizeof(boundaries) / sizeof(size_t));
}

// Test 3: Alignment validation via LargeAllocRegistry (>2MB or high alignment)
TEST(AlignmentValidation) {
    Cell::Context ctx;

    // LargeAllocRegistry supports arbitrary alignment for large allocations
    struct TestCase {
        size_t size;
        size_t align;
    } cases[] = {
        {3 * 1024 * 1024, 4096},        // 3MB with 4KB alignment
        {4 * 1024 * 1024, 1024 * 1024}, // 4MB with 1MB alignment
    };

    for (auto &tc : cases) {
        void *p = ctx.alloc_aligned(tc.size, tc.align);
        if (p != nullptr) {
            uintptr_t addr = reinterpret_cast<uintptr_t>(p);
            assert((addr % tc.align) == 0 && "Alignment violated");
            std::memset(p, 0xCD, tc.size);
            ctx.free_bytes(p);
            printf("  %zuMB @ %zuKB align: OK\n", tc.size / (1024 * 1024), tc.align / 1024);
        } else {
            printf("  %zuMB @ %zuKB align: skipped\n", tc.size / (1024 * 1024), tc.align / 1024);
        }
    }

    printf("  PASSED\n");
}

// Test 4: Buddy tier boundary (around 32KB and 2MB)
TEST(BuddyBoundaries) {
    Cell::Context ctx;

    // Just below buddy min (should use cell)
    void *p1 = ctx.alloc_bytes(16 * 1024 - 64); // ~16KB - header
    assert(p1 != nullptr);

    // At buddy minimum
    void *p2 = ctx.alloc_large(32 * 1024); // 32KB
    assert(p2 != nullptr);

    // Just below buddy max
    void *p3 = ctx.alloc_large(2 * 1024 * 1024 - 8); // ~2MB
    assert(p3 != nullptr);

    // At large allocation threshold
    void *p4 = ctx.alloc_large(3 * 1024 * 1024); // 3MB
    assert(p4 != nullptr);

    ctx.free_bytes(p1);
    ctx.free_large(p2);
    ctx.free_large(p3);
    ctx.free_large(p4);

    printf("  PASSED\n");
}

// Test 5: Null pointer free (should be no-op)
TEST(NullFreeSafety) {
    Cell::Context ctx;

    // These should all be safe no-ops
    ctx.free_bytes(nullptr);
    ctx.free_large(nullptr);
    ctx.free_cell(nullptr);

    printf("  PASSED\n");
}

// =============================================================================
// High Concurrency Stress Tests
// =============================================================================

// Test 6: Many threads, rapid alloc/free
TEST(HighConcurrencySubCell) {
    Cell::Context ctx;
    constexpr int num_threads = 8;
    constexpr int ops_per_thread = 10000;

    std::atomic<int> success_count{0};
    std::atomic<int> failure_count{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&ctx, &success_count, &failure_count, t, ops_per_thread]() {
            std::mt19937 rng(t * 12345);
            std::uniform_int_distribution<size_t> size_dist(16, 4096);

            for (int i = 0; i < ops_per_thread; ++i) {
                size_t size = size_dist(rng);
                void *p = ctx.alloc_bytes(size);
                if (p) {
                    std::memset(p, 0xEE, size);
                    ctx.free_bytes(p);
                    success_count.fetch_add(1, std::memory_order_relaxed);
                } else {
                    failure_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto &t : threads) {
        t.join();
    }

    printf("  Operations: %d successful, %d failed\n", success_count.load(), failure_count.load());
    assert(failure_count.load() == 0 && "Some allocations failed unexpectedly");
    printf("  PASSED\n");
}

// Test 7: Concurrent mixed-tier allocations
TEST(ConcurrentMixedTiers) {
    Cell::Context ctx;
    constexpr int num_threads = 4;
    constexpr int rounds = 100;

    std::atomic<int> total_ops{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&ctx, &total_ops, t, rounds]() {
            std::vector<std::pair<void *, size_t>> allocs;

            for (int r = 0; r < rounds; ++r) {
                // Allocate from different tiers
                size_t sizes[] = {64, 1024, 8000, 20000, 100000, 1000000};

                for (size_t size : sizes) {
                    void *p = ctx.alloc_bytes(size);
                    if (p) {
                        allocs.push_back({p, size});
                        total_ops.fetch_add(1, std::memory_order_relaxed);
                    }
                }

                // Free in random order
                while (!allocs.empty()) {
                    size_t idx = total_ops.load() % allocs.size();
                    ctx.free_bytes(allocs[idx].first);
                    allocs.erase(allocs.begin() + idx);
                }
            }
        });
    }

    for (auto &t : threads) {
        t.join();
    }

    printf("  Total operations: %d\n", total_ops.load());
    printf("  PASSED\n");
}

// Test 8: Producer-consumer pattern (simplified to avoid complex synchronization)
TEST(ProducerConsumer) {
    Cell::Context ctx;
    constexpr int total_items = 1000;

    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};
    std::vector<std::atomic<void *>> buffer(total_items);

    for (auto &slot : buffer) {
        slot.store(nullptr);
    }

    // Producer
    std::thread producer([&]() {
        for (int i = 0; i < total_items; ++i) {
            void *p = ctx.alloc_bytes(256);
            assert(p != nullptr);
            std::memset(p, static_cast<uint8_t>(i), 256);
            buffer[i].store(p, std::memory_order_release);
            produced.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Consumer
    std::thread consumer([&]() {
        for (int i = 0; i < total_items; ++i) {
            void *p = nullptr;
            while ((p = buffer[i].load(std::memory_order_acquire)) == nullptr) {
                std::this_thread::yield();
            }
            ctx.free_bytes(p);
            consumed.fetch_add(1, std::memory_order_relaxed);
        }
    });

    producer.join();
    consumer.join();

    assert(consumed.load() == total_items && "Not all items consumed");
    printf("  Produced/Consumed: %d/%d items\n", produced.load(), consumed.load());
    printf("  PASSED\n");
}

// =============================================================================
// Realloc Stress Tests
// =============================================================================

// Test 9: Rapid realloc growth
TEST(ReallocGrowthStress) {
    Cell::Context ctx;

    void *p = ctx.alloc_bytes(16);
    assert(p != nullptr);

    // Pattern to verify data integrity
    static_cast<uint8_t *>(p)[0] = 0x42;

    size_t current_size = 16;
    for (int i = 0; i < 20; ++i) {
        size_t new_size = current_size * 2;
        void *new_p = ctx.realloc_bytes(p, new_size);
        assert(new_p != nullptr && "Realloc failed");

        // Verify first byte preserved
        assert(static_cast<uint8_t *>(new_p)[0] == 0x42 && "Data corrupted");

        p = new_p;
        current_size = new_size;
    }

    // Should now be ~16MB
    assert(current_size == 16 * 1024 * 1024);
    ctx.free_bytes(p);

    printf("  Grew from 16B to %zuMB\n", current_size / (1024 * 1024));
    printf("  PASSED\n");
}

// Test 10: Oscillating realloc (grow/shrink pattern)
TEST(ReallocOscillate) {
    Cell::Context ctx;
    constexpr int iterations = 100;

    void *p = ctx.alloc_bytes(1024);
    assert(p != nullptr);

    for (int i = 0; i < iterations; ++i) {
        // Grow
        p = ctx.realloc_bytes(p, 100000);
        assert(p != nullptr);
        std::memset(p, 0xAA, 100000);

        // Shrink
        p = ctx.realloc_bytes(p, 64);
        assert(p != nullptr);
    }

    ctx.free_bytes(p);
    printf("  PASSED (%d oscillations)\n", iterations);
}

// =============================================================================
// Memory Pattern Tests
// =============================================================================

// Test 11: LIFO allocation pattern (stack-like)
TEST(LifoPattern) {
    Cell::Context ctx;
    std::vector<void *> stack;
    constexpr int depth = 1000;

    for (int i = 0; i < depth; ++i) {
        void *p = ctx.alloc_bytes(128);
        assert(p != nullptr);
        stack.push_back(p);
    }

    while (!stack.empty()) {
        ctx.free_bytes(stack.back());
        stack.pop_back();
    }

    printf("  PASSED (depth=%d)\n", depth);
}

// Test 12: FIFO allocation pattern (queue-like)
TEST(FifoPattern) {
    Cell::Context ctx;
    std::vector<void *> queue;
    constexpr int total = 10000;
    constexpr int window = 100;

    for (int i = 0; i < total; ++i) {
        void *p = ctx.alloc_bytes(64);
        assert(p != nullptr);
        queue.push_back(p);

        if (queue.size() > window) {
            ctx.free_bytes(queue.front());
            queue.erase(queue.begin());
        }
    }

    // Drain remaining
    for (void *p : queue) {
        ctx.free_bytes(p);
    }

    printf("  PASSED (total=%d, window=%d)\n", total, window);
}

// Test 13: Random allocation/free pattern
TEST(RandomPattern) {
    Cell::Context ctx;
    std::vector<void *> live;
    std::mt19937 rng(42);
    constexpr int operations = 10000;

    for (int i = 0; i < operations; ++i) {
        bool should_alloc = live.empty() || (rng() % 2 == 0);

        if (should_alloc) {
            size_t size = 16 + (rng() % 8000);
            void *p = ctx.alloc_bytes(size);
            if (p) {
                live.push_back(p);
            }
        } else {
            size_t idx = rng() % live.size();
            ctx.free_bytes(live[idx]);
            live.erase(live.begin() + idx);
        }
    }

    // Cleanup
    for (void *p : live) {
        ctx.free_bytes(p);
    }

    printf("  PASSED (%d operations)\n", operations);
}

// =============================================================================
// STL Container Stress
// =============================================================================

// Test 14: Vector with many reallocations
TEST(StlVectorStress) {
    Cell::Context ctx;
    Cell::StlAllocator<int> alloc(ctx);

    std::vector<int, Cell::StlAllocator<int>> vec(alloc);

    // Force many reallocations
    for (int i = 0; i < 100000; ++i) {
        vec.push_back(i);
    }

    // Verify data integrity
    for (int i = 0; i < 100000; ++i) {
        assert(vec[i] == i);
    }

    vec.clear();
    vec.shrink_to_fit();

    printf("  PASSED\n");
}

// Test 15: Multiple containers sharing context
TEST(MultipleContainers) {
    Cell::Context ctx;
    Cell::StlAllocator<int> alloc(ctx);

    std::vector<int, Cell::StlAllocator<int>> v1(alloc);
    std::vector<int, Cell::StlAllocator<int>> v2(alloc);
    std::vector<int, Cell::StlAllocator<int>> v3(alloc);

    for (int i = 0; i < 1000; ++i) {
        v1.push_back(i);
        v2.push_back(i * 2);
        v3.push_back(i * 3);
    }

    // Interleaved operations
    for (int i = 0; i < 500; ++i) {
        v1.pop_back();
        v2.push_back(i);
        v3[i] = v1[i];
    }

    printf("  PASSED\n");
}

// =============================================================================
// Edge Case Stress
// =============================================================================

// Test 16: Immediate free after alloc (cache stress)
TEST(ImmediateFreeStress) {
    Cell::Context ctx;
    constexpr int iterations = 100000;

    for (int i = 0; i < iterations; ++i) {
        void *p = ctx.alloc_bytes(64);
        assert(p != nullptr);
        ctx.free_bytes(p);
    }

    printf("  PASSED (%d iterations)\n", iterations);
}

// Test 17: Single byte allocations
TEST(SingleByteAllocs) {
    Cell::Context ctx;
    std::vector<void *> ptrs;
    constexpr int count = 1000;

    for (int i = 0; i < count; ++i) {
        void *p = ctx.alloc_bytes(1);
        assert(p != nullptr);
        *static_cast<uint8_t *>(p) = static_cast<uint8_t>(i);
        ptrs.push_back(p);
    }

    // Verify and free
    for (int i = 0; i < count; ++i) {
        assert(*static_cast<uint8_t *>(ptrs[i]) == static_cast<uint8_t>(i));
        ctx.free_bytes(ptrs[i]);
    }

    printf("  PASSED (%d single-byte allocations)\n", count);
}

// Test 18: Maximum alignment request
TEST(MaxAlignmentRequest) {
    Cell::Context ctx;

    // 2MB alignment (huge page size) - must use large allocation (>2MB)
    void *p = ctx.alloc_aligned(4 * 1024 * 1024, 2 * 1024 * 1024);
    if (p != nullptr) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(p);
        assert((addr % (2 * 1024 * 1024)) == 0 && "2MB alignment violated");
        ctx.free_bytes(p);
        printf("  2MB alignment: OK\n");
    } else {
        printf("  2MB alignment: skipped (insufficient memory)\n");
    }

    // 4KB alignment on large allocation - must use size > 2MB for LargeAllocRegistry
    p = ctx.alloc_aligned(3 * 1024 * 1024, 4096);
    assert(p != nullptr);
    uintptr_t addr = reinterpret_cast<uintptr_t>(p);
    assert((addr % 4096) == 0 && "4KB alignment violated");
    ctx.free_bytes(p);

    printf("  PASSED\n");
}

// =============================================================================
// Main
// =============================================================================

int main() {
    printf("Cell Library Stress Tests\n");
    printf("=========================\n");
    printf("These tests verify edge cases and high-load scenarios.\n\n");

    int passed = 0;
    int failed = 0;

    auto start = std::chrono::high_resolution_clock::now();

    for (const auto &test : tests) {
        printf("Running %s...\n", test.name);
        try {
            test.fn();
            ++passed;
        } catch (...) {
            printf("  FAILED (exception)\n");
            ++failed;
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    printf("\n");
    printf("Results: %d passed, %d failed (%.2fs)\n", passed, failed, duration.count() / 1000.0);
    return failed == 0 ? 0 : 1;
}
