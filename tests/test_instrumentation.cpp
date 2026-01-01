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
// Instrumentation Tests (only run when CELL_ENABLE_INSTRUMENTATION is defined)
// =============================================================================

#ifdef CELL_ENABLE_INSTRUMENTATION

// Global callback tracking
static int g_alloc_count = 0;
static int g_free_count = 0;
static void *g_last_ptr = nullptr;
static size_t g_last_size = 0;
static uint8_t g_last_tag = 0;
static bool g_last_is_alloc = false;

void reset_tracking() {
    g_alloc_count = 0;
    g_free_count = 0;
    g_last_ptr = nullptr;
    g_last_size = 0;
    g_last_tag = 0;
    g_last_is_alloc = false;
}

void alloc_callback(void *ptr, size_t size, uint8_t tag, bool is_alloc) {
    if (is_alloc) {
        g_alloc_count++;
    } else {
        g_free_count++;
    }
    g_last_ptr = ptr;
    g_last_size = size;
    g_last_tag = tag;
    g_last_is_alloc = is_alloc;
}

// Test 1: Basic callback invocation
TEST(CallbackInvocation) {
    Cell::Config config;
    config.reserve_size = 64 * 1024 * 1024;

    Cell::Context ctx(config);
    ctx.set_alloc_callback(alloc_callback);
    reset_tracking();

    // Allocate
    void *p = ctx.alloc_bytes(128, 42);
    assert(p != nullptr && "Allocation should succeed");
    assert(g_alloc_count == 1 && "Callback should be called once for alloc");
    assert(g_last_ptr == p && "Callback should receive correct pointer");
    assert(g_last_size == 128 && "Callback should receive correct size");
    assert(g_last_tag == 42 && "Callback should receive correct tag");
    assert(g_last_is_alloc == true && "Callback should indicate allocation");

    // Free
    ctx.free_bytes(p);
    assert(g_free_count == 1 && "Callback should be called once for free");
    assert(g_last_ptr == p && "Callback should receive correct pointer for free");
    assert(g_last_is_alloc == false && "Callback should indicate deallocation");

    printf("  PASSED\n");
}

// Test 2: Multiple allocations
TEST(MultipleAllocations) {
    Cell::Config config;
    config.reserve_size = 64 * 1024 * 1024;

    Cell::Context ctx(config);
    ctx.set_alloc_callback(alloc_callback);
    reset_tracking();

    std::vector<void *> ptrs;
    for (int i = 0; i < 10; ++i) {
        void *p = ctx.alloc_bytes(64);
        assert(p != nullptr);
        ptrs.push_back(p);
    }
    assert(g_alloc_count == 10 && "Should have 10 allocations");

    for (void *p : ptrs) {
        ctx.free_bytes(p);
    }
    assert(g_free_count == 10 && "Should have 10 frees");

    printf("  PASSED\n");
}

// Test 3: Null callback (no-op)
TEST(NullCallback) {
    Cell::Config config;
    config.reserve_size = 64 * 1024 * 1024;

    Cell::Context ctx(config);
    // Don't set callback - should be nullptr by default
    assert(ctx.get_alloc_callback() == nullptr && "Default callback should be null");

    // Should not crash
    void *p = ctx.alloc_bytes(128);
    assert(p != nullptr);
    ctx.free_bytes(p);

    printf("  PASSED\n");
}

// Test 4: Large allocation callback
TEST(LargeAllocationCallback) {
    Cell::Config config;
    config.reserve_size = 128 * 1024 * 1024;

    Cell::Context ctx(config);
    ctx.set_alloc_callback(alloc_callback);
    reset_tracking();

    // Buddy allocation (64KB)
    void *p1 = ctx.alloc_large(64 * 1024, 10);
    assert(p1 != nullptr && "Buddy allocation should succeed");
    assert(g_alloc_count == 1 && "Callback should be called for buddy alloc");
    assert(g_last_size == 64 * 1024 && "Size should match");
    assert(g_last_tag == 10 && "Tag should match");

    ctx.free_large(p1);
    assert(g_free_count == 1 && "Callback should be called for buddy free");

    printf("  PASSED\n");
}

// Test 5: Callback can be changed at runtime
TEST(RuntimeCallbackChange) {
    Cell::Config config;
    config.reserve_size = 64 * 1024 * 1024;

    Cell::Context ctx(config);
    reset_tracking();

    // No callback initially
    void *p1 = ctx.alloc_bytes(64);
    assert(g_alloc_count == 0 && "No callback yet");
    ctx.free_bytes(p1);

    // Set callback
    ctx.set_alloc_callback(alloc_callback);

    void *p2 = ctx.alloc_bytes(64);
    assert(g_alloc_count == 1 && "Callback now set");
    ctx.free_bytes(p2);
    assert(g_free_count == 1 && "Free callback called");

    // Clear callback
    ctx.set_alloc_callback(nullptr);
    reset_tracking();

    void *p3 = ctx.alloc_bytes(64);
    assert(g_alloc_count == 0 && "Callback cleared");
    ctx.free_bytes(p3);

    printf("  PASSED\n");
}

#else

// When instrumentation is disabled
TEST(InstrumentationDisabled) {
    printf("  CELL_ENABLE_INSTRUMENTATION not defined, tests skipped\n");
    printf("  PASSED\n");
}

#endif

// =============================================================================
// Main
// =============================================================================

int main() {
    printf("Instrumentation Callback Tests\n");
    printf("==============================\n");
#ifdef CELL_ENABLE_INSTRUMENTATION
    printf("CELL_ENABLE_INSTRUMENTATION: ENABLED\n");
#else
    printf("CELL_ENABLE_INSTRUMENTATION: DISABLED\n");
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
