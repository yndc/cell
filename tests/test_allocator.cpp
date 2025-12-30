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

// Test 1: Basic allocation and free
TEST(BasicAllocFree) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024; // 16MB for testing

    Cell::Context ctx(config);

    // Allocate a cell
    Cell::CellData *cell = ctx.alloc(42);
    assert(cell != nullptr && "Failed to allocate cell");
    assert(cell->header.tag == 42 && "Tag not set correctly");

    // Free it
    ctx.free(cell);

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
        Cell::CellData *cell = ctx.alloc(static_cast<uint8_t>(i & 0xFF));
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
        ctx.free(cells[i]);
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
        Cell::CellData *cell = ctx.alloc(0);
        assert(cell != nullptr && "Failed to allocate cell");
        cells.push_back(cell);
    }

    // Free all
    for (auto *cell : cells) {
        ctx.free(cell);
    }

    printf("  PASSED (allocated %zu cells across superblocks)\n", count);
}

// Test 4: Multi-threaded allocation
TEST(MultiThreaded) {
    Cell::Config config;
    config.reserve_size = 64 * 1024 * 1024; // 64MB

    Cell::Context ctx(config);
    constexpr int num_threads = 4;
    constexpr int allocs_per_thread = 100;

    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&ctx, &success_count, t]() {
            std::vector<Cell::CellData *> local_cells;
            for (int i = 0; i < allocs_per_thread; ++i) {
                Cell::CellData *cell = ctx.alloc(static_cast<uint8_t>(t));
                if (cell) {
                    local_cells.push_back(cell);
                }
            }
            for (auto *cell : local_cells) {
                ctx.free(cell);
            }
            success_count += static_cast<int>(local_cells.size());
        });
    }

    for (auto &t : threads) {
        t.join();
    }

    printf("  PASSED (%d allocations across %d threads)\n", success_count.load(), num_threads);
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
