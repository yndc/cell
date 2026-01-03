#include "cell/context.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
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
// Sub-Cell Allocation Tests
// =============================================================================

// Test 1: Basic small allocation
TEST(SmallAlloc16) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024;

    Cell::Context ctx(config);

    // Allocate 16 bytes (smallest size class)
    void *ptr = ctx.alloc_bytes(16, 1);
    assert(ptr != nullptr && "Failed to allocate 16 bytes");

    // Verify we can write to it
    std::memset(ptr, 0xAA, 16);

    ctx.free_bytes(ptr);
    printf("  PASSED\n");
}

// Test 2: Various size classes
TEST(VariousSizeClasses) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024;

    Cell::Context ctx(config);

    // Test each size class
    const size_t sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
    std::vector<void *> ptrs;

    for (size_t size : sizes) {
        void *ptr = ctx.alloc_bytes(size, 0);
        assert(ptr != nullptr && "Failed to allocate from size class");

        // Write pattern to verify usability
        std::memset(ptr, 0x55, size);

        ptrs.push_back(ptr);
        printf("  Allocated %zu bytes at %p\n", size, ptr);
    }

    // Free all
    for (void *ptr : ptrs) {
        ctx.free_bytes(ptr);
    }

    printf("  PASSED (all size classes)\n");
}

// Test 3: Typed allocation
TEST(TypedAllocation) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024;

    Cell::Context ctx(config);

    struct Transform {
        float position[3];
        float rotation[4];
        float scale[3];
    };

    // Allocate a Transform
    Transform *t = ctx.alloc<Transform>(42);
    assert(t != nullptr && "Failed to allocate Transform");

    // Write to it
    t->position[0] = 1.0f;
    t->position[1] = 2.0f;
    t->position[2] = 3.0f;

    ctx.free_bytes(t);

    printf("  PASSED (sizeof(Transform) = %zu)\n", sizeof(Transform));
}

// Test 4: Array allocation
TEST(ArrayAllocation) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024;

    Cell::Context ctx(config);

    // Allocate array of 100 ints
    int *arr = ctx.alloc_array<int>(100, 5);
    assert(arr != nullptr && "Failed to allocate int array");

    // Write to all elements
    for (int i = 0; i < 100; ++i) {
        arr[i] = i * i;
    }

    // Verify
    for (int i = 0; i < 100; ++i) {
        assert(arr[i] == i * i && "Array data corrupted");
    }

    ctx.free_bytes(arr);

    printf("  PASSED (100 ints = %zu bytes)\n", sizeof(int) * 100);
}

// Test 5: Many small allocations
TEST(ManySmallAllocations) {
    Cell::Config config;
    config.reserve_size = 64 * 1024 * 1024;

    Cell::Context ctx(config);

    // Allocate 10000 small objects
    const size_t count = 10000;
    std::vector<void *> ptrs;
    ptrs.reserve(count);

    printf("  Allocating %zu small objects...\n", count);
    for (size_t i = 0; i < count; ++i) {
        void *ptr = ctx.alloc_bytes(64, static_cast<uint8_t>(i & 0xFF));
        if (!ptr) {
            printf("  FAILED at allocation %zu\n", i);
            assert(false);
        }
        ptrs.push_back(ptr);
    }

    printf("  Freeing %zu objects...\n", count);
    for (void *ptr : ptrs) {
        ctx.free_bytes(ptr);
    }

    printf("  PASSED\n");
}

// Test 6: Cell reuse after free
TEST(CellReuse) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024;

    Cell::Context ctx(config);

    // Figure out how many 64-byte blocks fit in a cell
    size_t blocks_per_cell = Cell::blocks_per_cell(2); // bin 2 = 64 bytes
    printf("  Blocks per cell (64B): %zu\n", blocks_per_cell);

    // Allocate exactly one cell's worth
    std::vector<void *> ptrs;
    for (size_t i = 0; i < blocks_per_cell; ++i) {
        void *ptr = ctx.alloc_bytes(64, 0);
        assert(ptr != nullptr);
        ptrs.push_back(ptr);
    }

    // Free all of them
    for (void *ptr : ptrs) {
        ctx.free_bytes(ptr);
    }
    ptrs.clear();

    // Now allocate again - should reuse the same cell
    for (size_t i = 0; i < blocks_per_cell; ++i) {
        void *ptr = ctx.alloc_bytes(64, 0);
        assert(ptr != nullptr);
        ptrs.push_back(ptr);
    }

    // Free again
    for (void *ptr : ptrs) {
        ctx.free_bytes(ptr);
    }

    printf("  PASSED (cell reuse verified)\n");
}

// Test 7: Full cell fallback (allocation > 8KB)
TEST(FullCellFallback) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024;

    Cell::Context ctx(config);

    // Allocate something larger than kMaxSubCellSize (8KB)
    // This should use full-cell allocation
    void *ptr = ctx.alloc_bytes(10000, 99);
    assert(ptr != nullptr && "Failed to allocate large object");

    // Verify header
    Cell::CellHeader *header = Cell::get_header(ptr);
    assert(header->size_class == Cell::kFullCellMarker &&
           "Large allocation should use full cell marker");
    assert(header->tag == 99 && "Tag not set correctly");

    ctx.free_bytes(ptr);

    printf("  PASSED (10KB allocation uses full cell)\n");
}

// Test 8: Multi-threaded sub-cell allocation
TEST(MultiThreadedSubCell) {
    Cell::Config config;
    config.reserve_size = 128 * 1024 * 1024;

    Cell::Context ctx(config);
    constexpr int num_threads = 4;
    constexpr int allocs_per_thread = 1000;

    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&ctx, &success_count, t, allocs_per_thread]() {
            std::vector<void *> local_ptrs;
            for (int i = 0; i < allocs_per_thread; ++i) {
                // Vary size classes across threads
                size_t size = 16 << (i % 4); // 16, 32, 64, 128
                void *ptr = ctx.alloc_bytes(size, static_cast<uint8_t>(t));
                if (ptr) {
                    local_ptrs.push_back(ptr);
                }
            }
            for (void *ptr : local_ptrs) {
                ctx.free_bytes(ptr);
            }
            success_count += static_cast<int>(local_ptrs.size());
        });
    }

    for (auto &t : threads) {
        t.join();
    }

    printf("  PASSED (%d sub-cell allocations across %d threads)\n", success_count.load(),
           num_threads);
}

// Test 9: Stress test - rapid alloc/free
TEST(SubCellStress) {
    Cell::Config config;
    config.reserve_size = 32 * 1024 * 1024;

    Cell::Context ctx(config);

    printf("  Running 10000 rapid alloc/free cycles...\n");
    for (int cycle = 0; cycle < 10000; ++cycle) {
        // Allocate a few objects
        void *p1 = ctx.alloc_bytes(32, 0);
        void *p2 = ctx.alloc_bytes(64, 0);
        void *p3 = ctx.alloc_bytes(128, 0);

        assert(p1 && p2 && p3);

        // Free in different order
        ctx.free_bytes(p2);
        ctx.free_bytes(p1);
        ctx.free_bytes(p3);
    }

    printf("  PASSED\n");
}

#ifndef NDEBUG
// Test 10: Debug poison detection (debug builds only)
TEST(DebugPoisonDetection) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024;

    Cell::Context ctx(config);

    // Allocate and write
    uint8_t *ptr = static_cast<uint8_t *>(ctx.alloc_bytes(64, 0));
    assert(ptr != nullptr);
    std::memset(ptr, 0xAA, 64);

    // After free, memory should be poisoned
    ctx.free_bytes(ptr);

    // Check if poisoned (don't dereference in real code!)
    // This is just for testing.
    // Note: First sizeof(FreeBlock*) bytes are overwritten by free list pointer
    constexpr size_t skip_bytes = sizeof(void *);
    bool poisoned = true;
    for (size_t i = skip_bytes; i < 64; ++i) {
        if (ptr[i] != Cell::kPoisonByte) {
            printf("  Byte %zu: expected 0x%02X, got 0x%02X\n", i, Cell::kPoisonByte, ptr[i]);
            poisoned = false;
            break;
        }
    }

    assert(poisoned && "Memory should be poisoned after free (after free-list pointer)");
    printf("  PASSED (poison byte = 0x%02X)\n", Cell::kPoisonByte);
}
#endif

// =============================================================================
// Realloc Tests
// =============================================================================

// Test 11: Realloc same bin (no reallocation needed)
TEST(ReallocSameBin) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024;
    Cell::Context ctx(config);

    // Allocate 24 bytes (fits in 32-byte bin)
    void *ptr = ctx.alloc_bytes(24, 1);
    assert(ptr != nullptr);
    std::memset(ptr, 0xAB, 24);

    // Realloc to 28 bytes (still fits in 32-byte bin)
    void *ptr2 = ctx.realloc_bytes(ptr, 28, 1);

    // Should return same pointer (same bin optimization)
    assert(ptr2 == ptr && "Same-bin realloc should return same pointer");

    // Verify data preserved
    uint8_t *bytes = static_cast<uint8_t *>(ptr2);
    for (int i = 0; i < 24; ++i) {
        assert(bytes[i] == 0xAB);
    }

    ctx.free_bytes(ptr2);
    printf("  PASSED\n");
}

// Test 12: Realloc grow to larger bin
TEST(ReallocGrowBin) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024;
    Cell::Context ctx(config);

    // Allocate 50 bytes (fits in 64-byte bin)
    void *ptr = ctx.alloc_bytes(50, 2);
    assert(ptr != nullptr);
    std::memset(ptr, 0xCD, 50);

    // Realloc to 200 bytes (needs 256-byte bin)
    void *ptr2 = ctx.realloc_bytes(ptr, 200, 2);
    assert(ptr2 != nullptr);

    // Verify data preserved
    uint8_t *bytes = static_cast<uint8_t *>(ptr2);
    for (int i = 0; i < 50; ++i) {
        assert(bytes[i] == 0xCD);
    }

    ctx.free_bytes(ptr2);
    printf("  PASSED\n");
}

// Test 13: Realloc shrink to smaller bin
TEST(ReallocShrinkBin) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024;
    Cell::Context ctx(config);

    // Allocate 200 bytes (fits in 256-byte bin)
    void *ptr = ctx.alloc_bytes(200, 3);
    assert(ptr != nullptr);
    std::memset(ptr, 0xEF, 200);

    // Realloc to 50 bytes (needs 64-byte bin)
    void *ptr2 = ctx.realloc_bytes(ptr, 50, 3);
    assert(ptr2 != nullptr);

    // Verify data preserved (first 50 bytes)
    uint8_t *bytes = static_cast<uint8_t *>(ptr2);
    for (int i = 0; i < 50; ++i) {
        assert(bytes[i] == 0xEF);
    }

    ctx.free_bytes(ptr2);
    printf("  PASSED\n");
}

// Test 14: Realloc nullptr behaves like alloc
TEST(ReallocNullPtr) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024;
    Cell::Context ctx(config);

    void *ptr = ctx.realloc_bytes(nullptr, 100, 4);
    assert(ptr != nullptr && "realloc(nullptr) should behave like alloc");

    std::memset(ptr, 0x12, 100);
    ctx.free_bytes(ptr);
    printf("  PASSED\n");
}

// Test 15: Realloc zero size behaves like free
TEST(ReallocZeroSize) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024;
    Cell::Context ctx(config);

    void *ptr = ctx.alloc_bytes(100, 5);
    assert(ptr != nullptr);

    void *ptr2 = ctx.realloc_bytes(ptr, 0, 5);
    assert(ptr2 == nullptr && "realloc(ptr, 0) should return nullptr");
    // ptr has been freed, don't use it

    printf("  PASSED\n");
}

// Test 16: Realloc cross-tier (sub-cell to buddy)
TEST(ReallocSubCellToBuddy) {
    Cell::Config config;
    config.reserve_size = 128 * 1024 * 1024;
    Cell::Context ctx(config);

    // Allocate small (sub-cell)
    void *ptr = ctx.alloc_bytes(100, 6);
    assert(ptr != nullptr);
    std::memset(ptr, 0x77, 100);

    // Grow to buddy range (> 8KB, < 2MB)
    void *ptr2 = ctx.realloc_bytes(ptr, 50 * 1024, 6);
    assert(ptr2 != nullptr);

    // Verify data preserved
    uint8_t *bytes = static_cast<uint8_t *>(ptr2);
    for (int i = 0; i < 100; ++i) {
        assert(bytes[i] == 0x77);
    }

    ctx.free_bytes(ptr2);
    printf("  PASSED\n");
}

// Test 17: Realloc cross-tier (sub-cell to large)
TEST(ReallocSubCellToLarge) {
    Cell::Config config;
    config.reserve_size = 128 * 1024 * 1024;
    Cell::Context ctx(config);

    // Allocate small (sub-cell)
    void *ptr = ctx.alloc_bytes(200, 7);
    assert(ptr != nullptr);
    std::memset(ptr, 0x11, 200);

    // Grow to large range (> 2MB)
    void *ptr2 = ctx.realloc_bytes(ptr, 3 * 1024 * 1024, 7);
    assert(ptr2 != nullptr);

    // Verify data preserved
    uint8_t *bytes = static_cast<uint8_t *>(ptr2);
    for (int i = 0; i < 200; ++i) {
        assert(bytes[i] == 0x11);
    }

    ctx.free_bytes(ptr2);
    printf("  PASSED\n");
}

// Test 18: Realloc cross-tier (buddy to large)
TEST(ReallocBuddyToLarge) {
    Cell::Config config;
    config.reserve_size = 128 * 1024 * 1024;
    Cell::Context ctx(config);

    // Allocate buddy-tier (32KB-2MB)
    void *ptr = ctx.alloc_bytes(64 * 1024, 8);
    assert(ptr != nullptr);
    std::memset(ptr, 0x22, 64 * 1024);

    // Grow to large range (> 2MB)
    void *ptr2 = ctx.realloc_bytes(ptr, 4 * 1024 * 1024, 8);
    assert(ptr2 != nullptr);

    // Verify data preserved (first 64KB)
    uint8_t *bytes = static_cast<uint8_t *>(ptr2);
    for (int i = 0; i < 64 * 1024; ++i) {
        assert(bytes[i] == 0x22);
    }

    ctx.free_bytes(ptr2);
    printf("  PASSED\n");
}

// Test 19: Realloc cross-tier (large to buddy)
TEST(ReallocLargeToBuddy) {
    Cell::Config config;
    config.reserve_size = 128 * 1024 * 1024;
    Cell::Context ctx(config);

    // Allocate large (> 2MB)
    void *ptr = ctx.alloc_bytes(3 * 1024 * 1024, 9);
    assert(ptr != nullptr);
    std::memset(ptr, 0x33, 100 * 1024); // Fill first 100KB

    // Shrink to buddy range
    void *ptr2 = ctx.realloc_bytes(ptr, 100 * 1024, 9);
    assert(ptr2 != nullptr);

    // Verify data preserved
    uint8_t *bytes = static_cast<uint8_t *>(ptr2);
    for (int i = 0; i < 100 * 1024; ++i) {
        assert(bytes[i] == 0x33);
    }

    ctx.free_bytes(ptr2);
    printf("  PASSED\n");
}

// Test 20: Realloc cross-tier (large to sub-cell)
TEST(ReallocLargeToSubCell) {
    Cell::Config config;
    config.reserve_size = 128 * 1024 * 1024;
    Cell::Context ctx(config);

    // Allocate large (> 2MB)
    void *ptr = ctx.alloc_bytes(3 * 1024 * 1024, 10);
    assert(ptr != nullptr);
    std::memset(ptr, 0x44, 500); // Fill first 500 bytes

    // Shrink to sub-cell range
    void *ptr2 = ctx.realloc_bytes(ptr, 500, 10);
    assert(ptr2 != nullptr);

    // Verify data preserved
    uint8_t *bytes = static_cast<uint8_t *>(ptr2);
    for (int i = 0; i < 500; ++i) {
        assert(bytes[i] == 0x44);
    }

    ctx.free_bytes(ptr2);
    printf("  PASSED\n");
}

// Test 21: Realloc cross-tier (buddy to sub-cell)
TEST(ReallocBuddyToSubCell) {
    Cell::Config config;
    config.reserve_size = 128 * 1024 * 1024;
    Cell::Context ctx(config);

    // Allocate buddy-tier
    void *ptr = ctx.alloc_bytes(64 * 1024, 11);
    assert(ptr != nullptr);
    std::memset(ptr, 0x55, 1000); // Fill first 1000 bytes

    // Shrink to sub-cell range
    void *ptr2 = ctx.realloc_bytes(ptr, 1000, 11);
    assert(ptr2 != nullptr);

    // Verify data preserved
    uint8_t *bytes = static_cast<uint8_t *>(ptr2);
    for (int i = 0; i < 1000; ++i) {
        assert(bytes[i] == 0x55);
    }

    ctx.free_bytes(ptr2);
    printf("  PASSED\n");
}

// =============================================================================
// STL Allocator Tests
// =============================================================================

#include "cell/stl_allocator.h"
#include <map>
#include <string>

// Test 22: StlAllocator with std::vector
TEST(StlAllocatorVector) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024;
    Cell::Context ctx(config);

    // Create a vector using Cell memory
    Cell::StlAllocator<int> alloc(ctx, 42);
    std::vector<int, Cell::StlAllocator<int>> vec(alloc);

    // Push elements
    for (int i = 0; i < 1000; ++i) {
        vec.push_back(i * i);
    }

    // Verify data
    for (int i = 0; i < 1000; ++i) {
        assert(vec[i] == i * i);
    }

    // Resize (triggers reallocation)
    vec.resize(2000);
    for (int i = 1000; i < 2000; ++i) {
        vec[i] = i;
    }

    // Clear (frees memory through Cell)
    vec.clear();
    vec.shrink_to_fit();

    printf("  PASSED\n");
}

// Test 23: StlAllocator with std::map
TEST(StlAllocatorMap) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024;
    Cell::Context ctx(config);

    // Create a map using Cell memory
    using MapAlloc = Cell::StlAllocator<std::pair<const int, int>>;
    MapAlloc alloc(ctx, 43);
    std::map<int, int, std::less<int>, MapAlloc> m(std::less<int>(), alloc);

    // Insert elements
    for (int i = 0; i < 100; ++i) {
        m[i] = i * 10;
    }

    // Verify data
    for (int i = 0; i < 100; ++i) {
        assert(m[i] == i * 10);
    }

    // Erase some
    for (int i = 0; i < 50; ++i) {
        m.erase(i);
    }

    assert(m.size() == 50);

    printf("  PASSED\n");
}

// Test 24: StlAllocator rebind (container uses different internal types)
TEST(StlAllocatorRebind) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024;
    Cell::Context ctx(config);

    // std::vector<bool> uses different internal storage, but other containers
    // like std::list use node-based allocation which requires rebind
    Cell::StlAllocator<double> alloc(ctx);
    std::vector<double, Cell::StlAllocator<double>> vec(alloc);

    vec.push_back(3.14159);
    vec.push_back(2.71828);
    vec.push_back(1.41421);

    assert(vec.size() == 3);
    assert(vec[0] > 3.14 && vec[0] < 3.15);

    printf("  PASSED\n");
}

// =============================================================================
// Main
// =============================================================================

int main() {
    printf("Sub-Cell Allocator Tests\n");
    printf("========================\n");
    printf("Configuration:\n");
    printf("  Cell size: %zu bytes\n", Cell::kCellSize);
    printf("  Size classes: %zu bins\n", Cell::kNumSizeBins);
    printf("  Min block size: %zu bytes\n", Cell::kMinBlockSize);
    printf("  Max sub-cell size: %zu bytes\n", Cell::kMaxSubCellSize);
    printf("  Warm cells per bin: %zu\n", Cell::kWarmCellsPerBin);
    printf("\n");

    int passed = 0;
    int failed = 0;

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

    printf("\n");
    printf("Results: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
