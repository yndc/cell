# Cell

A high-performance, cache-friendly C++ memory layer designed specifically for **Data-Oriented** applications such as **Entity Component Systems (ECS)**.

## Key Features

* **Configurable Uniform Cells:** Every memory unit is a fixed-size "Cell" (default 16KB, power-of-two). Cells are naturally aligned to their size, eliminating external fragmentation.
* **Lock-Free Thread Local Storage (TLS):** Each worker thread maintains a private cache of hot Cells to prevent mutex contention.
* **Dual-State Virtual Memory:** Reserves massive address space (GBs) and only commits physical RAM in Cell-sized or Large Page (2MB) increments.
* **Integrated Tagging:** Every Cell is tagged by subsystem for real-time memory profiling.

## Architecture

1. **Backend:** Direct syscalls to `VirtualAlloc` (Win) or `mmap` (Linux).
2. **Context (Class):** An RAII object representing a memory environment. Ownership of the virtual address range is tied to the object's lifetime.
3. **Global Pool:** A per-context thread-safe pool of committed but unused Cells.
4. **TLS Cache:** A per-thread cache that stores a small number of Cells for lock-free access within a context.

### Lifecycle (RAII)

```cpp
Cell::Context ctx(config);
```

* Constructor initializes the memory environment and reserves the address range.

```cpp
// Context is destroyed when it goes out of scope
```

* Destructor immediately releases all virtual and physical memory.

### Methods

```cpp
CellData* ctx.alloc(MemoryTag tag);
```

* Retrieves an aligned `CellData` from the instance's pool.
* Complexity: O(1) (TLS) or O(1) (Global with lock).

```cpp
void ctx.free(CellData* cell);
```

* Returns the `CellData` to the instance's local or global pool.

### Metadata Access

```cpp
CellHeader* Cell::get_header(void* ptr);
```

* Performs a constant-time alignment mask to locate the `CellHeader` for any pointer within a Cell.

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
