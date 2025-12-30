#include "cell/arena.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
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
// Arena Allocator Tests
// =============================================================================

// Test 1: Basic allocation
TEST(BasicArenaAlloc) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024;

    Cell::Context ctx(config);
    Cell::Arena arena(ctx, 1);

    // Allocate a few objects
    void *p1 = arena.alloc(64);
    void *p2 = arena.alloc(128);
    void *p3 = arena.alloc(256);

    assert(p1 != nullptr && "First allocation failed");
    assert(p2 != nullptr && "Second allocation failed");
    assert(p3 != nullptr && "Third allocation failed");

    // Verify they're different addresses
    assert(p1 != p2 && p2 != p3 && "Allocations should be unique");

    // Verify we can write to them
    std::memset(p1, 0xAA, 64);
    std::memset(p2, 0xBB, 128);
    std::memset(p3, 0xCC, 256);

    printf("  PASSED\n");
}

// Test 2: Typed allocation
TEST(TypedArenaAlloc) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024;

    Cell::Context ctx(config);
    Cell::Arena arena(ctx);

    struct Transform {
        float position[3];
        float rotation[4];
        float scale[3];
    };

    Transform *t = arena.alloc<Transform>();
    assert(t != nullptr && "Failed to allocate Transform");

    t->position[0] = 1.0f;
    t->position[1] = 2.0f;
    t->position[2] = 3.0f;

    printf("  PASSED (sizeof(Transform) = %zu)\n", sizeof(Transform));
}

// Test 3: Array allocation
TEST(ArrayArenaAlloc) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024;

    Cell::Context ctx(config);
    Cell::Arena arena(ctx);

    int *arr = arena.alloc_array<int>(100);
    assert(arr != nullptr && "Failed to allocate int array");

    // Write to all elements
    for (int i = 0; i < 100; ++i) {
        arr[i] = i * i;
    }

    // Verify
    for (int i = 0; i < 100; ++i) {
        assert(arr[i] == i * i && "Array data corrupted");
    }

    printf("  PASSED (100 ints)\n");
}

// Test 4: Reset functionality
TEST(ArenaReset) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024;

    Cell::Context ctx(config);
    Cell::Arena arena(ctx);

    // Allocate some objects
    for (int i = 0; i < 100; ++i) {
        void *p = arena.alloc(64);
        assert(p != nullptr);
    }

    size_t before_reset = arena.bytes_allocated();
    printf("  Allocated %zu bytes before reset\n", before_reset);

    // Reset
    arena.reset();

    assert(arena.bytes_allocated() == 0 && "bytes_allocated should be 0 after reset");
    assert(arena.cell_count() > 0 && "Cells should be retained after reset");

    // Allocate again - should reuse same cells
    void *p = arena.alloc(64);
    assert(p != nullptr && "Allocation after reset failed");

    printf("  PASSED\n");
}

// Test 5: Release functionality
TEST(ArenaRelease) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024;

    Cell::Context ctx(config);
    Cell::Arena arena(ctx);

    // Allocate enough to use multiple cells
    for (int i = 0; i < 100; ++i) {
        arena.alloc(1024);
    }

    size_t cells_before = arena.cell_count();
    printf("  Cells before release: %zu\n", cells_before);

    // Release
    arena.release();

    assert(arena.cell_count() == 0 && "cell_count should be 0 after release");
    assert(arena.bytes_allocated() == 0 && "bytes_allocated should be 0 after release");

    printf("  PASSED\n");
}

// Test 6: Alignment
TEST(ArenaAlignment) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024;

    Cell::Context ctx(config);
    Cell::Arena arena(ctx);

    // Test various alignments
    void *p8 = arena.alloc(10, 8);
    void *p16 = arena.alloc(10, 16);
    void *p32 = arena.alloc(10, 32);
    void *p64 = arena.alloc(10, 64);

    assert(p8 != nullptr && "8-byte aligned alloc failed");
    assert(p16 != nullptr && "16-byte aligned alloc failed");
    assert(p32 != nullptr && "32-byte aligned alloc failed");
    assert(p64 != nullptr && "64-byte aligned alloc failed");

    assert((reinterpret_cast<uintptr_t>(p8) % 8) == 0 && "8-byte alignment broken");
    assert((reinterpret_cast<uintptr_t>(p16) % 16) == 0 && "16-byte alignment broken");
    assert((reinterpret_cast<uintptr_t>(p32) % 32) == 0 && "32-byte alignment broken");
    assert((reinterpret_cast<uintptr_t>(p64) % 64) == 0 && "64-byte alignment broken");

    printf("  PASSED (8, 16, 32, 64 byte alignments verified)\n");
}

// Test 7: Auto-growth across cells
TEST(ArenaAutoGrowth) {
    Cell::Config config;
    config.reserve_size = 64 * 1024 * 1024;

    Cell::Context ctx(config);
    Cell::Arena arena(ctx);

    // Allocate more than one cell's worth
    // Each cell has ~16KB usable space
    size_t alloc_size = 1024;
    size_t count = 100; // 100KB total, needs ~7 cells

    for (size_t i = 0; i < count; ++i) {
        void *p = arena.alloc(alloc_size);
        assert(p != nullptr && "Allocation failed during growth");
    }

    printf("  Allocated %zu x %zu = %zu bytes\n", count, alloc_size, count * alloc_size);
    printf("  Used %zu cells\n", arena.cell_count());

    assert(arena.cell_count() > 1 && "Should have used multiple cells");

    printf("  PASSED\n");
}

// Test 8: Markers
TEST(ArenaMarkers) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024;

    Cell::Context ctx(config);
    Cell::Arena arena(ctx);

    // Allocate some persistent data
    int *persistent = arena.alloc_array<int>(10);
    for (int i = 0; i < 10; ++i) {
        persistent[i] = i;
    }

    // Save marker
    auto marker = arena.save();
    size_t bytes_at_marker = arena.bytes_allocated();

    // Allocate temporary data
    int *temp1 = arena.alloc_array<int>(100);
    int *temp2 = arena.alloc_array<int>(100);
    (void)temp1;
    (void)temp2;

    printf("  Before marker reset: %zu bytes\n", arena.bytes_allocated());

    // Reset to marker
    arena.reset_to_marker(marker);

    printf("  After marker reset: %zu bytes\n", arena.bytes_allocated());

    // Persistent data should still be valid
    for (int i = 0; i < 10; ++i) {
        assert(persistent[i] == i && "Persistent data corrupted after marker reset");
    }

    printf("  PASSED\n");
}

// Test 9: Many allocations
TEST(ArenaManyAllocations) {
    Cell::Config config;
    config.reserve_size = 64 * 1024 * 1024;

    Cell::Context ctx(config);
    Cell::Arena arena(ctx);

    printf("  Allocating 10000 small objects...\n");
    for (int i = 0; i < 10000; ++i) {
        void *p = arena.alloc(32);
        assert(p != nullptr && "Allocation failed");
    }

    printf("  Total allocated: %zu bytes\n", arena.bytes_allocated());
    printf("  Cells used: %zu\n", arena.cell_count());

    arena.reset();

    printf("  After reset: %zu bytes allocated\n", arena.bytes_allocated());

    printf("  PASSED\n");
}

// Test 10: Large allocation (falls back to Context)
TEST(ArenaLargeAllocation) {
    Cell::Config config;
    config.reserve_size = 64 * 1024 * 1024;

    Cell::Context ctx(config);
    Cell::Arena arena(ctx);

    // Allocate something larger than a cell
    void *large = arena.alloc(32 * 1024); // 32KB
    assert(large != nullptr && "Large allocation failed");

    // Write to it
    std::memset(large, 0xDD, 32 * 1024);

    printf("  PASSED (32KB allocation succeeded)\n");
}

// Test 11: Introspection
TEST(ArenaIntrospection) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024;

    Cell::Context ctx(config);
    Cell::Arena arena(ctx);

    assert(arena.bytes_allocated() == 0 && "Should start at 0");
    assert(arena.cell_count() == 0 && "Should start with 0 cells");

    arena.alloc(100);

    assert(arena.bytes_allocated() == 100 && "Should track allocation");
    assert(arena.cell_count() == 1 && "Should have 1 cell");
    assert(arena.bytes_remaining() > 0 && "Should have remaining space");

    printf("  bytes_allocated: %zu\n", arena.bytes_allocated());
    printf("  bytes_remaining: %zu\n", arena.bytes_remaining());
    printf("  cell_count: %zu\n", arena.cell_count());

    printf("  PASSED\n");
}

// =============================================================================
// Main
// =============================================================================

int main() {
    printf("Arena Allocator Tests\n");
    printf("=====================\n");
    printf("Configuration:\n");
    printf("  Cell size: %zu bytes\n", Cell::kCellSize);
    printf("  Block start offset: %zu bytes\n", Cell::kBlockStartOffset);
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
