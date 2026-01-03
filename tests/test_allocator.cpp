#include "cell/context.h"

#include <cassert>
#include <cstdio>
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

// Test 1: Basic cell allocation and free
TEST(BasicCellAllocFree) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024; // 16MB for testing

    Cell::Context ctx(config);

    // Allocate a cell
    Cell::CellData *cell = ctx.alloc_cell(42);
    assert(cell != nullptr && "Failed to allocate cell");
    assert(cell->header.tag == 42 && "Tag not set correctly");

    // Free it
    ctx.free_cell(cell);

    printf("  PASSED\n");
}

// Test 2: Multiple allocations fill TLS cache
TEST(TlsCacheFill) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024;

    Cell::Context ctx(config);

    // Allocate more than TLS cache capacity
    std::vector<Cell::CellData *> cells;
    const size_t count = Cell::kTlsCacheCapacity + 10;

    printf("  Allocating %zu cells...\n", count);
    for (size_t i = 0; i < count; ++i) {
        Cell::CellData *cell = ctx.alloc_cell(static_cast<uint8_t>(i & 0xFF));
        if (!cell) {
            printf("  FAILED: allocation %zu returned nullptr\n", i);
            assert(false);
        }
        cells.push_back(cell);
    }
    printf("  Allocated %zu cells successfully\n", count);

    // Free all
    printf("  Freeing cells...\n");
    for (size_t i = 0; i < cells.size(); ++i) {
        ctx.free_cell(cells[i]);
    }
    printf("  Freed %zu cells\n", cells.size());

    printf("  PASSED (allocated %zu cells)\n", count);
}

// Test 3: Superblock carving - allocate more than one superblock worth
TEST(SuperblockCarving) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024;

    Cell::Context ctx(config);

    // Allocate more than one superblock worth of cells
    const size_t count = Cell::kCellsPerSuperblock + 10;
    std::vector<Cell::CellData *> cells;

    for (size_t i = 0; i < count; ++i) {
        Cell::CellData *cell = ctx.alloc_cell(0);
        assert(cell != nullptr && "Failed to allocate cell");
        cells.push_back(cell);
    }

    // Free all
    for (auto *cell : cells) {
        ctx.free_cell(cell);
    }

    printf("  PASSED (allocated %zu cells across superblocks)\n", count);
}

// Test 4: Multi-threaded cell allocation
TEST(MultiThreadedCell) {
    Cell::Config config;
    config.reserve_size = 64 * 1024 * 1024; // 64MB

    Cell::Context ctx(config);
    constexpr int num_threads = 4;
    constexpr int allocs_per_thread = 100;

    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&ctx, &success_count, t, allocs_per_thread]() {
            std::vector<Cell::CellData *> local_cells;
            for (int i = 0; i < allocs_per_thread; ++i) {
                Cell::CellData *cell = ctx.alloc_cell(static_cast<uint8_t>(t));
                if (cell) {
                    local_cells.push_back(cell);
                }
            }
            for (auto *cell : local_cells) {
                ctx.free_cell(cell);
            }
            success_count += static_cast<int>(local_cells.size());
        });
    }

    for (auto &t : threads) {
        t.join();
    }

    printf("  PASSED (%d allocations across %d threads)\n", success_count.load(), num_threads);
}

// Test 5: Leak detection - verify cells are properly returned to pool
TEST(CellLeakDetection) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024; // 16MB - half goes to cells, half to buddy

    Cell::Context ctx(config);

    // Calculate max cells we could allocate from reserved space
    const size_t max_cells = config.reserve_size / Cell::kCellSize;
    (void)max_cells; // Unused but kept for documentation

    // First pass: allocate many cells and free them
    std::vector<Cell::CellData *> cells;
    const size_t alloc_count = Cell::kCellsPerSuperblock * 2; // 256 cells = 2 superblocks

    printf("  Pass 1: Allocating %zu cells...\n", alloc_count);
    for (size_t i = 0; i < alloc_count; ++i) {
        Cell::CellData *cell = ctx.alloc_cell(0);
        assert(cell != nullptr && "Allocation failed");
        cells.push_back(cell);
    }

    printf("  Pass 1: Freeing all %zu cells...\n", alloc_count);
    for (auto *cell : cells) {
        ctx.free_cell(cell);
    }
    cells.clear();

    // Second pass: allocate the same amount again
    // If there were leaks, we'd run out of memory faster
    printf("  Pass 2: Re-allocating %zu cells (should reuse freed cells)...\n", alloc_count);
    for (size_t i = 0; i < alloc_count; ++i) {
        Cell::CellData *cell = ctx.alloc_cell(0);
        assert(cell != nullptr && "Re-allocation failed - possible leak!");
        cells.push_back(cell);
    }

    // Free again
    for (auto *cell : cells) {
        ctx.free_cell(cell);
    }
    cells.clear();

    // Third pass: stress test - allocate/free in a loop
    printf("  Pass 3: Stress test - 1000 alloc/free cycles...\n");
    for (int cycle = 0; cycle < 1000; ++cycle) {
        Cell::CellData *cell = ctx.alloc_cell(0);
        assert(cell != nullptr && "Stress allocation failed");
        ctx.free_cell(cell);
    }

    printf("  PASSED (no leaks detected: %zu cells recycled)\n", alloc_count);
}

// Test 6: Memory decommit
TEST(MemoryDecommit) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024;

    Cell::Context ctx(config);

    // Allocate 2 superblocks worth of cells
    const size_t count = Cell::kCellsPerSuperblock * 2;
    std::vector<Cell::CellData *> cells;

    for (size_t i = 0; i < count; ++i) {
        Cell::CellData *cell = ctx.alloc_cell(0);
        assert(cell != nullptr);
        cells.push_back(cell);
    }

    size_t committed_before_free = ctx.committed_bytes();
    printf("  Committed after alloc: %zu bytes\n", committed_before_free);
    assert(committed_before_free >= count * Cell::kCellSize && "Should have committed memory");

    // Free all cells
    for (auto *cell : cells) {
        ctx.free_cell(cell);
    }
    cells.clear();

    // Decommit unused memory
    size_t freed = ctx.decommit_unused();
    size_t committed_after = ctx.committed_bytes();

    printf("  Decommitted: %zu bytes\n", freed);
    printf("  Committed after decommit: %zu bytes\n", committed_after);
    assert(freed > 0 && "Should have freed some memory");
    assert(committed_after < committed_before_free && "Committed should decrease");

    // Verify we can still allocate (recommit works)
    Cell::CellData *cell = ctx.alloc_cell(0);
    assert(cell != nullptr && "Allocation after decommit should work");
    ctx.free_cell(cell);

    printf("  PASSED\n");
}

int main() {
    printf("Cell Allocator Tests\n");
    printf("====================\n");
    printf("Configuration:\n");
    printf("  Cell size: %zu bytes\n", Cell::kCellSize);
    printf("  Superblock size: %zu bytes\n", Cell::kSuperblockSize);
    printf("  Cells per superblock: %zu\n", Cell::kCellsPerSuperblock);
    printf("  TLS cache capacity: %zu\n", Cell::kTlsCacheCapacity);
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
