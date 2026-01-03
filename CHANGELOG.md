# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0] - 2026-01-03

### Added

#### Multi-Tier Allocation System
- **Tier 1 (Cell)**: 16KB fixed-size block allocator with TLS cache and global pool
- **Tier 2 (Sub-Cell)**: Segregated size classes for 16B–8KB allocations across 10 bins
- **Tier 3 (Buddy)**: Binary buddy allocator for 32KB–2MB medium-large allocations
- **Tier 4 (Large)**: Direct OS allocation with huge page support for >2MB allocations

#### Performance Optimizations
- Lock-free TLS caches for cells and hot sub-cell sizes (16B–128B)
- Batch refill from global pools to amortize synchronization costs
- Memory decommit API (`decommit_unused()`) for releasing physical memory during idle

#### High-Level Abstractions
- `Arena`: Linear/bump allocator with O(1) allocations and bulk reset
- `Pool<T>`: Typed object pool with optional construction/destruction and batch operations
- `ArenaScope`: RAII guard for automatic arena marker restoration
- `StlAllocator<T>`: STL-compatible allocator for standard containers

#### Debug & Diagnostic Features (Compile-Time Optional)
- `CELL_DEBUG_GUARDS`: Guard bytes for buffer overflow/underflow detection
- `CELL_DEBUG_STACKTRACE`: Stack trace capture on allocation
- `CELL_DEBUG_LEAKS`: Leak detection and reporting at shutdown
- `CELL_ENABLE_STATS`: Memory statistics tracking (counts, sizes, peaks)
- `CELL_ENABLE_BUDGET`: Per-context memory budget limits with callbacks
- `CELL_ENABLE_INSTRUMENTATION`: Allocation/deallocation callbacks

#### Build System
- CMake 3.16+ build system with modular options
- Standalone tests (no GTest required) and GTest-based tests
- Google Benchmark integration for performance testing
- Cross-platform support: Linux, Windows, macOS

#### Documentation
- Comprehensive README with API reference
- Architecture diagrams
- Style guide for contributors

[Unreleased]: https://github.com/elfenlabs/cell/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/elfenlabs/cell/releases/tag/v0.1.0
