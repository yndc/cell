/**
 * @file test_debug.cpp
 * @brief Tests for Cell library debug features.
 *
 * Compile with CELL_DEBUG_GUARDS and CELL_DEBUG_LEAKS to test all features.
 */

#include <cell/context.h>

#include <cassert>
#include <cstdio>
#include <cstring>

using namespace Cell;

// ============================================================================
// Test Utilities
// ============================================================================

static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT(condition)                                                                     \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "FAILED: %s at %s:%d\n", #condition, __FILE__, __LINE__);         \
            g_tests_failed++;                                                                      \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define TEST_PASS()                                                                                \
    do {                                                                                           \
        g_tests_passed++;                                                                          \
    } while (0)

// ============================================================================
// Guard Bytes Tests
// ============================================================================

#ifdef CELL_DEBUG_GUARDS
void test_guards_valid_allocation() {
    std::printf("  test_guards_valid_allocation... ");

    Context ctx;

    void *ptr = ctx.alloc_bytes(64);
    TEST_ASSERT(ptr != nullptr);

    // Guards should be valid right after allocation
    TEST_ASSERT(ctx.check_guards(ptr));

    ctx.free_bytes(ptr);

    std::printf("OK\n");
    TEST_PASS();
}

void test_guards_multiple_allocations() {
    std::printf("  test_guards_multiple_allocations... ");

    Context ctx;

    void *p1 = ctx.alloc_bytes(32);
    void *p2 = ctx.alloc_bytes(128);
    void *p3 = ctx.alloc_bytes(256);

    TEST_ASSERT(p1 != nullptr);
    TEST_ASSERT(p2 != nullptr);
    TEST_ASSERT(p3 != nullptr);

    // All should have valid guards
    TEST_ASSERT(ctx.check_guards(p1));
    TEST_ASSERT(ctx.check_guards(p2));
    TEST_ASSERT(ctx.check_guards(p3));

    ctx.free_bytes(p1);
    ctx.free_bytes(p2);
    ctx.free_bytes(p3);

    std::printf("OK\n");
    TEST_PASS();
}
#endif

// ============================================================================
// Leak Detection Tests
// ============================================================================

#ifdef CELL_DEBUG_LEAKS
void test_leak_count_zero_initially() {
    std::printf("  test_leak_count_zero_initially... ");

    Context ctx;
    TEST_ASSERT(ctx.live_allocation_count() == 0);

    std::printf("OK\n");
    TEST_PASS();
}

void test_leak_count_tracks_allocations() {
    std::printf("  test_leak_count_tracks_allocations... ");

    Context ctx;

    void *p1 = ctx.alloc_bytes(32);
    TEST_ASSERT(ctx.live_allocation_count() == 1);

    void *p2 = ctx.alloc_bytes(64);
    TEST_ASSERT(ctx.live_allocation_count() == 2);

    void *p3 = ctx.alloc_bytes(128);
    TEST_ASSERT(ctx.live_allocation_count() == 3);

    ctx.free_bytes(p2);
    TEST_ASSERT(ctx.live_allocation_count() == 2);

    ctx.free_bytes(p1);
    ctx.free_bytes(p3);
    TEST_ASSERT(ctx.live_allocation_count() == 0);

    std::printf("OK\n");
    TEST_PASS();
}

void test_leak_count_different_sizes() {
    std::printf("  test_leak_count_different_sizes... ");

    Context ctx;

    // Test various allocation sizes
    void *small = ctx.alloc_bytes(16);     // Sub-cell bin 0
    void *medium = ctx.alloc_bytes(1024);  // Sub-cell bin 6
    void *large = ctx.alloc_bytes(10000);  // Full cell
    void *buddy = ctx.alloc_bytes(100000); // Buddy allocator

    TEST_ASSERT(ctx.live_allocation_count() == 4);

    ctx.free_bytes(small);
    ctx.free_bytes(medium);
    ctx.free_bytes(large);
    ctx.free_bytes(buddy);

    TEST_ASSERT(ctx.live_allocation_count() == 0);

    std::printf("OK\n");
    TEST_PASS();
}

void test_no_false_positives() {
    std::printf("  test_no_false_positives... ");

    // Create scope to ensure destructor runs
    {
        Context ctx;

        for (int i = 0; i < 100; i++) {
            void *ptr = ctx.alloc_bytes(64);
            ctx.free_bytes(ptr);
        }

        TEST_ASSERT(ctx.live_allocation_count() == 0);
    }

    std::printf("OK\n");
    TEST_PASS();
}
#endif

// ============================================================================
// Combined Tests (Guards + Leaks)
// ============================================================================

#if defined(CELL_DEBUG_GUARDS) && defined(CELL_DEBUG_LEAKS)
void test_guards_and_leaks_combined() {
    std::printf("  test_guards_and_leaks_combined... ");

    Context ctx;

    void *p1 = ctx.alloc_bytes(64);
    void *p2 = ctx.alloc_bytes(128);

    TEST_ASSERT(ctx.live_allocation_count() == 2);
    TEST_ASSERT(ctx.check_guards(p1));
    TEST_ASSERT(ctx.check_guards(p2));

    ctx.free_bytes(p1);
    TEST_ASSERT(ctx.live_allocation_count() == 1);

    ctx.free_bytes(p2);
    TEST_ASSERT(ctx.live_allocation_count() == 0);

    std::printf("OK\n");
    TEST_PASS();
}
#endif

// ============================================================================
// Tests that work without debug flags
// ============================================================================

void test_basic_alloc_free() {
    std::printf("  test_basic_alloc_free... ");

    Context ctx;

    void *ptr = ctx.alloc_bytes(64);
    TEST_ASSERT(ptr != nullptr);

    // Write to the memory to ensure it's usable
    std::memset(ptr, 0xAA, 64);

    ctx.free_bytes(ptr);

    std::printf("OK\n");
    TEST_PASS();
}

void test_various_sizes() {
    std::printf("  test_various_sizes... ");

    Context ctx;

    // Test various allocation sizes work correctly
    size_t sizes[] = {1, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 10000, 50000};

    for (size_t size : sizes) {
        void *ptr = ctx.alloc_bytes(size);
        TEST_ASSERT(ptr != nullptr);

        // Write to memory
        std::memset(ptr, 0x55, size);

        ctx.free_bytes(ptr);
    }

    std::printf("OK\n");
    TEST_PASS();
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::printf("=== Cell Debug Features Tests ===\n\n");

    // Print enabled features
    std::printf("Enabled features:\n");
#ifdef CELL_DEBUG_GUARDS
    std::printf("  - CELL_DEBUG_GUARDS\n");
#endif
#ifdef CELL_DEBUG_STACKTRACE
    std::printf("  - CELL_DEBUG_STACKTRACE\n");
#endif
#ifdef CELL_DEBUG_LEAKS
    std::printf("  - CELL_DEBUG_LEAKS\n");
#endif
#if !defined(CELL_DEBUG_GUARDS) && !defined(CELL_DEBUG_STACKTRACE) && !defined(CELL_DEBUG_LEAKS)
    std::printf("  (none - running basic tests only)\n");
#endif
    std::printf("\n");

    std::printf("Basic tests:\n");
    test_basic_alloc_free();
    test_various_sizes();

#ifdef CELL_DEBUG_GUARDS
    std::printf("\nGuard bytes tests:\n");
    test_guards_valid_allocation();
    test_guards_multiple_allocations();
#endif

#ifdef CELL_DEBUG_LEAKS
    std::printf("\nLeak detection tests:\n");
    test_leak_count_zero_initially();
    test_leak_count_tracks_allocations();
    test_leak_count_different_sizes();
    test_no_false_positives();
#endif

#if defined(CELL_DEBUG_GUARDS) && defined(CELL_DEBUG_LEAKS)
    std::printf("\nCombined tests:\n");
    test_guards_and_leaks_combined();
#endif

    std::printf("\n=== Results: %d passed, %d failed ===\n", g_tests_passed, g_tests_failed);

    return g_tests_failed > 0 ? 1 : 0;
}
