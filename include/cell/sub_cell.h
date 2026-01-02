#pragma once

#include "cell.h"
#include "config.h"

#include <cstddef>
#include <cstdint>

namespace Cell {

    // -------------------------------------------------------------------------
    // Size Class Utilities
    // -------------------------------------------------------------------------

    /**
     * @brief Rounds a size up to the given alignment.
     *
     * @param size Size to round up.
     * @param alignment Alignment (must be power of 2).
     * @return Aligned size.
     */
    inline constexpr size_t align_up(size_t size, size_t alignment) {
        return (size + alignment - 1) & ~(alignment - 1);
    }

    /**
     * @brief Finds the size class bin for a given allocation request.
     *
     * @param size Size of the allocation in bytes.
     * @param alignment Required alignment (must be power of 2).
     * @return Bin index (0 to kNumSizeBins-1), or kFullCellMarker if too large.
     */
    inline uint8_t get_size_class(size_t size, size_t alignment) {
        // Round up to alignment requirement
        size = align_up(size, alignment);

        // Ensure minimum size
        if (size < kMinBlockSize) {
            size = kMinBlockSize;
        }

        // Find smallest bin that fits
        for (size_t i = 0; i < kNumSizeBins; ++i) {
            if (kSizeClasses[i] >= size) {
                // Verify alignment is satisfied by this bin
                // Power-of-2 sizes are naturally aligned to any smaller power-of-2
                if (kSizeClasses[i] >= alignment) {
                    return static_cast<uint8_t>(i);
                }
            }
        }

        // Too large for sub-cell allocation
        return kFullCellMarker;
    }

// Branch hint macros for performance-critical paths
#if defined(__GNUC__) || defined(__clang__)
#define CELL_LIKELY(x) __builtin_expect(!!(x), 1)
#define CELL_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define CELL_FORCE_INLINE __attribute__((always_inline)) inline
#else
#define CELL_LIKELY(x) (x)
#define CELL_UNLIKELY(x) (x)
#define CELL_FORCE_INLINE __forceinline
#endif

    /**
     * @brief Fast O(1) size class lookup using bit manipulation.
     *
     * Uses count-leading-zeros to find the bin in constant time.
     * Only valid for power-of-2 size classes.
     *
     * @param size Size of the allocation (will be rounded up to min).
     * @return Bin index (0-9), or kFullCellMarker if too large.
     */
    CELL_FORCE_INLINE uint8_t get_size_class_fast(size_t size) {
        // Clamp to minimum
        if (CELL_UNLIKELY(size < kMinBlockSize)) {
            size = kMinBlockSize;
        }

        // Too large for sub-cell
        if (CELL_UNLIKELY(size > kMaxSubCellSize)) {
            return kFullCellMarker;
        }

        // Round up to next power of 2
        // Formula: ceil(log2(size)) gives us the order
        // For power-of-2 sizes: bin = log2(size) - 4  (since 16 = 2^4 is bin 0)
        size_t v = size - 1; // Handle exact powers correctly

#if defined(__GNUC__) || defined(__clang__)
        // Use builtin for speed: 63 - clz gives us floor(log2(v))
        // We want ceil(log2(size)) = floor(log2(size-1)) + 1 for non-powers
        unsigned order = (v == 0) ? 0 : (64 - __builtin_clzll(v));
#else
        // Fallback: bit scan
        unsigned order = 0;
        while (v >>= 1)
            ++order;
        ++order;
#endif

        // Clamp minimum to 4 (16 bytes = 2^4)
        if (order < 4)
            order = 4;

        // Bin index = order - 4 (since bin 0 = 16 = 2^4)
        return static_cast<uint8_t>(order - 4);
    }

    /**
     * @brief Calculates how many blocks fit in a cell for a given size class.
     *
     * @param bin_index The size class bin index.
     * @return Number of blocks that fit in one cell.
     */
    inline constexpr size_t blocks_per_cell(size_t bin_index) {
        return (kCellSize - kBlockStartOffset) / kSizeClasses[bin_index];
    }

    // -------------------------------------------------------------------------
    // Size Bin
    // -------------------------------------------------------------------------

    /**
     * @brief Manages cells dedicated to a specific size class.
     *
     * Each bin maintains a list of "partial" cells that have at least one free block.
     * The allocator tries partial cells first, then requests fresh cells.
     */
    struct SizeBin {
        CellHeader *partial_head = nullptr; /**< Head of partial cell list. */
        size_t warm_cell_count = 0;         /**< Number of warm (empty) cells kept. */

        // Statistics (optional, useful for debugging)
        size_t total_allocated = 0;   /**< Total blocks allocated from this bin. */
        size_t current_allocated = 0; /**< Currently allocated blocks. */
    };

}
