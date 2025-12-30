# Cell Memory Library Roadmap

## Current Status

Layer 1 (Cell Allocator) is complete:
- Three-tier allocation: TLS → Global Pool → OS
- 16KB cells with lock-free global pool
- Superblock carving (2MB)
- Debug instrumentation (magic numbers, generation counters)

Layer 2 (Sub-Cell Allocator) is complete:
- 10 size class bins (16B to 8KB)
- Per-bin mutex locking
- Warm cell reserves per bin
- Debug memory poisoning

Layer 3 (High-Level Abstractions) is complete:
- Arena allocator with linear/bump allocation
- Pool\<T\> typed object pool
- ArenaScope RAII guard

Large Allocations complete:
- Buddy allocator for 32KB-2MB (power-of-2 splitting/coalescing)
- Direct OS allocation for >2MB (with optional huge pages)
- Automatic size routing in alloc_bytes()

Memory Statistics complete:
- Compile-time optional via `CELL_ENABLE_STATS`
- Global, per-allocator, and per-tag tracking
- Peak usage tracking
- `get_stats()`, `dump_stats()`, `reset_stats()` API

---

## Planned

### Layer 2 Optimization: TLS Caches for Size Bins

**Goal:** Reduce contention on hot size classes by adding thread-local caches.

**Features:**
- [ ] TLS cache for bins 0-3 (16B, 32B, 64B, 128B) — most common sizes
- [ ] Batch refill from global bin
- [ ] Flush on thread exit

---

### Debug Features

- [x] Magic numbers for corruption detection
- [x] Generation counters for stale references
- [x] Memory poisoning on free
- [ ] Bounds checking (guard bytes)
- [ ] Stack trace capture on allocation
- [ ] Leak detection report

---

### Platform Optimizations

- [x] Large page support (`MAP_HUGETLB` / `MEM_LARGE_PAGES`) for >2MB allocations
- [ ] Memory decommit for long-running games
- [ ] NUMA-aware allocation (future)

---

## Version Milestones

| Version | Features |
|---------|----------|
| 0.1.0   | Cell allocator (Layer 1) ✓ |
| 0.2.0   | Sub-cell allocator (Layer 2) ✓ |
| 0.3.0   | Arena allocator ✓ |
| 0.4.0   | Pool\<T\> + ArenaScope ✓ |
| 0.5.0   | Buddy + Large allocations ✓ |
| **0.6.0** | **Memory statistics ✓** ← Current |
| 1.0.0   | Production-ready with all debug features |
