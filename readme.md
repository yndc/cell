# Cell

A high-performance, cache-friendly C++ memory library for **data-oriented applications**.

## Features

### Multi-Layer Allocation

| Layer | Size Range | Strategy |
|-------|------------|----------|
| 1 - Cell | 16KB | TLS → Global Pool → OS |
| 2 - Sub-Cell | 16B - 8KB | Segregated size classes (10 bins) |
| 3 - Buddy | 32KB - 2MB | Power-of-2 splitting/coalescing |
| 4 - Large | >2MB | Direct OS (with huge page support) |

### Performance Optimizations

- **Lock-free TLS caches** for cells and hot sub-cell sizes (16B-128B)
- **Batch refill** from global pools to amortize lock costs
- **Memory decommit** API for long-running applications

### High-Level Abstractions

- `Arena` — Linear/bump allocator with bulk reset
- `Pool<T>` — Typed object pool with construct/destruct
- `ArenaScope` — RAII guard for automatic arena reset

### Debug Features

- Magic numbers for corruption detection
- Generation counters for stale reference detection
- Memory poisoning on free
- Optional memory statistics (`CELL_ENABLE_STATS`)

## Quick Start

```cpp
#include <cell/context.h>

int main() {
    Cell::Context ctx;

    // Sub-cell allocation (auto size-routed)
    int* data = ctx.alloc<int>();
    ctx.free_bytes(data);

    // Arena for temporary allocations
    Cell::Arena arena(ctx);
    void* temp = arena.alloc(1024);
    arena.reset();  // Bulk free

    // Memory management for long-running apps
    ctx.decommit_unused();  // Release unused physical memory
}
```

## Building

```bash
mkdir build && cd build
cmake ..
cmake --build .
```

## Running Tests

```bash
cd build
ctest --output-on-failure
```
