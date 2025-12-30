#pragma once

#include "allocator.h"
#include "buddy.h"
#include "cell.h"
#include "config.h"
#include "large.h"
#include "sub_cell.h"

#include <memory>
#include <mutex>

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
    };

}
