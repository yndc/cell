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

Layer 3 (Arena Allocator) is complete:
- Linear/bump allocation
- Cell chaining with auto-growth
- Marker-based nested scopes
- reset() and release() lifetime management

---

## Planned

### Layer 2 Optimization: TLS Caches for Size Bins

**Goal:** Reduce contention on hot size classes by adding thread-local caches.

**Features:**
- [ ] TLS cache for bins 0-3 (16B, 32B, 64B, 128B) — most common sizes
- [ ] Batch refill from global bin
- [ ] Flush on thread exit

---

### Layer 3: Additional Abstractions

#### Pool<T> (Typed Object Pool)
- [ ] Type-safe wrapper over sub-cell allocator
- [ ] Batch allocation support
- [ ] Optional constructor/destructor calls

#### Stack Allocator
- [ ] Scope-based temporary allocations
- [ ] Marker-based free (`free_to_marker()`)

---

### Large Allocations

- [ ] Multi-cell allocations (16KB - 2MB)
- [ ] Direct OS allocation path (2MB+)
- [ ] Large allocation registry for tracking

---

### Memory Statistics & Profiling

- [ ] Per-tag memory tracking
- [ ] `get_stats()` API
- [ ] Peak usage tracking
- [ ] `dump_stats()` for debugging

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

- [ ] Large page support (`MAP_HUGETLB` / `MEM_LARGE_PAGES`)
- [ ] Memory decommit for long-running games
- [ ] NUMA-aware allocation (future)

---

## Version Milestones

| Version | Features |
|---------|----------|
| 0.1.0   | Cell allocator (Layer 1) ✓ |
| 0.2.0   | Sub-cell allocator (Layer 2) ✓ |
| **0.3.0** | **Arena allocator ✓** ← Current |
| 0.4.0   | Pool<T> + Stack allocator |
| 0.5.0   | Memory stats & profiling |
| 0.6.0   | Large allocations |
| 1.0.0   | Production-ready with all debug features |

