#include "cell/context.h"

#include "tls_bin_cache.h"

#include <cassert>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace Cell {

    Context::Context(const Config &config) : m_reserved_size(config.reserve_size) {
        // Split reserved space: half for cells, half for buddy
        // Both need to be reasonably sized for their use cases
        size_t cell_reserve = m_reserved_size / 2;
        size_t buddy_reserve = m_reserved_size / 2;

        // Round down to superblock alignment for cell region
        cell_reserve = (cell_reserve / kSuperblockSize) * kSuperblockSize;
        // Round down to 2MB alignment for buddy region
        buddy_reserve =
            (buddy_reserve / BuddyAllocator::kMaxBlockSize) * BuddyAllocator::kMaxBlockSize;

#if defined(_WIN32)
        m_base = VirtualAlloc(nullptr, cell_reserve, MEM_RESERVE, PAGE_NOACCESS);
        if (m_base) {
            m_buddy_base = VirtualAlloc(nullptr, buddy_reserve, MEM_RESERVE, PAGE_NOACCESS);
        }
#else
        m_base = mmap(nullptr, cell_reserve, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                      -1, 0);
        if (m_base == MAP_FAILED) {
            m_base = nullptr;
        }
        if (m_base) {
            m_buddy_base = mmap(nullptr, buddy_reserve, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (m_buddy_base == MAP_FAILED) {
                m_buddy_base = nullptr;
            }
        }
#endif

        if (m_base) {
            m_reserved_size = cell_reserve;
            m_allocator = std::make_unique<Allocator>(m_base, cell_reserve);
        }

        if (m_buddy_base) {
            m_buddy_reserved_size = buddy_reserve;
            m_buddy = std::make_unique<BuddyAllocator>(m_buddy_base, buddy_reserve);
        }

        // Initialize bins (already zero-initialized, but be explicit)
        for (size_t i = 0; i < kNumSizeBins; ++i) {
            m_bins[i].partial_head = nullptr;
            m_bins[i].warm_cell_count = 0;
            m_bins[i].total_allocated = 0;
            m_bins[i].current_allocated = 0;
        }
    }

    Context::~Context() {
#ifdef CELL_DEBUG_LEAKS
        // Report any leaked allocations before cleanup
        if (!m_live_allocs.empty()) {
            std::fprintf(stderr, "\n[CELL] WARNING: %zu allocation(s) leaked:\n",
                         m_live_allocs.size());
            report_leaks();
        }
#endif

        // Clear TLS bin caches to prevent stale pointers for future Contexts.
        // The cached blocks will be freed when the memory region is unmapped.
        // Note: This only clears the current thread's caches.
        for (size_t i = 0; i < kTlsBinCacheCount; ++i) {
            t_bin_cache[i].count = 0;
        }

        // Buddy allocator destructor handles its cleanup
        m_buddy.reset();
        m_allocator.reset();

        if (m_base) {
#if defined(_WIN32)
            VirtualFree(m_base, 0, MEM_RELEASE);
#else
            munmap(m_base, m_reserved_size);
#endif
        }
        if (m_buddy_base) {
#if defined(_WIN32)
            VirtualFree(m_buddy_base, 0, MEM_RELEASE);
#else
            munmap(m_buddy_base, m_buddy_reserved_size);
#endif
        }
    }

    // =========================================================================
    // Sub-Cell Allocation API
    // =========================================================================

    void *Context::alloc_bytes(size_t size, uint8_t tag, size_t alignment) {
        if (size == 0) {
            return nullptr;
        }

        // Size routing:
        // <= 8KB: sub-cell bins
        // <= 16KB (usable cell space): full cell
        // <= 2MB: buddy allocator
        // > 2MB: direct OS (large allocation)

        size_t usable_cell_size = kCellSize - kBlockStartOffset;
        void *result = nullptr;

#ifdef CELL_DEBUG_GUARDS
        // For sub-cell allocations that fit with guards, add space for guard bytes
        size_t alloc_size = size;
        bool will_have_guards = false;
        if (size + (2 * kGuardSize) <= kMaxSubCellSize) {
            alloc_size = size + (2 * kGuardSize);
            will_have_guards = true;
        }
#else
        size_t alloc_size = size;
#endif

        if (alloc_size <= kMaxSubCellSize) {
            // Sub-cell allocation
            if (!m_allocator)
                return nullptr;
            uint8_t bin_index = get_size_class(alloc_size, alignment);
            if (bin_index == kFullCellMarker) {
                // Rare edge case: alignment pushes us to full cell
                CellData *cell = alloc_cell(tag);
                if (cell) {
                    cell->header.size_class = kFullCellMarker;
                    // Return pointer to usable area, not the header
                    result = get_block_start(&cell->header);
                }
#ifdef CELL_DEBUG_GUARDS
                will_have_guards = false; // Full cell, no guards
#endif
#ifdef CELL_ENABLE_STATS
                if (result) {
                    m_stats.record_alloc(kCellSize, tag);
                    m_stats.cell_allocs.fetch_add(1, std::memory_order_relaxed);
                }
#endif
            } else {
                result = alloc_from_bin(bin_index, tag);
#ifdef CELL_ENABLE_STATS
                if (result) {
                    m_stats.record_alloc(kSizeClasses[bin_index], tag);
                    m_stats.subcell_allocs.fetch_add(1, std::memory_order_relaxed);
                }
#endif
            }
        } else if (size <= usable_cell_size) {
            // Full cell allocation (up to ~16KB)
            if (!m_allocator)
                return nullptr;
            CellData *cell = alloc_cell(tag);
            if (cell) {
                cell->header.size_class = kFullCellMarker;
                // Return pointer to usable area, not the header
                result = get_block_start(&cell->header);
            }
#ifdef CELL_DEBUG_GUARDS
            will_have_guards = false;
#endif
#ifdef CELL_ENABLE_STATS
            if (result) {
                m_stats.record_alloc(kCellSize, tag);
                m_stats.cell_allocs.fetch_add(1, std::memory_order_relaxed);
            }
#endif
        } else {
            // Large allocation (buddy or direct OS)
            result = alloc_large(size, tag);
#ifdef CELL_DEBUG_GUARDS
            will_have_guards = false;
#endif
            // Stats tracking handled in alloc_large
        }

        if (!result) {
            return nullptr;
        }

#ifdef CELL_DEBUG_GUARDS
        // Only apply guards when we allocated extra space for them
        if (will_have_guards) {
            // Fill front guard bytes
            auto *guard_ptr = static_cast<uint8_t *>(result);
            std::memset(guard_ptr, kGuardPattern, kGuardSize);

            // Calculate user pointer (after front guard)
            void *user_ptr = guard_ptr + kGuardSize;

            // Fill back guard bytes
            std::memset(static_cast<uint8_t *>(user_ptr) + size, kGuardPattern, kGuardSize);

            result = user_ptr;
        }
#endif

#ifdef CELL_DEBUG_LEAKS
        // Track this allocation
        {
            std::lock_guard<std::mutex> lock(m_debug_mutex);
            DebugAllocation alloc{};
            alloc.ptr = result;
            alloc.size = size;
            alloc.tag = tag;
#ifdef CELL_DEBUG_STACKTRACE
            alloc.stack_depth = capture_stack(alloc.stack, kMaxStackDepth, 2);
#endif
            m_live_allocs[result] = alloc;
        }
#endif

        return result;
    }

    void Context::free_bytes(void *ptr) {
        if (!ptr) {
            return;
        }

#ifdef CELL_DEBUG_LEAKS
        // Remove from tracking and get allocation size
        size_t alloc_size = 0;
        {
            std::lock_guard<std::mutex> lock(m_debug_mutex);
            auto it = m_live_allocs.find(ptr);
            if (it != m_live_allocs.end()) {
                alloc_size = it->second.size;
                m_live_allocs.erase(it);
            }
        }
#endif

        // First, check for buddy and large allocations (no guards applied to these)
        // For these, use the pointer as-is without guard adjustment
        if (m_buddy && m_buddy->owns(ptr)) {
#ifdef CELL_ENABLE_STATS
            m_stats.buddy_frees.fetch_add(1, std::memory_order_relaxed);
#endif
            m_buddy->free(ptr);
            return;
        }

        if (m_large_allocs.owns(ptr)) {
#ifdef CELL_ENABLE_STATS
            m_stats.large_frees.fetch_add(1, std::memory_order_relaxed);
#endif
            m_large_allocs.free(ptr);
            return;
        }

        // Must be cell/sub-cell allocation
        // For sub-cell (bin) allocations with small enough sizes, guards were applied
        // For full-cell allocations or large sub-cell allocations, no guards

#if defined(CELL_DEBUG_GUARDS) && defined(CELL_DEBUG_LEAKS)
        // Determine if guards were applied based on allocation size
        // Guards are only applied when: size + 2*kGuardSize <= kMaxSubCellSize
        // Since alloc_size stores the original requested size, check that
        bool has_guards = (alloc_size > 0 && (alloc_size + 2 * kGuardSize) <= kMaxSubCellSize);

        if (has_guards) {
            auto *user_ptr = static_cast<uint8_t *>(ptr);
            auto *front_guard = user_ptr - kGuardSize;

            // Validate front guard
            for (size_t i = 0; i < kGuardSize; ++i) {
                if (front_guard[i] != kGuardPattern) {
                    std::fprintf(
                        stderr,
                        "[CELL] ERROR: Front guard corrupted at offset %zu (expected 0x%02X, got "
                        "0x%02X)\n",
                        i, kGuardPattern, front_guard[i]);
                    assert(false && "Memory corruption: front guard bytes overwritten");
                }
            }

            // Validate back guard
            auto *back_guard = user_ptr + alloc_size;
            for (size_t i = 0; i < kGuardSize; ++i) {
                if (back_guard[i] != kGuardPattern) {
                    std::fprintf(
                        stderr,
                        "[CELL] ERROR: Back guard corrupted at offset %zu (expected 0x%02X, got "
                        "0x%02X)\n",
                        i, kGuardPattern, back_guard[i]);
                    assert(false && "Memory corruption: back guard bytes overwritten");
                }
            }

            // Adjust pointer to original allocation
            ptr = front_guard;
        }
#elif !defined(CELL_DEBUG_LEAKS)
        // Without leak tracking, we can't know the original size, so skip guard checking
        (void)alloc_size;
#endif

        CellHeader *header = get_header(ptr);
        uint8_t tag = header->tag;

        if (header->size_class == kFullCellMarker) {
            // Full-cell allocation
#ifdef CELL_ENABLE_STATS
            m_stats.record_free(kCellSize, tag);
            m_stats.cell_frees.fetch_add(1, std::memory_order_relaxed);
#endif
            free_cell(reinterpret_cast<CellData *>(header));
        } else {
            // Sub-cell allocation
#ifdef CELL_ENABLE_STATS
            size_t block_size = kSizeClasses[header->size_class];
            m_stats.record_free(block_size, tag);
            m_stats.subcell_frees.fetch_add(1, std::memory_order_relaxed);
#endif
            free_to_bin(ptr, header);
        }
    }

    void *Context::realloc_bytes(void *ptr, size_t new_size, uint8_t tag) {
        // Edge case: nullptr -> behaves like alloc
        if (!ptr) {
            return alloc_bytes(new_size, tag);
        }

        // Edge case: zero size -> behaves like free
        if (new_size == 0) {
            free_bytes(ptr);
            return nullptr;
        }

        // Check buddy tier first
        if (m_buddy && m_buddy->owns(ptr)) {
            // For buddy allocations, check if new size still fits in buddy range
            if (new_size <= BuddyAllocator::kMaxBlockSize &&
                new_size >= BuddyAllocator::kMinBlockSize) {
                // Stay in buddy tier
                return m_buddy->realloc_bytes(ptr, new_size);
            }
            // Cross-tier: buddy -> somewhere else
            // Get old size from buddy header
            // Buddy stores order in header, block size = 2^order
            // For now, use allocate+copy+free with conservative size estimate
            void *new_ptr = alloc_bytes(new_size, tag);
            if (!new_ptr)
                return nullptr;
            // Copy up to the smaller of old block size or new size
            // Buddy min is 32KB, so copy at most new_size
            std::memcpy(new_ptr, ptr, new_size);
            m_buddy->free(ptr);
            return new_ptr;
        }

        // Check large tier
        if (m_large_allocs.owns(ptr)) {
            // For large allocations, check if new size still needs large
            if (new_size > BuddyAllocator::kMaxBlockSize) {
                // Stay in large tier
                return m_large_allocs.realloc_bytes(ptr, new_size, tag);
            }
            // Cross-tier: large -> smaller tier
            void *new_ptr = alloc_bytes(new_size, tag);
            if (!new_ptr)
                return nullptr;
            std::memcpy(new_ptr, ptr, new_size);
            m_large_allocs.free(ptr);
            return new_ptr;
        }

        // Must be cell/sub-cell allocation
        CellHeader *header = get_header(ptr);
        size_t old_size;

        if (header->size_class == kFullCellMarker) {
            // Full cell allocation
            old_size = kCellSize - kBlockStartOffset;
        } else {
            // Sub-cell allocation
            old_size = kSizeClasses[header->size_class];

            // Same-bin optimization: if new size fits in same bin, return same pointer
            // Must account for guards if enabled, to match what alloc_bytes does
#ifdef CELL_DEBUG_GUARDS
            size_t alloc_size = new_size;
            if (new_size + (2 * kGuardSize) <= kMaxSubCellSize) {
                alloc_size = new_size + (2 * kGuardSize);
            }
            uint8_t new_bin = get_size_class(alloc_size, 8);
#else
            uint8_t new_bin = get_size_class(new_size, 8);
#endif
            if (new_bin != kFullCellMarker && new_bin == header->size_class) {
                return ptr; // Fits in same bin, no reallocation needed
            }
        }

        // Fallback: allocate new block, copy data, free old block
        void *new_ptr = alloc_bytes(new_size, tag);
        if (!new_ptr) {
            return nullptr; // Allocation failed, old block unchanged
        }

        std::memcpy(new_ptr, ptr, std::min(old_size, new_size));
        free_bytes(ptr);

        return new_ptr;
    }

    // =========================================================================
    // Large Allocation API
    // =========================================================================

    void *Context::alloc_large(size_t size, uint8_t tag, bool try_huge_pages) {
        if (size == 0) {
            return nullptr;
        }

        void *result = nullptr;

        // Route: <= 2MB to buddy, > 2MB to direct OS
        if (size <= BuddyAllocator::kMaxBlockSize) {
            if (m_buddy) {
                result = m_buddy->alloc(size);
#ifdef CELL_ENABLE_STATS
                if (result) {
                    // Buddy rounds up to power-of-2
                    m_stats.record_alloc(size, tag);
                    m_stats.buddy_allocs.fetch_add(1, std::memory_order_relaxed);
                }
#endif
                return result;
            }
            // Fallback to large alloc if buddy not initialized
        }

        // Direct OS allocation for > 2MB
        result = m_large_allocs.alloc(size, tag, try_huge_pages);
#ifdef CELL_ENABLE_STATS
        if (result) {
            m_stats.record_alloc(size, tag);
            m_stats.large_allocs.fetch_add(1, std::memory_order_relaxed);
        }
#endif
        return result;
    }

    void Context::free_large(void *ptr) {
        if (!ptr)
            return;

        if (m_buddy && m_buddy->owns(ptr)) {
#ifdef CELL_ENABLE_STATS
            m_stats.buddy_frees.fetch_add(1, std::memory_order_relaxed);
#endif
            m_buddy->free(ptr);
        } else {
#ifdef CELL_ENABLE_STATS
            m_stats.large_frees.fetch_add(1, std::memory_order_relaxed);
#endif
            m_large_allocs.free(ptr);
        }
    }

    void *Context::alloc_aligned(size_t size, size_t alignment, uint8_t tag) {
        if (size == 0) {
            return nullptr;
        }

        // Validate alignment is power of 2
        if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
            return nullptr;
        }

        // For buddy allocations: check if natural power-of-2 alignment is sufficient
        if (size <= BuddyAllocator::kMaxBlockSize && m_buddy) {
            // Calculate the order (and thus natural alignment) for this size
            // Account for buddy header (8 bytes)
            size_t total_size = size + 8;
            if (total_size < BuddyAllocator::kMinBlockSize) {
                total_size = BuddyAllocator::kMinBlockSize;
            }

            // Round up to next power of 2 to find actual block size
            size_t block_size = BuddyAllocator::kMinBlockSize;
            while (block_size < total_size && block_size < BuddyAllocator::kMaxBlockSize) {
                block_size <<= 1;
            }

            // Buddy blocks are naturally aligned to their size.
            // The user pointer is offset by 8 bytes (header), so actual alignment
            // is min(block_size, block_alignment_after_8_byte_offset).
            // For power-of-2 blocks >= 32KB aligned, offset by 8 is still 8-byte aligned
            // but the overall block is block_size aligned.
            //
            // If requested alignment <= block_size and block_size >= alignment,
            // we can use buddy. Otherwise, use LargeAllocRegistry.
            if (alignment <= block_size) {
                void *result = m_buddy->alloc(size);
#ifdef CELL_ENABLE_STATS
                if (result) {
                    m_stats.record_alloc(size, tag);
                    m_stats.buddy_allocs.fetch_add(1, std::memory_order_relaxed);
                }
#endif
                return result;
            }
            // For higher alignment requirements, fall through to LargeAllocRegistry
        }

        // Use LargeAllocRegistry for:
        // - Sizes > 2MB
        // - Alignments exceeding buddy's natural block alignment
        void *result = m_large_allocs.alloc_aligned(size, alignment, tag);
#ifdef CELL_ENABLE_STATS
        if (result) {
            m_stats.record_alloc(size, tag);
            m_stats.large_allocs.fetch_add(1, std::memory_order_relaxed);
        }
#endif
        return result;
    }

    // =========================================================================
    // Cell-Level API
    // =========================================================================

    CellData *Context::alloc_cell(uint8_t tag) {
        if (!m_allocator) {
            return nullptr;
        }

        void *ptr = m_allocator->alloc();
        if (!ptr) {
            return nullptr;
        }

        auto *cell = static_cast<CellData *>(ptr);
        cell->header.tag = tag;
        cell->header.size_class = kFullCellMarker;
        cell->header.free_count = 0;
        return cell;
    }

    void Context::free_cell(CellData *cell) {
        if (m_allocator && cell) {
            m_allocator->free(cell);
        }
    }

    // =========================================================================
    // Memory Management API
    // =========================================================================

    size_t Context::decommit_unused() {
        size_t total = 0;

        // Flush TLS caches first to get accurate free counts
        if (m_allocator) {
            m_allocator->flush_tls_cache();
            total += m_allocator->decommit_unused();
        }

        // Could also add buddy allocator decommit here in the future

        return total;
    }

    size_t Context::committed_bytes() const {
        size_t total = 0;
        if (m_allocator) {
            total += m_allocator->committed_bytes();
        }
        // Could add buddy allocator committed bytes here
        return total;
    }

    // =========================================================================
    // Sub-Cell Implementation
    // =========================================================================

    void *Context::alloc_from_bin(size_t bin_index, uint8_t tag) {
        assert(bin_index < kNumSizeBins);

        // TLS fast path for hot bins (0-3: 16B, 32B, 64B, 128B)
        if (bin_index < kTlsBinCacheCount) {
            TlsBinCache &cache = t_bin_cache[bin_index];

            // Try TLS cache first (no lock)
            if (!cache.is_empty()) {
                return cache.pop();
            }

            // Try batch refill from global bin
            batch_refill_tls_bin(bin_index, tag);
            if (!cache.is_empty()) {
                return cache.pop();
            }
        }

        // Fallback: lock-based allocation from global bin
        std::lock_guard<std::mutex> lock(m_bin_locks[bin_index]);
        SizeBin &bin = m_bins[bin_index];

        // Try to allocate from a partial cell
        if (bin.partial_head) {
            CellHeader *cell_header = bin.partial_head;
            CellMetadata *metadata = get_metadata(cell_header);

            // Pop a block from the free list
            FreeBlock *block = metadata->free_list;
            assert(block && "Partial cell should have free blocks");
            metadata->free_list = block->next;
            cell_header->free_count--;

            // If cell is now full, remove from partial list
            if (cell_header->free_count == 0) {
                bin.partial_head = reinterpret_cast<CellHeader *>(metadata->next_partial);
                metadata->next_partial = nullptr;
            }

            // Update stats
            bin.total_allocated++;
            bin.current_allocated++;

            return block;
        }

        // No partial cells available, get a fresh cell
        void *raw_cell = m_allocator->alloc();
        if (!raw_cell) {
            return nullptr;
        }

        // Initialize the cell for this bin
        init_cell_for_bin(raw_cell, bin_index, tag);

        CellHeader *cell_header = static_cast<CellHeader *>(raw_cell);
        CellMetadata *metadata = get_metadata(cell_header);

        // Pop the first block
        FreeBlock *block = metadata->free_list;
        metadata->free_list = block->next;
        cell_header->free_count--;

        // Add to partial list (if there are still free blocks)
        if (cell_header->free_count > 0) {
            metadata->next_partial = reinterpret_cast<CellHeader *>(bin.partial_head);
            bin.partial_head = cell_header;
        }

        // Update stats
        bin.total_allocated++;
        bin.current_allocated++;

        return block;
    }

    void Context::free_to_bin(void *ptr, CellHeader *header) {
        size_t bin_index = header->size_class;
        assert(bin_index < kNumSizeBins);

        size_t block_size = kSizeClasses[bin_index];

#ifndef NDEBUG
        // Poison the freed memory
        std::memset(ptr, kPoisonByte, block_size);
#endif

        // TLS fast path for hot bins (0-3: 16B, 32B, 64B, 128B)
        if (bin_index < kTlsBinCacheCount) {
            TlsBinCache &cache = t_bin_cache[bin_index];
            if (!cache.is_full()) {
                cache.push(static_cast<FreeBlock *>(ptr));
                return;
            }
        }

        // Fallback: lock-based free to global bin
        std::lock_guard<std::mutex> lock(m_bin_locks[bin_index]);
        SizeBin &bin = m_bins[bin_index];
        CellMetadata *metadata = get_metadata(header);

        // Check if cell was full (not in partial list)
        bool was_full = (header->free_count == 0);

        // Add block back to cell's free list
        auto *block = static_cast<FreeBlock *>(ptr);
        block->next = metadata->free_list;
        metadata->free_list = block;
        header->free_count++;

        // Update stats
        bin.current_allocated--;

        // Calculate max blocks for this bin
        size_t max_blocks = blocks_per_cell(bin_index);

        // If cell is now completely empty
        if (header->free_count == max_blocks) {
            // Warm reserve policy: keep a few empty cells per bin
            if (bin.warm_cell_count < kWarmCellsPerBin) {
                // Keep as warm reserve, stays in partial list
                bin.warm_cell_count++;
                if (was_full) {
                    // Add to partial list
                    metadata->next_partial = reinterpret_cast<CellHeader *>(bin.partial_head);
                    bin.partial_head = header;
                }
            } else {
                // Return cell to allocator
                // First, remove from partial list
                CellHeader **pp = &bin.partial_head;
                while (*pp && *pp != header) {
                    pp = reinterpret_cast<CellHeader **>(&get_metadata(*pp)->next_partial);
                }
                if (*pp == header) {
                    *pp = reinterpret_cast<CellHeader *>(metadata->next_partial);
                }
                metadata->next_partial = nullptr;

                // Return to allocator
                m_allocator->free(header);
            }
        } else if (was_full) {
            // Cell was full, now has space - add to partial list
            metadata->next_partial = reinterpret_cast<CellHeader *>(bin.partial_head);
            bin.partial_head = header;
        }
        // Otherwise cell is already in partial list, nothing to do
    }

    void Context::init_cell_for_bin(void *cell, size_t bin_index, uint8_t tag) {
        auto *header = static_cast<CellHeader *>(cell);
        CellMetadata *metadata = get_metadata(header);

        // Set up header
        header->tag = tag;
        header->size_class = static_cast<uint8_t>(bin_index);

#ifndef NDEBUG
        header->magic = kCellMagic;
        header->generation = 0;
#endif

        // Calculate block layout
        size_t block_size = kSizeClasses[bin_index];
        size_t num_blocks = blocks_per_cell(bin_index);
        header->free_count = static_cast<uint16_t>(num_blocks);

        // Initialize metadata
        metadata->next_partial = nullptr;
        metadata->free_list = nullptr;

        // Build free list (all blocks are free initially)
        char *block_start = static_cast<char *>(get_block_start(header));
        FreeBlock *prev = nullptr;

        for (size_t i = num_blocks; i > 0; --i) {
            auto *block = reinterpret_cast<FreeBlock *>(block_start + (i - 1) * block_size);
            block->next = prev;
            prev = block;
        }

        metadata->free_list = prev;
    }

    void Context::batch_refill_tls_bin(size_t bin_index, uint8_t tag) {
        assert(bin_index < kTlsBinCacheCount);

        TlsBinCache &cache = t_bin_cache[bin_index];
        size_t to_refill = kTlsBinBatchRefill;

        std::lock_guard<std::mutex> lock(m_bin_locks[bin_index]);
        SizeBin &bin = m_bins[bin_index];

        // Try to get blocks from partial cells
        while (to_refill > 0 && !cache.is_full() && bin.partial_head) {
            CellHeader *cell_header = bin.partial_head;
            CellMetadata *metadata = get_metadata(cell_header);

            while (to_refill > 0 && !cache.is_full() && metadata->free_list) {
                FreeBlock *block = metadata->free_list;
                metadata->free_list = block->next;
                cell_header->free_count--;
                cache.push(block);
                --to_refill;

                bin.total_allocated++;
                bin.current_allocated++;
            }

            // If cell is now full, remove from partial list
            if (cell_header->free_count == 0) {
                bin.partial_head = reinterpret_cast<CellHeader *>(metadata->next_partial);
                metadata->next_partial = nullptr;
            }
        }

        // If we still need more blocks, allocate a fresh cell
        if (to_refill > 0 && !cache.is_full()) {
            void *raw_cell = m_allocator->alloc();
            if (raw_cell) {
                init_cell_for_bin(raw_cell, bin_index, tag);

                CellHeader *cell_header = static_cast<CellHeader *>(raw_cell);
                CellMetadata *metadata = get_metadata(cell_header);

                // Take blocks from the new cell
                while (to_refill > 0 && !cache.is_full() && metadata->free_list) {
                    FreeBlock *block = metadata->free_list;
                    metadata->free_list = block->next;
                    cell_header->free_count--;
                    cache.push(block);
                    --to_refill;

                    bin.total_allocated++;
                    bin.current_allocated++;
                }

                // Add remaining blocks to partial list
                if (cell_header->free_count > 0) {
                    metadata->next_partial = reinterpret_cast<CellHeader *>(bin.partial_head);
                    bin.partial_head = cell_header;
                }
            }
        }
    }

    void Context::flush_tls_bin_caches() {
        for (size_t bin_index = 0; bin_index < kTlsBinCacheCount; ++bin_index) {
            TlsBinCache &cache = t_bin_cache[bin_index];

            while (!cache.is_empty()) {
                FreeBlock *block = cache.pop();
                CellHeader *header = get_header(block);

                // Use the lock-based path for proper cell management
                std::lock_guard<std::mutex> lock(m_bin_locks[bin_index]);
                SizeBin &bin = m_bins[bin_index];
                CellMetadata *metadata = get_metadata(header);

                bool was_full = (header->free_count == 0);

                block->next = metadata->free_list;
                metadata->free_list = block;
                header->free_count++;

                bin.current_allocated--;

                size_t max_blocks = blocks_per_cell(bin_index);

                if (header->free_count == max_blocks) {
                    if (bin.warm_cell_count < kWarmCellsPerBin) {
                        bin.warm_cell_count++;
                        if (was_full) {
                            metadata->next_partial =
                                reinterpret_cast<CellHeader *>(bin.partial_head);
                            bin.partial_head = header;
                        }
                    } else {
                        CellHeader **pp = &bin.partial_head;
                        while (*pp && *pp != header) {
                            pp = reinterpret_cast<CellHeader **>(&get_metadata(*pp)->next_partial);
                        }
                        if (*pp == header) {
                            *pp = reinterpret_cast<CellHeader *>(metadata->next_partial);
                        }
                        metadata->next_partial = nullptr;
                        m_allocator->free(header);
                    }
                } else if (was_full) {
                    metadata->next_partial = reinterpret_cast<CellHeader *>(bin.partial_head);
                    bin.partial_head = header;
                }
            }
        }
    }

    // =========================================================================
    // Debug API Implementation
    // =========================================================================

#ifdef CELL_DEBUG_GUARDS
    bool Context::check_guards(void *ptr) const {
        if (!ptr) {
            return false;
        }

        auto *user_ptr = static_cast<uint8_t *>(ptr);
        auto *front_guard = user_ptr - kGuardSize;

        // Check front guard
        for (size_t i = 0; i < kGuardSize; ++i) {
            if (front_guard[i] != kGuardPattern) {
                return false;
            }
        }

#ifdef CELL_DEBUG_LEAKS
        // Check back guard if we have size info
        {
            std::lock_guard<std::mutex> lock(m_debug_mutex);
            auto it = m_live_allocs.find(ptr);
            if (it != m_live_allocs.end()) {
                auto *back_guard = user_ptr + it->second.size;
                for (size_t i = 0; i < kGuardSize; ++i) {
                    if (back_guard[i] != kGuardPattern) {
                        return false;
                    }
                }
            }
        }
#endif

        return true;
    }
#endif

#ifdef CELL_DEBUG_LEAKS
    void Context::report_leaks() const {
        std::lock_guard<std::mutex> lock(m_debug_mutex);

        for (const auto &[ptr, alloc] : m_live_allocs) {
            std::fprintf(stderr, "  Leak: %p, size=%zu, tag=%u\n", alloc.ptr, alloc.size,
                         alloc.tag);
#ifdef CELL_DEBUG_STACKTRACE
            if (alloc.stack_depth > 0) {
                print_stack(alloc.stack, alloc.stack_depth);
            }
#endif
        }
    }

    size_t Context::live_allocation_count() const {
        std::lock_guard<std::mutex> lock(m_debug_mutex);
        return m_live_allocs.size();
    }
#endif
}
