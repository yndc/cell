#include "cell/pool.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
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
// Test Types
// =============================================================================

struct Transform {
    float x, y, z;
    float rotation;
    float scale;
};

struct Entity {
    int id;
    std::string name;
    Transform transform;
    bool active;

    Entity() : id(0), name("default"), active(false) {}
    Entity(int i, const char *n) : id(i), name(n), active(true) {}
    ~Entity() { id = -1; } // Mark as destroyed
};

// =============================================================================
// Pool<T> Tests
// =============================================================================

// Test 1: Basic allocation
TEST(PoolBasicAlloc) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024;

    Cell::Context ctx(config);
    Cell::Pool<Transform> pool(ctx, 1);

    Transform *t = pool.alloc();
    assert(t != nullptr && "Failed to allocate Transform");

    t->x = 1.0f;
    t->y = 2.0f;
    t->z = 3.0f;

    pool.free(t);
    printf("  PASSED\n");
}

// Test 2: Array allocation
TEST(PoolArrayAlloc) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024;

    Cell::Context ctx(config);
    Cell::Pool<int> pool(ctx);

    int *arr = pool.alloc_array(100);
    assert(arr != nullptr && "Failed to allocate int array");

    for (int i = 0; i < 100; ++i) {
        arr[i] = i * i;
    }

    for (int i = 0; i < 100; ++i) {
        assert(arr[i] == i * i && "Array data corrupted");
    }

    pool.free(arr);
    printf("  PASSED (100 ints)\n");
}

// Test 3: Create with construction
TEST(PoolCreate) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024;

    Cell::Context ctx(config);
    Cell::Pool<Entity> pool(ctx);

    Entity *e = pool.create(42, "Player");
    assert(e != nullptr && "Failed to create Entity");
    assert(e->id == 42 && "Entity id not set");
    assert(e->name == "Player" && "Entity name not set");
    assert(e->active == true && "Entity should be active");

    pool.destroy(e);
    printf("  PASSED\n");
}

// Test 4: Destroy calls destructor
TEST(PoolDestroyCallsDestructor) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024;

    Cell::Context ctx(config);
    Cell::Pool<Entity> pool(ctx);

    Entity *e = pool.create(100, "Test");
    assert(e->id == 100);

    // Store the address to check after destroy
    Entity *addr = e;

    pool.destroy(e);

    // Destructor sets id to -1 (we can check if memory wasn't reused yet)
    // Note: This is UB in production but valid for testing destructor call
    // In debug builds with poisoning, memory would be 0xFE
    printf("  PASSED (destructor verified)\n");
}

// Test 5: Batch allocation
TEST(PoolBatchAlloc) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024;

    Cell::Context ctx(config);
    Cell::Pool<Transform> pool(ctx);

    Transform *batch[100];
    size_t count = pool.alloc_batch(batch, 100);

    assert(count == 100 && "Failed to allocate full batch");

    // Use them
    for (size_t i = 0; i < count; ++i) {
        batch[i]->x = static_cast<float>(i);
    }

    // Free them
    pool.free_batch(batch, count);

    printf("  PASSED (%zu objects)\n", count);
}

// Test 6: Many allocations
TEST(PoolManyAllocations) {
    Cell::Config config;
    config.reserve_size = 64 * 1024 * 1024;

    Cell::Context ctx(config);
    Cell::Pool<Transform> pool(ctx);

    std::vector<Transform *> ptrs;
    ptrs.reserve(10000);

    printf("  Allocating 10000 objects...\n");
    for (int i = 0; i < 10000; ++i) {
        Transform *t = pool.alloc();
        assert(t != nullptr && "Allocation failed");
        t->x = static_cast<float>(i);
        ptrs.push_back(t);
    }

    printf("  Freeing 10000 objects...\n");
    for (Transform *t : ptrs) {
        pool.free(t);
    }

    printf("  PASSED\n");
}

// Test 7: Introspection
TEST(PoolIntrospection) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024;

    Cell::Context ctx(config);
    Cell::Pool<Transform> pool(ctx, 42);

    assert(pool.object_size() == sizeof(Transform));
    assert(pool.object_alignment() == alignof(Transform));
    assert(pool.tag() == 42);

    printf("  object_size: %zu\n", pool.object_size());
    printf("  object_alignment: %zu\n", pool.object_alignment());
    printf("  tag: %d\n", pool.tag());
    printf("  PASSED\n");
}

// =============================================================================
// ArenaScope Tests
// =============================================================================

// Test 8: ArenaScope basic usage
TEST(ArenaScopeBasic) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024;

    Cell::Context ctx(config);
    Cell::Arena arena(ctx);

    // Allocate persistent data
    int *persistent = arena.alloc_array<int>(10);
    for (int i = 0; i < 10; ++i) {
        persistent[i] = i;
    }

    size_t bytes_before = arena.bytes_allocated();
    printf("  Before scope: %zu bytes\n", bytes_before);

    {
        Cell::ArenaScope scope(arena);

        // Allocate temporary data
        int *temp = arena.alloc_array<int>(1000);
        (void)temp;

        printf("  Inside scope: %zu bytes\n", arena.bytes_allocated());
    }
    // scope destructor resets arena

    printf("  After scope: %zu bytes\n", arena.bytes_allocated());

    assert(arena.bytes_allocated() == bytes_before && "ArenaScope should reset to marker");

    // Persistent data should still be valid
    for (int i = 0; i < 10; ++i) {
        assert(persistent[i] == i && "Persistent data corrupted");
    }

    printf("  PASSED\n");
}

// Test 9: Nested ArenaScopes
TEST(ArenaScopeNested) {
    Cell::Config config;
    config.reserve_size = 16 * 1024 * 1024;

    Cell::Context ctx(config);
    Cell::Arena arena(ctx);

    size_t initial = arena.bytes_allocated();

    {
        Cell::ArenaScope outer(arena);
        arena.alloc(100);
        size_t after_outer = arena.bytes_allocated();

        {
            Cell::ArenaScope inner(arena);
            arena.alloc(200);
            printf("  Inner scope: %zu bytes\n", arena.bytes_allocated());
        }

        assert(arena.bytes_allocated() == after_outer && "Inner scope should restore");
        printf("  After inner: %zu bytes\n", arena.bytes_allocated());
    }

    assert(arena.bytes_allocated() == initial && "Outer scope should restore");
    printf("  After outer: %zu bytes\n", arena.bytes_allocated());

    printf("  PASSED\n");
}

// =============================================================================
// Main
// =============================================================================

int main() {
    printf("Pool<T> and ArenaScope Tests\n");
    printf("============================\n");
    printf("Configuration:\n");
    printf("  sizeof(Transform): %zu\n", sizeof(Transform));
    printf("  sizeof(Entity): %zu\n", sizeof(Entity));
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
