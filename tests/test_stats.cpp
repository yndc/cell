#include "cell/context.h"

#include <cassert>
#include <cstdio>
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
// Stats Tests (only run when CELL_ENABLE_STATS is defined)
// =============================================================================

#ifdef CELL_ENABLE_STATS

// Test 1: Basic stats tracking
TEST(StatsBasicTracking) {
    Cell::Config config;
    config.reserve_size = 64 * 1024 * 1024;

    Cell::Context ctx(config);

    // Initial state
    const auto &stats = ctx.get_stats();
    assert(stats.current_allocated == 0 && "Should start at zero");

    // Allocate some memory
    void *p1 = ctx.alloc_bytes(100, 1);
    assert(p1 != nullptr);

    assert(stats.current_allocated > 0 && "Should have allocated");
    assert(stats.total_allocated > 0 && "Total should increase");
    assert(stats.subcell_allocs >= 1 && "Should have sub-cell alloc");

    // Free it
    ctx.free_bytes(p1);
    assert(stats.current_allocated == 0 && "Should be zero after free");
    assert(stats.subcell_frees >= 1 && "Should have sub-cell free");

    printf("  PASSED\n");
}

// Test 2: Peak tracking
TEST(StatsPeakTracking) {
    Cell::Config config;
    config.reserve_size = 64 * 1024 * 1024;

    Cell::Context ctx(config);
    const auto &stats = ctx.get_stats();

    // Allocate
    void *p1 = ctx.alloc_bytes(1000, 0);
    void *p2 = ctx.alloc_bytes(2000, 0);

    size_t peak_after_alloc = stats.peak_allocated.load();

    // Free one
    ctx.free_bytes(p1);

    // Peak should not decrease
    assert(stats.peak_allocated >= peak_after_alloc && "Peak should not decrease");

    ctx.free_bytes(p2);

    // Peak should still be preserved
    assert(stats.peak_allocated >= peak_after_alloc && "Peak should persist after free");

    printf("  Peak: %zu bytes\n", stats.peak_allocated.load());
    printf("  PASSED\n");
}

// Test 3: Per-tag tracking
TEST(StatsPerTagTracking) {
    Cell::Config config;
    config.reserve_size = 64 * 1024 * 1024;

    Cell::Context ctx(config);
    const auto &stats = ctx.get_stats();

    // Allocate with different tags
    void *p1 = ctx.alloc_bytes(500, 10);
    void *p2 = ctx.alloc_bytes(1000, 20);
    void *p3 = ctx.alloc_bytes(1500, 10); // Same tag as p1

    size_t tag10 = stats.per_tag_current[10].load();
    size_t tag20 = stats.per_tag_current[20].load();

    printf("  Tag 10: %zu bytes\n", tag10);
    printf("  Tag 20: %zu bytes\n", tag20);

    assert(tag10 > 0 && "Tag 10 should have allocations");
    assert(tag20 > 0 && "Tag 20 should have allocations");

    ctx.free_bytes(p1);
    ctx.free_bytes(p2);
    ctx.free_bytes(p3);

    printf("  PASSED\n");
}

// Test 4: Allocator type tracking
TEST(StatsAllocatorTypes) {
    Cell::Config config;
    config.reserve_size = 128 * 1024 * 1024;

    Cell::Context ctx(config);
    const auto &stats = ctx.get_stats();

    // Sub-cell allocation
    void *p1 = ctx.alloc_bytes(100, 0);
    assert(stats.subcell_allocs >= 1);

    // Full cell allocation
    void *p2 = ctx.alloc_bytes(10 * 1024, 0); // 10KB -> full cell
    assert(stats.cell_allocs >= 1);

    // Buddy allocation
    void *p3 = ctx.alloc_bytes(64 * 1024, 0); // 64KB -> buddy
    assert(stats.buddy_allocs >= 1);

    // Large allocation
    void *p4 = ctx.alloc_bytes(4 * 1024 * 1024, 0); // 4MB -> large
    assert(stats.large_allocs >= 1);

    printf("  SubCell: %zu, Cell: %zu, Buddy: %zu, Large: %zu\n", stats.subcell_allocs.load(),
           stats.cell_allocs.load(), stats.buddy_allocs.load(), stats.large_allocs.load());

    ctx.free_bytes(p1);
    ctx.free_bytes(p2);
    ctx.free_bytes(p3);
    ctx.free_bytes(p4);

    printf("  PASSED\n");
}

// Test 5: Stats dump
TEST(StatsDump) {
    Cell::Config config;
    config.reserve_size = 64 * 1024 * 1024;

    Cell::Context ctx(config);

    // Make some allocations
    std::vector<void *> ptrs;
    for (int i = 0; i < 10; ++i) {
        ptrs.push_back(ctx.alloc_bytes(100, static_cast<uint8_t>(i)));
    }

    printf("\n");
    ctx.dump_stats();

    for (void *p : ptrs) {
        ctx.free_bytes(p);
    }

    printf("  PASSED\n");
}

// Test 6: Stats reset
TEST(StatsReset) {
    Cell::Config config;
    config.reserve_size = 64 * 1024 * 1024;

    Cell::Context ctx(config);
    const auto &stats = ctx.get_stats();

    // Make allocations
    void *p = ctx.alloc_bytes(1000, 0);
    assert(stats.total_allocated > 0);

    ctx.free_bytes(p);

    // Reset
    ctx.reset_stats();

    assert(stats.total_allocated == 0 && "Total should be reset");
    assert(stats.total_freed == 0 && "Freed should be reset");
    assert(stats.peak_allocated == 0 && "Peak should be reset");

    printf("  PASSED\n");
}

#else

// When stats are disabled, just report that
TEST(StatsDisabled) {
    printf("  CELL_ENABLE_STATS not defined, stats tests skipped\n");
    printf("  PASSED\n");
}

#endif

// =============================================================================
// Main
// =============================================================================

int main() {
    printf("Memory Statistics Tests\n");
    printf("=======================\n");
#ifdef CELL_ENABLE_STATS
    printf("CELL_ENABLE_STATS: ENABLED\n");
#else
    printf("CELL_ENABLE_STATS: DISABLED\n");
#endif
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
