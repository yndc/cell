#pragma once

#include "allocator.h"
#include "buddy.h"
#include "cell.h"
#include "config.h"
#include "debug.h"
#include "large.h"
#include "stats.h"
#include "sub_cell.h"

#include <memory>
#include <mutex>
#ifdef CELL_DEBUG_LEAKS
#include <unordered_map>
#endif

namespace Cell {

    /**
     * @brief A memory environment owning a reserved virtual address range.
     *
     * RAII: Memory is released when the Context is destroyed.
     */
    class Context {
    public:
        /**
         * @brief Creates a new memory environment with the given configuration.
         * @param config Configuration options for the context.
         */
        explicit Context(const Config &config = Config{});

        /**
         * @brief Releases all virtual and physical memory.
         */
        ~Context();

        // Non-copyable, non-movable (owns OS resources)
        Context(const Context &) = delete;
        Context &operator=(const Context &) = delete;
        Context(Context &&) = delete;
        Context &operator=(Context &&) = delete;

        // =====================================================================
        // Sub-Cell Allocation API (preferred for most allocations)
        // =====================================================================

        /**
         * @brief Allocates memory of the specified size.
         *
         * Routing by size:
         * - <= kMaxSubCellSize (8KB): Sub-cell bins
         * - <= kCellSize (16KB): Full cell
         * - <= 2MB: Buddy allocator
         * - > 2MB: Direct OS allocation
         *
         * @param size Size in bytes to allocate.
         * @param tag Application-defined tag for profiling (default: 0).
         * @param alignment Required alignment (default: 8, must be power of 2).
         * @return Pointer to allocated memory, or nullptr on failure.
         */
        [[nodiscard]] void *alloc_bytes(size_t size, uint8_t tag = 0, size_t alignment = 8);

        /**
         * @brief Frees memory allocated by alloc_bytes().
         *
         * @param ptr Pointer to memory to free.
         */
        void free_bytes(void *ptr);

        /**
         * @brief Reallocates memory to a new size.
         *
         * Behavior:
         * - If ptr is nullptr, behaves like alloc_bytes(new_size, tag)
         * - If new_size is 0, behaves like free_bytes(ptr)
         * - Data preserved up to min(old_size, new_size)
         * - On failure, returns nullptr and old block is unchanged
         *
         * @param ptr Pointer from previous alloc_bytes/realloc_bytes
         * @param new_size New size in bytes
         * @param tag Tag for new allocation (used if allocating new block)
         * @return Pointer to reallocated memory, or nullptr on failure
         */
        [[nodiscard]] void *realloc_bytes(void *ptr, size_t new_size, uint8_t tag = 0);

        /**
         * @brief Allocates memory for a single object of type T.
         *
         * @tparam T Type to allocate (uses sizeof(T) and alignof(T)).
         * @param tag Application-defined tag for profiling (default: 0).
         * @return Pointer to uninitialized memory for T, or nullptr on failure.
         */
        template <typename T> [[nodiscard]] T *alloc(uint8_t tag = 0) {
            return static_cast<T *>(alloc_bytes(sizeof(T), tag, alignof(T)));
        }

        /**
         * @brief Allocates memory for an array of objects of type T.
         *
         * @tparam T Element type.
         * @param count Number of elements.
         * @param tag Application-defined tag for profiling (default: 0).
         * @return Pointer to uninitialized memory for count T objects, or nullptr.
         */
        template <typename T> [[nodiscard]] T *alloc_array(size_t count, uint8_t tag = 0) {
            return static_cast<T *>(alloc_bytes(sizeof(T) * count, tag, alignof(T)));
        }

        // =====================================================================
        // Large Allocation API (explicit, for allocations > 16KB)
        // =====================================================================

        /**
         * @brief Explicitly allocates a large block (uses buddy or direct OS).
         *
         * Routing:
         * - <= 2MB: Buddy allocator
         * - > 2MB: Direct OS with optional huge pages
         *
         * @param size Size in bytes.
         * @param tag Application-defined tag for profiling.
         * @param try_huge_pages For >2MB allocations, try to use huge pages.
         * @return Pointer to allocated memory, or nullptr on failure.
         */
        [[nodiscard]] void *alloc_large(size_t size, uint8_t tag = 0, bool try_huge_pages = true);

        /**
         * @brief Frees a large allocation.
         *
         * @param ptr Pointer returned by alloc_large().
         */
        void free_large(void *ptr);

        /**
         * @brief Allocates memory with explicit alignment (buddy/large only).
         *
         * For sizes that fit in sub-cell/cell range, use regular alloc() which
         * provides natural 16-byte alignment. This API is for cases requiring
         * higher alignment (e.g., SIMD, cache lines, page boundaries).
         *
         * @param size Size in bytes.
         * @param alignment Required alignment (must be power of 2).
         * @param tag Application-defined tag for profiling.
         * @return Aligned pointer, or nullptr on failure.
         */
        [[nodiscard]] void *alloc_aligned(size_t size, size_t alignment, uint8_t tag = 0);

        /**
         * @brief Flushes thread-local sub-cell caches to global bins.
         *
         * Call this before thread exit to avoid leaked cached blocks.
         * Only affects bins 0-3 (16B, 32B, 64B, 128B).
         */
        void flush_tls_bin_caches();

        // =====================================================================
        // Cell-Level Allocation API (for 16KB blocks or internal use)
        // =====================================================================

        /**
         * @brief Allocates a full Cell (16KB) from this context's pool.
         * @param tag Application-defined tag for profiling (default: 0).
         * @return Pointer to an aligned CellData, or nullptr on failure.
         */
        [[nodiscard]] CellData *alloc_cell(uint8_t tag = 0);

        /**
         * @brief Returns a full Cell to this context's pool.
         * @param cell Pointer to the Cell to free.
         */
        void free_cell(CellData *cell);

        // =====================================================================
        // Memory Management API
        // =====================================================================

        /**
         * @brief Decommits all fully-free memory regions to the OS.
         *
         * Call during loading screens, pause menus, or other idle periods
         * to release physical memory while keeping virtual address space.
         *
         * @return Number of bytes released to the OS.
         */
        size_t decommit_unused();

        /**
         * @brief Returns currently committed physical memory.
         */
        [[nodiscard]] size_t committed_bytes() const;

        // =====================================================================
        // Statistics (compile-time optional via CELL_ENABLE_STATS)
        // =====================================================================

#ifdef CELL_ENABLE_STATS
        /**
         * @brief Returns current memory statistics.
         */
        [[nodiscard]] const MemoryStats &get_stats() const { return m_stats; }

        /**
         * @brief Prints memory statistics to stdout.
         */
        void dump_stats() const { m_stats.dump(); }

        /**
         * @brief Resets all statistics counters.
         */
        void reset_stats() { m_stats.reset(); }
#endif

        // =====================================================================
        // Debug Features (compile-time optional)
        // =====================================================================

#ifdef CELL_DEBUG_GUARDS
        /**
         * @brief Checks if an allocation's guard bytes are intact.
         *
         * @param ptr Pointer returned by alloc_bytes().
         * @return true if guards are valid, false if corrupted.
         */
        [[nodiscard]] bool check_guards(void *ptr) const;
#endif

#ifdef CELL_DEBUG_LEAKS
        /**
         * @brief Reports all currently live allocations to stderr.
         *
         * Prints size, tag, and stack trace (if CELL_DEBUG_STACKTRACE is enabled).
         */
        void report_leaks() const;

        /**
         * @brief Returns number of live (unfreed) allocations.
         */
        [[nodiscard]] size_t live_allocation_count() const;
#endif

    private:
        // =====================================================================
        // Sub-Cell Implementation
        // =====================================================================

        /**
         * @brief Allocates a block from the specified size class bin.
         * @param bin_index Size class index (0 to kNumSizeBins-1).
         * @param tag Tag for profiling.
         * @return Pointer to allocated block, or nullptr on failure.
         */
        void *alloc_from_bin(size_t bin_index, uint8_t tag);

        /**
         * @brief Frees a block back to its size class bin.
         * @param ptr Pointer to the block.
         * @param header Cell header containing the block.
         */
        void free_to_bin(void *ptr, CellHeader *header);

        /**
         * @brief Initializes a fresh cell for a size class.
         * @param cell Raw cell memory.
         * @param bin_index Size class to prepare for.
         * @param tag Tag for profiling.
         */
        void init_cell_for_bin(void *cell, size_t bin_index, uint8_t tag);

        /**
         * @brief Batch refills TLS cache from global bin.
         * @param bin_index Size class index (must be < kTlsBinCacheCount).
         * @param tag Tag for profiling (used if new cell is needed).
         */
        void batch_refill_tls_bin(size_t bin_index, uint8_t tag);

        // =====================================================================
        // Members
        // =====================================================================

        void *m_base = nullptr;                 ///< Start of reserved address range.
        size_t m_reserved_size = 0;             ///< Total reserved bytes.
        std::unique_ptr<Allocator> m_allocator; ///< Cell-level allocator.

        SizeBin m_bins[kNumSizeBins];         ///< Size class bins.
        std::mutex m_bin_locks[kNumSizeBins]; ///< Per-bin locks.

        // Buddy allocator for 32KB - 2MB
        void *m_buddy_base = nullptr;     ///< Start of buddy region.
        size_t m_buddy_reserved_size = 0; ///< Buddy reserved size.
        std::unique_ptr<BuddyAllocator> m_buddy;

        // Large allocation registry for > 2MB
        LargeAllocRegistry m_large_allocs;

#ifdef CELL_ENABLE_STATS
        mutable MemoryStats m_stats;
#endif

#ifdef CELL_DEBUG_LEAKS
        mutable std::unordered_map<void *, DebugAllocation> m_live_allocs;
        mutable std::mutex m_debug_mutex;
#endif
    };

}
