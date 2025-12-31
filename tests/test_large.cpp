#include "cell/large.h"

#include <algorithm>
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
// Realloc Tests
// =============================================================================

TEST(ReallocGrowth) {
    Cell::LargeAllocRegistry registry;

    const size_t old_size = 3 * 1024 * 1024; // 3MB
    const size_t new_size = 5 * 1024 * 1024; // 5MB

    // Allocate initial block
    auto *ptr = static_cast<uint8_t *>(registry.alloc(old_size, 42));
    assert(ptr != nullptr);
    assert(registry.owns(ptr));

    // Fill with test pattern
    for (size_t i = 0; i < old_size; ++i) {
        ptr[i] = static_cast<uint8_t>(i & 0xFF);
    }

    // Realloc to larger size
    auto *new_ptr = static_cast<uint8_t *>(registry.realloc_bytes(ptr, new_size, 42));
    assert(new_ptr != nullptr);
    assert(registry.owns(new_ptr));
    assert(!registry.owns(ptr)); // Old pointer should be freed

    // Verify data was preserved
    for (size_t i = 0; i < old_size; ++i) {
        assert(new_ptr[i] == static_cast<uint8_t>(i & 0xFF));
    }

    // Cleanup
    registry.free(new_ptr);
    assert(registry.allocation_count() == 0);
    printf("  PASSED\n");
}

TEST(ReallocShrink) {
    Cell::LargeAllocRegistry registry;

    const size_t old_size = 5 * 1024 * 1024; // 5MB
    const size_t new_size = 3 * 1024 * 1024; // 3MB

    // Allocate initial block
    auto *ptr = static_cast<uint8_t *>(registry.alloc(old_size, 99));
    assert(ptr != nullptr);

    // Fill with test pattern
    for (size_t i = 0; i < old_size; ++i) {
        ptr[i] = static_cast<uint8_t>((i * 7) & 0xFF);
    }

    // Realloc to smaller size
    auto *new_ptr = static_cast<uint8_t *>(registry.realloc_bytes(ptr, new_size, 99));
    assert(new_ptr != nullptr);
    assert(registry.owns(new_ptr));

    // Verify data was preserved (up to new size)
    for (size_t i = 0; i < new_size; ++i) {
        assert(new_ptr[i] == static_cast<uint8_t>((i * 7) & 0xFF));
    }

    // Cleanup
    registry.free(new_ptr);
    assert(registry.allocation_count() == 0);
    printf("  PASSED\n");
}

TEST(ReallocNullPtr) {
    Cell::LargeAllocRegistry registry;

    const size_t size = 4 * 1024 * 1024; // 4MB

    // realloc_bytes(nullptr, size) should behave like alloc(size)
    void *ptr = registry.realloc_bytes(nullptr, size, 123);
    assert(ptr != nullptr);
    assert(registry.owns(ptr));
    assert(registry.allocation_count() == 1);

    // Cleanup
    registry.free(ptr);
    assert(registry.allocation_count() == 0);
    printf("  PASSED\n");
}

TEST(ReallocZeroSize) {
    Cell::LargeAllocRegistry registry;

    const size_t size = 3 * 1024 * 1024; // 3MB

    // Allocate a block
    void *ptr = registry.alloc(size, 5);
    assert(ptr != nullptr);
    assert(registry.allocation_count() == 1);

    // realloc_bytes(ptr, 0) should behave like free(ptr)
    void *result = registry.realloc_bytes(ptr, 0, 5);
    assert(result == nullptr);
    assert(registry.allocation_count() == 0);
    printf("  PASSED\n");
}

TEST(ReallocSameSize) {
    Cell::LargeAllocRegistry registry;

    const size_t size = 4 * 1024 * 1024; // 4MB

    // Allocate initial block
    auto *ptr = static_cast<uint8_t *>(registry.alloc(size, 77));
    assert(ptr != nullptr);

    // Fill with pattern
    for (size_t i = 0; i < 256; ++i) {
        ptr[i] = static_cast<uint8_t>(i);
    }

    // Realloc to same size
    auto *new_ptr = static_cast<uint8_t *>(registry.realloc_bytes(ptr, size, 77));
    assert(new_ptr != nullptr);
    assert(registry.owns(new_ptr));

    // Verify data preserved
    for (size_t i = 0; i < 256; ++i) {
        assert(new_ptr[i] == static_cast<uint8_t>(i));
    }

    // Cleanup
    registry.free(new_ptr);
    assert(registry.allocation_count() == 0);
    printf("  PASSED\n");
}

TEST(ReallocInvalidPtr) {
    Cell::LargeAllocRegistry registry;

    // Try to realloc a pointer not owned by the registry
    int dummy = 42;
    void *invalid_ptr = &dummy;

    void *result = registry.realloc_bytes(invalid_ptr, 4 * 1024 * 1024, 0);
    assert(result == nullptr); // Should fail gracefully
    printf("  PASSED\n");
}

TEST(ReallocAlignedAlloc) {
    Cell::LargeAllocRegistry registry;

    const size_t old_size = 3 * 1024 * 1024; // 3MB
    const size_t new_size = 6 * 1024 * 1024; // 6MB
    const size_t alignment = 1024 * 1024;    // 1MB alignment

    // Allocate aligned block
    auto *ptr = static_cast<uint8_t *>(registry.alloc_aligned(old_size, alignment, 88));
    assert(ptr != nullptr);
    assert(reinterpret_cast<uintptr_t>(ptr) % alignment == 0);

    // Fill with pattern
    for (size_t i = 0; i < old_size; ++i) {
        ptr[i] = static_cast<uint8_t>((i ^ 0xAA) & 0xFF);
    }

    // Realloc the aligned allocation
    auto *new_ptr = static_cast<uint8_t *>(registry.realloc_bytes(ptr, new_size, 88));
    assert(new_ptr != nullptr);
    assert(registry.owns(new_ptr));

    // Verify data preserved
    for (size_t i = 0; i < old_size; ++i) {
        assert(new_ptr[i] == static_cast<uint8_t>((i ^ 0xAA) & 0xFF));
    }

    // Cleanup
    registry.free(new_ptr);
    assert(registry.allocation_count() == 0);
    printf("  PASSED\n");
}

int main() {
    printf("Large Allocation Realloc Tests\n");
    printf("===============================\n\n");

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
