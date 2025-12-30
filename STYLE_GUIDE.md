# Cell C++ Style Guide

This document defines the coding conventions and style guidelines for the Cell project. All contributions should adhere to these standards to maintain consistency.

## Table of Contents

- [Formatting](#formatting)
- [Naming Conventions](#naming-conventions)
- [File Organization](#file-organization)
- [Classes and Structs](#classes-and-structs)
- [Functions](#functions)
- [Comments and Documentation](#comments-and-documentation)
- [Modern C++ Features](#modern-c-features)
- [Concurrency](#concurrency)
- [Error Handling](#error-handling)

---

## Formatting

Use the provided `.clang-format` configuration for automatic formatting.

### Key Settings

| Setting | Value |
|---------|-------|
| Base Style | LLVM |
| Indent Width | 4 spaces |
| Tab Width | 4 spaces |
| Use Tabs | Never |
| Column Limit | 100 characters |
| Namespace Indentation | All |
| Access Modifier Offset | -4 |

### Braces

```cpp
// Opening brace on same line
if (condition) {
    // ...
}

// Single-line lambdas are acceptable
auto callback = [&]() { count++; };

// Multi-line lambdas
pool.enqueue([&]() {
    task_started = true;
    count++;
});
```

### Includes

Order includes in the following groups, separated by blank lines:

1. Corresponding header (for `.cpp` files)
2. Project headers (`"cell/..."`, `"..."`)
3. Standard library headers (`<atomic>`, `<vector>`, etc.)

```cpp
#include "cell/graph_builder.h"   // 1. Corresponding header

#include "cell/context.h"          // 2. Project headers
#include "thread_pool.h"

#include <algorithm>                  // 3. Standard library (alphabetical)
#include <map>
#include <stdexcept>
#include <vector>
```

---

## Naming Conventions

### Summary Table

| Entity | Convention | Example |
|--------|------------|---------|
| Classes/Structs | PascalCase | `GraphBuilder`, `NodeConfig` |
| Enums | PascalCase | `ExecutionMode`, `Access` |
| Enum Values | PascalCase | `CancelGraph`, `Sequential` |
| Functions/Methods | snake_case | `add_node()`, `run_async()` |
| Variables (local) | snake_case | `node_idx`, `task_count` |
| Member Variables | `m_` prefix + snake_case | `m_nodes`, `m_pool` |
| Thread-Local Variables | `t_` prefix + snake_case | `t_worker_index` |
| Compile-Time Constants | `k` prefix + PascalCase | `kTaskStorageSize` |
| Type Aliases | PascalCase | `NodeID`, `ResourceID`, `Task` |
| Namespaces | PascalCase | `Cell` |
| Template Parameters | Single uppercase or PascalCase | `T`, `F`, `U` |

### Member Variable Prefix: `m_`

All non-static member variables use the `m_` prefix:

```cpp
class Context {
private:
    std::map<std::string, ResourceID> m_name_to_id;
    std::map<ResourceID, std::string> m_id_to_name;
    mutable std::mutex m_mutex;
    ResourceID m_next_id = 0;
};
```

### Thread-Local Variable Prefix: `t_`

Thread-local storage variables use the `t_` prefix:

```cpp
inline thread_local int t_worker_index = -1;
```

### Compile-Time Constants Prefix: `k`

Use the `k` prefix for compile-time constants (constexpr, static const):

```cpp
static constexpr size_t kTaskStorageSize = 64;
```

### Type Aliases

Use `using` declarations (not `typedef`) with PascalCase names:

```cpp
using ResourceID = uint32_t;
using NodeID = uint32_t;
using Task = FixedFunction;
using ProfilerCallback = std::function<void(const char*, double)>;
```

---

## File Organization

### Directory Structure

```
include/cell/    # Public headers (API)
src/               # Implementation files (.cpp) and internal headers (.h)
tests/             # Test files (test_*.cpp)
benchmarks/        # Benchmark files (benchmark_*.cpp)
```

### Header Files

- Use `#pragma once` for include guards
- Public API headers go in `include/cell/`
- Internal/implementation headers go in `src/`
- Header-only implementations are acceptable for templates and inline code

```cpp
#pragma once
#include <dependency>

namespace Cell {
    // declarations
}
```

### Source Files

- Name pattern: `{component}.cpp` (e.g., `executor.cpp`, `graph_builder.cpp`)
- Test files: `test_{feature}.cpp` (e.g., `test_executor.cpp`, `test_concurrency.cpp`)
- Benchmark files: `benchmark_{component}.cpp`

---

## Classes and Structs

### Class Layout

Order class members as follows:

1. Public interface first
2. Private implementation last
3. Within each section: types, constructors, methods, members

```cpp
class Executor {
public:
    // Constructors
    Executor();
    explicit Executor(ThreadPool& pool);
    ~Executor();

    // Public methods
    void run(const ExecutionGraph& graph, ExecutionMode mode = ExecutionMode::Parallel);
    [[nodiscard]] AsyncHandle run_async(const ExecutionGraph& graph,
                                        ExecutionMode mode = ExecutionMode::Parallel);

    // Type aliases and callbacks
    using ProfilerCallback = std::function<void(const char*, double)>;
    void set_profiler_callback(ProfilerCallback callback);

private:
    // Private helper methods
    void run_task(...);
    void run_sequential(...);
    void run_parallel(...);

    // Member variables
    std::unique_ptr<ThreadPool> m_owned_pool;
    ThreadPool* m_pool;
    ProfilerCallback m_profiler_callback;
};
```

### Structs vs Classes

- Use `struct` for passive data aggregates (POD-like, all public)
- Use `class` for types with invariants or significant behavior

```cpp
// Struct: simple data container
struct Dependency {
    ResourceID id;
    Access access;
};

// Struct: configuration object
struct NodeConfig {
    std::string debug_name;
    std::function<void()> work_function;
    std::vector<Dependency> dependencies;
    int priority = 0;
    ErrorPolicy error_policy = ErrorPolicy::Continue;
};

// Class: has invariants and behavior
class Context {
public:
    ResourceID register_resource(const std::string& name);
    std::string get_name(ResourceID id) const;
private:
    // ...
};
```

### Deleted Operations

Explicitly delete copy operations when the class is non-copyable:

```cpp
class ThreadPool {
public:
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
};
```

### Explicit Constructors

Mark single-argument constructors `explicit` to prevent implicit conversions:

```cpp
explicit Executor(ThreadPool& pool);
explicit WorkStealingQueue(std::size_t capacity = 4096);
```

---

## Functions

### Parameters

- Pass fundamental types by value
- Pass non-trivial objects by `const&` for input
- Use `&&` for sink parameters (ownership transfer)
- Output parameters should generally be avoided; use return values instead

```cpp
// Input by const reference
void run(const ExecutionGraph& graph, ExecutionMode mode = ExecutionMode::Parallel);

// Ownership transfer (sink parameter)
void dispatch(std::vector<Task>&& tasks);

// Template forwarding reference
template <typename F>
void enqueue(F&& f);
```

### Return Types

- Use `[[nodiscard]]` for functions where ignoring the return value is likely a bug:

```cpp
[[nodiscard]] AsyncHandle run_async(const ExecutionGraph& graph, ...);
```

### Default Arguments

Place default arguments on the declaration (header), not the definition:

```cpp
// In header
void run(const ExecutionGraph& graph, ExecutionMode mode = ExecutionMode::Parallel);

// In source - no default
void Executor::run(const ExecutionGraph& graph, ExecutionMode mode) { ... }
```

---

## Comments and Documentation

### Doxygen Style

Use Doxygen `/** */` style for API documentation:

```cpp
/**
 * @brief A generic executor for processing ExecutionGraphs.
 *
 * The Executor takes a static ExecutionGraph and processes it dynamically using
 * the provided ThreadPool.
 */
class Executor {
public:
    /**
     * @brief Executes the given graph.
     *
     * This function blocks the calling thread until all tasks have completed.
     *
     * @param graph The dependency graph to execute.
     * @param mode Execution mode. Use Sequential for single-threaded debugging.
     */
    void run(const ExecutionGraph& graph, ExecutionMode mode = ExecutionMode::Parallel);
};
```

### Enum Documentation

Use `///<` for inline enum value documentation:

```cpp
enum class Access {
    READ, ///< Read-only access. Shared with other readers.
    WRITE ///< Exclusive write access. Blocks readers and other writers.
};
```

### Implementation Comments

Use `//` for implementation notes, especially for non-obvious code:

```cpp
// Chase–Lev requires a global order point between pop/steal.
std::atomic_thread_fence(std::memory_order_seq_cst);

// Batch-grab: take half of remaining tasks for local queue
// Cap at a reasonable maximum to avoid one worker hoarding everything
size_t to_grab = std::min(remaining / 2, size_t{64});
```

---

## Modern C++ Features

### C++ Standard

The project uses **C++20**. Leverage modern features appropriately.

### Smart Pointers

Prefer smart pointers over raw pointers for ownership:

```cpp
std::unique_ptr<ThreadPool> m_owned_pool;           // Single ownership
std::shared_ptr<AsyncHandle::State> m_state;        // Shared ownership
std::vector<std::unique_ptr<WorkQueue>> m_queues;   // Container of unique_ptr
```

### Auto

Use `auto` judiciously:

```cpp
// Good: type is obvious from context
auto& node = m_nodes[i];
auto graph = builder.bake();

// Good: complex types
auto it = topo_order.rbegin();

// Avoid: when type clarity matters
int count = 0;  // Not: auto count = 0;
```

### Range-Based For Loops

Prefer range-based for when iterating entire containers:

```cpp
for (const auto& dep : node.dependencies) { ... }
for (auto& q : m_queues) { ... }
for (int node_idx : graph.entry_nodes) { ... }
```

### std::optional

Use `std::optional` for values that may or may not be present:

```cpp
std::optional<Task> pop();
std::optional<Task> steal();

// Usage
if (auto task = queue.pop(); task.has_value()) {
    (*task)();
}
```

### constexpr

Use `constexpr` for compile-time constants:

```cpp
static constexpr size_t kTaskStorageSize = 64;
static constexpr int max_spins = 64;
```

### Lambdas

Use lambdas for callbacks and short inline functions:

```cpp
// Capture by reference for short-lived lambdas
pool.enqueue([&]() { counter++; });

// Capture by value when lambda outlives scope
pool.enqueue([=]() { run_recursive(dep_idx); });

// Mutable lambdas when needed
m_invoke = [](void* storage) { (*reinterpret_cast<F*>(storage))(); };
```

---

## Concurrency

### Atomic Operations

Use appropriate memory orderings. Document non-relaxed orderings:

```cpp
// Relaxed for simple counters
m_active_tasks.fetch_add(1, std::memory_order_relaxed);

// Acquire-release for synchronization
m_bottom.store(b + 1, std::memory_order_release);  // Publish write
int64_t t = m_top.load(std::memory_order_acquire); // Observe write

// Sequential consistency when required
std::atomic_thread_fence(std::memory_order_seq_cst);
```

### Thread Safety Documentation

Document thread safety requirements in comments:

```cpp
/**
 * @brief Registers a resource by name or retrieves its existing ID.
 *
 * This method is thread-safe.
 */
ResourceID register_resource(const std::string& name);

// Owner thread only (LIFO)
std::optional<T> pop();

// Thieves (FIFO)
std::optional<T> steal();
```

### Condition Variables

Always use with a predicate to handle spurious wakeups:

```cpp
m_cv_done.wait(lock, [this]() {
    return m_active_tasks.load(std::memory_order_relaxed) == 0;
});
```

### Cache Line Alignment

Use `alignas` to prevent false sharing in concurrent data structures:

```cpp
struct alignas(64) WorkQueue {
    WorkStealingQueue<Task> queue;
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> sleeping{false};
};

alignas(cacheline_size()) std::atomic<int64_t> m_top{0};
alignas(cacheline_size()) std::atomic<int64_t> m_bottom{0};
```

---

## Error Handling

### Exceptions

Use exceptions for exceptional conditions:

```cpp
if (processed_count != n) {
    std::stringstream ss;
    ss << "Cyclic dependency detected! Nodes involved: ";
    // ...
    throw std::runtime_error(ss.str());
}

if (m_stop.load(std::memory_order_relaxed))
    throw std::runtime_error("ThreadPool stopped");
```

### Exception Safety

Store exceptions for asynchronous operations:

```cpp
const std::vector<std::exception_ptr>& get_exceptions() const;

// In task execution
try {
    (*task_opt)();
} catch (...) {
    // Store exception for later retrieval
}
```

### Assertions

Use `assert` for invariants in debug builds:

```cpp
#ifndef NDEBUG
if (!m_buffer[idx].has_value()) {
    // This should never happen in a correct implementation
    return std::nullopt;
}
#endif

assert(task_opt.has_value() && "task_opt must have value before execution");
```

---

## Testing

### Test Naming

Use GoogleTest with descriptive test names:

```cpp
TEST(ExecutorTest, BasicExecution) { ... }
TEST(ExecutorTest, RunAsyncMultipleTasks) { ... }
TEST(ConcurrencyTest, StressRandomGraph) { ... }
TEST(AffinityTest, MixedAffinity) { ... }
```

### Test File Pattern

Name test files as `test_{feature}.cpp`:

- `test_executor.cpp`
- `test_concurrency.cpp`
- `test_error_handling.cpp`
- `test_affinity.cpp`

### Test Structure

```cpp
TEST(TestSuite, TestName) {
    // Arrange
    Cell::ThreadPool pool;
    Cell::Executor executor(pool);
    std::atomic<int> count{0};

    // Act
    Cell::ExecutionGraph graph;
    graph.nodes.push_back({[&]() { count++; }, {}, 0});
    graph.entry_nodes.push_back(0);
    executor.run(graph);

    // Assert
    EXPECT_EQ(count, 1);
}
```

---

## Build Configuration

### CMake

- Require C++20: `set(CMAKE_CXX_STANDARD 20)`
- Use `target_include_directories` with appropriate visibility
- Separate test and benchmark executables

---

## Quick Reference

```cpp
namespace Cell {                              // PascalCase namespace

    using NodeID = uint32_t;                    // PascalCase type alias

    enum class Access { READ, WRITE };          // PascalCase enum and values

    struct NodeConfig {                         // PascalCase struct
        std::string debug_name;                 // snake_case member (public struct)
        int priority = 0;
    };

    class Executor {                            // PascalCase class
    public:
        static constexpr int kVersion = 1;      // kPascalCase constant

        explicit Executor(ThreadPool& pool);    // explicit single-arg ctor
        void run(const ExecutionGraph& graph);  // snake_case method

    private:
        ThreadPool* m_pool;                     // m_ prefix member
    };

    inline thread_local int t_worker_index;     // t_ prefix thread-local

}
```
