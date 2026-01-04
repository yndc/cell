#include "cell/context.h"

#include "tls_bin_cache.h"
#include "tls_cache.h"

#include <cassert>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

// SIMD intrinsics for batch operations
#if defined(__x86_64__) || defined(_M_X64)
#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif
#endif

namespace Cell {

    // =========================================================================
    // Bit Manipulation Helpers
    // =========================================================================

    /**
     * @brief Rounds up to the next power of 2 using O(1) bit manipulation.
     * @param v Value to round up (must be > 0).
     * @return Next power of 2 >= v.
     */
    CELL_FORCE_INLINE size_t next_power_of_2(size_t v) {
        if (v == 0)
            return 1;
        --v;
#if defined(__GNUC__) || defined(__clang__)
        // Use count-leading-zeros for O(1) computation
        return 1ULL << (64 - __builtin_clzll(v | 1));
#elif defined(_MSC_VER)
        unsigned long idx;
        _BitScanReverse64(&idx, v | 1);
        return 1ULL << (idx + 1);
#else
        // Fallback: bit smearing
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        v |= v >> 32;
        return v + 1;
#endif
    }

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

#ifdef CELL_ENABLE_BUDGET
        m_budget = config.memory_budget;
#endif
    }

    // =========================================================================
    // Budget Helpers
    // =========================================================================

#ifdef CELL_ENABLE_BUDGET
    bool Context::check_budget(size_t size) {
        if (m_budget == 0) {
            return true; // Unlimited
        }
        size_t current = m_budget_current.load(std::memory_order_relaxed);
        if (current + size > m_budget) {
            if (m_budget_callback) {
                m_budget_callback(size, m_budget, current);
            }
            return false;
        }
        return true;
    }

    void Context::record_budget_alloc(size_t size) {
        m_budget_current.fetch_add(size, std::memory_order_relaxed);
    }

    void Context::record_budget_free(size_t size) {
        m_budget_current.fetch_sub(size, std::memory_order_relaxed);
    }
#endif

    // =========================================================================
    // Instrumentation Helpers
    // =========================================================================

#ifdef CELL_ENABLE_INSTRUMENTATION
    void Context::invoke_alloc_callback(void *ptr, size_t size, uint8_t tag, bool is_alloc) {
        if (m_alloc_callback) {
            m_alloc_callback(ptr, size, tag, is_alloc);
        }
    }
#endif

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
        
        // Also clear the cell-level TLS cache (don't flush, just clear)
        // The cached cells will be freed when the memory region is unmapped
        // Flushing would push them to the global pool which is about to be unmapped
        t_cache.count = 0;

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

        if (alignment == 0 || (alignment & (alignment - 1)) != 0 || alignment > 16) {
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

#ifdef CELL_ENABLE_BUDGET
        // Calculate budget_size upfront so check_budget uses the same rounded size
        // that record_budget_alloc will use, preventing budget overruns from rounding
        size_t budget_size = 0;
        if (alloc_size <= kMaxSubCellSize) {
            uint8_t bin_index = get_size_class(alloc_size, alignment);
            if (bin_index == kFullCellMarker) {
                budget_size = kCellSize;
            } else {
                budget_size = kSizeClasses[bin_index];
            }
        } else if (size <= usable_cell_size) {
            budget_size = kCellSize;
        } else {
            // Large allocation - alloc_large handles its own budget check
            budget_size = 0;
        }

        // Check budget with actual rounded size (skip if alloc_large will handle it)
        if (budget_size > 0 && !check_budget(budget_size)) {
            return nullptr;
        }
#endif

        if (CELL_LIKELY(alloc_size <= kMaxSubCellSize)) {
            // Sub-cell allocation - hot path
            if (CELL_UNLIKELY(!m_allocator))
                return nullptr;

            // Fast path: common sizes with default alignment go through TLS cache
            // directly, avoiding function call overhead (bins 0-8: 16B to 4KB)
#if !defined(CELL_DEBUG_GUARDS) && !defined(CELL_DEBUG_LEAKS) && !defined(CELL_ENABLE_BUDGET)
            if (CELL_LIKELY(alignment <= 8 && alloc_size <= 4096)) {
                // Use O(1) size class lookup
                uint8_t bin_index = get_size_class_fast(alloc_size);

                // Inline TLS cache check for maximum speed
                TlsBinCache &cache = t_bin_cache[bin_index];
                if (CELL_LIKELY(cache.count > 0)) {
                    result = cache.blocks[--cache.count];
#ifdef CELL_ENABLE_STATS
                    m_stats.record_alloc(kSizeClasses[bin_index], tag);
                    m_stats.subcell_allocs.fetch_add(1, std::memory_order_relaxed);
#endif
#ifdef CELL_ENABLE_INSTRUMENTATION
                    invoke_alloc_callback(result, size, tag, true);
#endif
                    return result;
                }
                // TLS cache empty - fall through to slow path
            }
#endif

            uint8_t bin_index = get_size_class(alloc_size, alignment);
            if (CELL_UNLIKELY(bin_index == kFullCellMarker)) {
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
            // Stats and budget tracking handled in alloc_large
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

#ifdef CELL_ENABLE_BUDGET
        if (budget_size > 0) {
            record_budget_alloc(budget_size);
        }
#endif

#ifdef CELL_ENABLE_INSTRUMENTATION
        invoke_alloc_callback(result, size, tag, true);
#endif

        return result;
    }

    // =========================================================================
    // Batch Allocation API (SIMD-optimized)
    // =========================================================================

    size_t Context::alloc_batch(size_t size, void **out_ptrs, size_t count, uint8_t tag) {
        if (CELL_UNLIKELY(count == 0 || !out_ptrs)) {
            return 0;
        }

        // Only sub-cell sizes benefit from TLS fast path
        if (CELL_UNLIKELY(size > kMaxSubCellSize || !m_allocator)) {
            // Fall back to individual allocations for large sizes
            size_t allocated = 0;
            for (size_t i = 0; i < count; ++i) {
                void *ptr = alloc_bytes(size, tag);
                if (!ptr)
                    break;
                out_ptrs[i] = ptr;
                ++allocated;
            }
            return allocated;
        }

        uint8_t bin_index = get_size_class_fast(size);
        size_t allocated = 0;

#if !defined(CELL_DEBUG_GUARDS) && !defined(CELL_DEBUG_LEAKS) && !defined(CELL_ENABLE_BUDGET)
        // SIMD-optimized TLS cache drain for supported bins
        if (CELL_LIKELY(bin_index < kTlsBinCacheCount)) {
            TlsBinCache &cache = t_bin_cache[bin_index];

            // Fast path: drain TLS cache in batches
            while (allocated < count && cache.count > 0) {
                // Calculate how many we can take from cache
                size_t take = std::min(count - allocated, cache.count);

#if defined(__AVX2__) && defined(__x86_64__)
                // AVX2: Copy 4 pointers (32 bytes) at a time
                while (take >= 4) {
                    cache.count -= 4;
                    // Prefetch next batch of cache blocks for better cache performance
                    if (cache.count >= 4) {
                        __builtin_prefetch(&cache.blocks[cache.count - 4], 0, 3);
                    }
                    __m256i ptrs = _mm256_loadu_si256(
                        reinterpret_cast<const __m256i *>(&cache.blocks[cache.count]));
                    _mm256_storeu_si256(reinterpret_cast<__m256i *>(&out_ptrs[allocated]), ptrs);
                    allocated += 4;
                    take -= 4;
                }
#elif defined(__SSE2__) && defined(__x86_64__)
                // SSE2: Copy 2 pointers (16 bytes) at a time
                while (take >= 2) {
                    cache.count -= 2;
                    // Prefetch next batch of cache blocks for better cache performance
                    if (cache.count >= 2) {
                        __builtin_prefetch(&cache.blocks[cache.count - 2], 0, 3);
                    }
                    __m128i ptrs = _mm_loadu_si128(
                        reinterpret_cast<const __m128i *>(&cache.blocks[cache.count]));
                    _mm_storeu_si128(reinterpret_cast<__m128i *>(&out_ptrs[allocated]), ptrs);
                    allocated += 2;
                    take -= 2;
                }
#endif
                // Scalar fallback for remaining
                while (take > 0) {
                    out_ptrs[allocated++] = cache.blocks[--cache.count];
                    --take;
                }

                // Refill cache if empty and need more
                if (allocated < count && cache.count == 0) {
                    batch_refill_tls_bin(bin_index, tag);
                }
            }

#ifdef CELL_ENABLE_STATS
            m_stats.subcell_allocs.fetch_add(allocated, std::memory_order_relaxed);
            for (size_t i = 0; i < allocated; ++i) {
                m_stats.record_alloc(kSizeClasses[bin_index], tag);
            }
#endif
        }
#endif

        // Slow path: allocate remaining individually
        while (allocated < count) {
            void *ptr = alloc_from_bin(bin_index, tag);
            if (!ptr)
                break;
            out_ptrs[allocated++] = ptr;

#ifdef CELL_ENABLE_STATS
            m_stats.record_alloc(kSizeClasses[bin_index], tag);
            m_stats.subcell_allocs.fetch_add(1, std::memory_order_relaxed);
#endif
        }

        return allocated;
    }

    void Context::free_batch(void **ptrs, size_t count) {
        if (CELL_UNLIKELY(count == 0 || !ptrs)) {
            return;
        }

#if !defined(CELL_DEBUG_GUARDS) && !defined(CELL_DEBUG_LEAKS) && !defined(CELL_ENABLE_BUDGET)
        // Fast path: check if first pointer is in cell region
        auto uptr = reinterpret_cast<uintptr_t>(ptrs[0]);
        auto base = reinterpret_cast<uintptr_t>(m_base);

        if (CELL_LIKELY(uptr >= base && uptr < base + m_reserved_size)) {
            // Get size class from first pointer
            CellHeader *first_header = get_header(ptrs[0]);
            uint8_t size_class = first_header->size_class;

#ifndef NDEBUG
            // Validate homogeneous batch - mixed sizes corrupt freelists
            for (size_t i = 1; i < count; ++i) {
                auto iptr = reinterpret_cast<uintptr_t>(ptrs[i]);
                assert(iptr >= base && iptr < base + m_reserved_size &&
                       "free_batch requires all pointers in cell region");
                CellHeader *h = get_header(ptrs[i]);
                assert(h->size_class == size_class &&
                       "free_batch requires all pointers to have same size class");
            }
#endif

            if (CELL_LIKELY(size_class < kTlsBinCacheCount)) {
                TlsBinCache &cache = t_bin_cache[size_class];
                size_t freed = 0;

                // SIMD-optimized TLS cache fill
                while (freed < count && cache.count < kTlsBinCacheCapacity) {
                    size_t space = kTlsBinCacheCapacity - cache.count;
                    size_t push = std::min(count - freed, space);

#if defined(__AVX2__) && defined(__x86_64__)
                    // AVX2: Copy 4 pointers at a time
                    while (push >= 4) {
                        __m256i block_ptrs =
                            _mm256_loadu_si256(reinterpret_cast<const __m256i *>(&ptrs[freed]));
                        _mm256_storeu_si256(reinterpret_cast<__m256i *>(&cache.blocks[cache.count]),
                                            block_ptrs);
                        cache.count += 4;
                        freed += 4;
                        push -= 4;
                    }
#elif defined(__SSE2__) && defined(__x86_64__)
                    // SSE2: Copy 2 pointers at a time
                    while (push >= 2) {
                        __m128i block_ptrs =
                            _mm_loadu_si128(reinterpret_cast<const __m128i *>(&ptrs[freed]));
                        _mm_storeu_si128(reinterpret_cast<__m128i *>(&cache.blocks[cache.count]),
                                         block_ptrs);
                        cache.count += 2;
                        freed += 2;
                        push -= 2;
                    }
#endif
                    // Scalar fallback
                    while (push > 0) {
                        cache.blocks[cache.count++] = static_cast<FreeBlock *>(ptrs[freed++]);
                        --push;
                    }
                }

#ifdef CELL_ENABLE_STATS
                m_stats.subcell_frees.fetch_add(freed, std::memory_order_relaxed);
#endif

                // Fall through to free remaining
                if (freed == count) {
                    return;
                }

                // Free remaining that didn't fit in TLS cache
                for (size_t i = freed; i < count; ++i) {
                    free_bytes(ptrs[i]);
                }
                return;
            }
        }
#endif

        // Fallback: free individually
        for (size_t i = 0; i < count; ++i) {
            free_bytes(ptrs[i]);
        }
    }

    void Context::free_bytes(void *ptr) {
        if (CELL_UNLIKELY(!ptr)) {
            return;
        }

#ifdef CELL_ENABLE_INSTRUMENTATION
        // For instrumentation, we need the size before we lose it
        // The callback will receive the originally requested size if available,
        // otherwise 0 (for non-tracked tiers)
        size_t callback_size = 0;
        uint8_t callback_tag = 0;
#endif

#ifdef CELL_DEBUG_LEAKS
        // Remove from tracking and get allocation size
        size_t alloc_size = 0;
        {
            std::lock_guard<std::mutex> lock(m_debug_mutex);
            auto it = m_live_allocs.find(ptr);
            if (it != m_live_allocs.end()) {
                alloc_size = it->second.size;
#ifdef CELL_ENABLE_INSTRUMENTATION
                callback_size = it->second.size;
                callback_tag = it->second.tag;
#endif
                m_live_allocs.erase(it);
            }
        }
#endif

#ifdef CELL_ENABLE_INSTRUMENTATION
        invoke_alloc_callback(ptr, callback_size, callback_tag, false);
#endif

        // Fast path: check if pointer is in cell region (most common case)
        // This is O(1) pointer comparison, much faster than buddy/large ownership checks
        auto uptr = reinterpret_cast<uintptr_t>(ptr);
        auto base = reinterpret_cast<uintptr_t>(m_base);

        if (CELL_LIKELY(uptr >= base && uptr < base + m_reserved_size)) {
            // Cell/sub-cell allocation - this is the hot path
#if !defined(CELL_DEBUG_GUARDS) && !defined(CELL_DEBUG_LEAKS) && !defined(CELL_ENABLE_BUDGET)
            // Ultra-fast path: inline TLS free for hot bins
            CellHeader *header = get_header(ptr);
            uint8_t size_class = header->size_class;

            if (CELL_LIKELY(size_class < kTlsBinCacheCount)) {
                // Hot bin - try TLS cache first
                TlsBinCache &cache = t_bin_cache[size_class];
                if (CELL_LIKELY(cache.count < kTlsBinCacheCapacity)) {
#ifndef NDEBUG
                    std::memset(ptr, kPoisonByte, kSizeClasses[size_class]);
#endif
#ifdef CELL_ENABLE_STATS
                    m_stats.record_free(kSizeClasses[size_class], header->tag);
                    m_stats.subcell_frees.fetch_add(1, std::memory_order_relaxed);
#endif
                    cache.blocks[cache.count++] = static_cast<FreeBlock *>(ptr);
                    return;
                }
            }
#endif
            // Fall through to normal cell/sub-cell handling
            goto handle_cell_subcell;
        }

        // Slower path: check buddy and large allocations
        if (m_buddy && m_buddy->owns(ptr)) {
#ifdef CELL_ENABLE_STATS
            m_stats.buddy_frees.fetch_add(1, std::memory_order_relaxed);
#endif
#ifdef CELL_ENABLE_BUDGET
            record_budget_free(m_buddy->get_alloc_size(ptr));
#endif
            m_buddy->free(ptr);
            return;
        }

        if (m_large_allocs.owns(ptr)) {
#ifdef CELL_ENABLE_STATS
            m_stats.large_frees.fetch_add(1, std::memory_order_relaxed);
#endif
#ifdef CELL_ENABLE_BUDGET
            record_budget_free(m_large_allocs.get_alloc_size(ptr));
#endif
            m_large_allocs.free(ptr);
            return;
        }

        // Not recognized - likely invalid pointer
        return;

    handle_cell_subcell:

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
#endif

        CellHeader *header = get_header(ptr);
        uint8_t tag = header->tag;

        if (header->size_class == kFullCellMarker) {
            // Full-cell allocation
#ifdef CELL_ENABLE_STATS
            m_stats.record_free(kCellSize, tag);
            m_stats.cell_frees.fetch_add(1, std::memory_order_relaxed);
#endif
#ifdef CELL_ENABLE_BUDGET
            record_budget_free(kCellSize);
#endif
            free_cell(reinterpret_cast<CellData *>(header));
        } else {
            // Sub-cell allocation
#ifdef CELL_ENABLE_STATS
            size_t block_size = kSizeClasses[header->size_class];
            m_stats.record_free(block_size, tag);
            m_stats.subcell_frees.fetch_add(1, std::memory_order_relaxed);
#endif
#ifdef CELL_ENABLE_BUDGET
            size_t budget_block_size = kSizeClasses[header->size_class];
            record_budget_free(budget_block_size);
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
                // Stay in buddy tier - delegate to buddy realloc
                // Note: buddy realloc doesn't know about leak tracking, so we handle it here
#ifdef CELL_DEBUG_LEAKS
                size_t old_alloc_size = 0;
                {
                    std::lock_guard<std::mutex> lock(m_debug_mutex);
                    auto it = m_live_allocs.find(ptr);
                    if (it != m_live_allocs.end()) {
                        old_alloc_size = it->second.size;
                        m_live_allocs.erase(it);
                    }
                }
#endif
                void *result = m_buddy->realloc_bytes(ptr, new_size);
#ifdef CELL_DEBUG_LEAKS
                if (result) {
                    std::lock_guard<std::mutex> lock(m_debug_mutex);
                    DebugAllocation alloc{};
                    alloc.ptr = result;
                    alloc.size = new_size;
                    alloc.tag = tag;
#ifdef CELL_DEBUG_STACKTRACE
                    alloc.stack_depth = capture_stack(alloc.stack, kMaxStackDepth, 2);
#endif
                    m_live_allocs[result] = alloc;
                }
#endif
                return result;
            }
            // Cross-tier: buddy -> somewhere else
#ifdef CELL_DEBUG_LEAKS
            {
                std::lock_guard<std::mutex> lock(m_debug_mutex);
                m_live_allocs.erase(ptr);
            }
#endif
            void *new_ptr = alloc_bytes(new_size, tag);
            if (!new_ptr)
                return nullptr;
            // Copy min(old_usable, new_size) to avoid reading past old allocation
            size_t old_usable = m_buddy->get_alloc_size(ptr) - 8; // Subtract header
            std::memcpy(new_ptr, ptr, std::min(old_usable, new_size));
            m_buddy->free(ptr);
            return new_ptr;
        }

        // Check large tier
        if (m_large_allocs.owns(ptr)) {
            // For large allocations, check if new size still needs large
            if (new_size > BuddyAllocator::kMaxBlockSize) {
                // Stay in large tier
#ifdef CELL_DEBUG_LEAKS
                {
                    std::lock_guard<std::mutex> lock(m_debug_mutex);
                    auto it = m_live_allocs.find(ptr);
                    if (it != m_live_allocs.end()) {
                        m_live_allocs.erase(it);
                    }
                }
#endif
                void *result = m_large_allocs.realloc_bytes(ptr, new_size, tag);
#ifdef CELL_DEBUG_LEAKS
                if (result) {
                    std::lock_guard<std::mutex> lock(m_debug_mutex);
                    DebugAllocation alloc{};
                    alloc.ptr = result;
                    alloc.size = new_size;
                    alloc.tag = tag;
#ifdef CELL_DEBUG_STACKTRACE
                    alloc.stack_depth = capture_stack(alloc.stack, kMaxStackDepth, 2);
#endif
                    m_live_allocs[result] = alloc;
                }
#endif
                return result;
            }
            // Cross-tier: large -> smaller tier
#ifdef CELL_DEBUG_LEAKS
            {
                std::lock_guard<std::mutex> lock(m_debug_mutex);
                m_live_allocs.erase(ptr);
            }
#endif
            void *new_ptr = alloc_bytes(new_size, tag);
            if (!new_ptr)
                return nullptr;
            // Copy min(old_size, new_size) to avoid reading past old allocation
            size_t old_size = m_large_allocs.get_alloc_size(ptr);
            std::memcpy(new_ptr, ptr, std::min(old_size, new_size));
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

#ifdef CELL_ENABLE_BUDGET
        // Calculate budget size upfront for check_budget
        // Buddy allocations round to power-of-2 including 8-byte header
        // Large allocations get page-rounded sizes
        size_t budget_size = 0;
        if (size <= BuddyAllocator::kMaxBlockSize && m_buddy) {
            // Calculate buddy rounded size (power-of-2 >= size + 8 byte header)
            size_t total = size + 8; // header
            if (total < BuddyAllocator::kMinBlockSize) {
                budget_size = BuddyAllocator::kMinBlockSize;
            } else {
                // O(1) power-of-2 rounding using bit manipulation
                budget_size = next_power_of_2(total);
                if (budget_size > BuddyAllocator::kMaxBlockSize) {
                    budget_size = BuddyAllocator::kMaxBlockSize;
                }
            }
        } else {
            // Large allocation - page-aligned, approximate as requested size
            // (record_budget_alloc will use get_alloc_size for actual)
            budget_size = size;
        }

        if (!check_budget(budget_size)) {
            return nullptr;
        }
#endif

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
#ifdef CELL_ENABLE_INSTRUMENTATION
                if (result) {
                    invoke_alloc_callback(result, size, tag, true);
                }
#endif
#ifdef CELL_ENABLE_BUDGET
                if (result) {
                    // Use actual rounded size for budget
                    record_budget_alloc(m_buddy->get_alloc_size(result));
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
#ifdef CELL_ENABLE_INSTRUMENTATION
        if (result) {
            invoke_alloc_callback(result, size, tag, true);
        }
#endif
#ifdef CELL_ENABLE_BUDGET
        if (result) {
            record_budget_alloc(m_large_allocs.get_alloc_size(result));
        }
#endif
        return result;
    }

    void Context::free_large(void *ptr) {
        if (!ptr)
            return;

#ifdef CELL_ENABLE_INSTRUMENTATION
        // Get size before freeing for callback
        size_t freed_size = 0;
        if (m_buddy && m_buddy->owns(ptr)) {
            freed_size = m_buddy->get_alloc_size(ptr);
        } else {
            freed_size = m_large_allocs.get_alloc_size(ptr);
        }
        invoke_alloc_callback(ptr, freed_size, 0, false);
#endif

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

#ifdef CELL_ENABLE_BUDGET
        // Calculate budget size upfront for check_budget
        // Similar logic to alloc_large: buddy rounds to power-of-2, large is page-aligned
        size_t budget_size = 0;
        if (size <= BuddyAllocator::kMaxBlockSize && m_buddy && alignment <= 8) {
            // Will use buddy path - calculate power-of-2 rounded size
            size_t total = size + 8; // header
            if (total < BuddyAllocator::kMinBlockSize) {
                budget_size = BuddyAllocator::kMinBlockSize;
            } else {
                // O(1) power-of-2 rounding using bit manipulation
                budget_size = next_power_of_2(total);
                if (budget_size > BuddyAllocator::kMaxBlockSize) {
                    budget_size = BuddyAllocator::kMaxBlockSize;
                }
            }
        } else {
            // Will use large allocation path - approximate as requested size
            budget_size = size;
        }

        if (!check_budget(budget_size)) {
            return nullptr;
        }
#endif

        // For buddy allocations: check if natural power-of-2 alignment is sufficient
        if (size <= BuddyAllocator::kMaxBlockSize && m_buddy) {
            // Calculate the order (and thus natural alignment) for this size
            // Account for buddy header (8 bytes)
            size_t total_size = size + 8;
            if (total_size < BuddyAllocator::kMinBlockSize) {
                total_size = BuddyAllocator::kMinBlockSize;
            }

            // O(1) power-of-2 rounding for block size
            size_t block_size = next_power_of_2(total_size);
            if (block_size > BuddyAllocator::kMaxBlockSize) {
                block_size = BuddyAllocator::kMaxBlockSize;
            }

            // Buddy blocks are naturally aligned to their size.
            // The user pointer is offset by 8 bytes (header), so actual alignment
            // is min(block_size, block_alignment_after_8_byte_offset).
            // For power-of-2 blocks >= 32KB aligned, offset by 8 is still 8-byte aligned
            // but the overall block is block_size aligned.
            //
            // If requested alignment <= block_size and block_size >= alignment,
            // we can use buddy. Otherwise, use LargeAllocRegistry.
            // Buddy user pointers are offset by 8-byte header from block start.
            // Only 8-byte alignment is guaranteed regardless of block size.
            if (alignment <= 8) {
                void *result = m_buddy->alloc(size);
#ifdef CELL_ENABLE_STATS
                if (result) {
                    m_stats.record_alloc(size, tag);
                    m_stats.buddy_allocs.fetch_add(1, std::memory_order_relaxed);
                }
#endif
#ifdef CELL_ENABLE_INSTRUMENTATION
                if (result) {
                    invoke_alloc_callback(result, size, tag, true);
                }
#endif
#ifdef CELL_ENABLE_BUDGET
                if (result) {
                    record_budget_alloc(m_buddy->get_alloc_size(result));
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
#ifdef CELL_ENABLE_INSTRUMENTATION
        if (result) {
            invoke_alloc_callback(result, size, tag, true);
        }
#endif
#ifdef CELL_ENABLE_BUDGET
        if (result) {
            record_budget_alloc(m_large_allocs.get_alloc_size(result));
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

        if (m_allocator) {
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
        if (CELL_LIKELY(bin_index < kTlsBinCacheCount)) {
            TlsBinCache &cache = t_bin_cache[bin_index];
            if (CELL_LIKELY(cache.count < kTlsBinCacheCapacity)) {
                cache.blocks[cache.count++] = static_cast<FreeBlock *>(ptr);
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
        
        // Also flush the cell-level TLS cache
        if (m_allocator) {
            m_allocator->flush_tls_cache();
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
