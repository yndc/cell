#pragma once

#include <cstddef>
#include <cstdint>

namespace Cell {

    /**
     * @brief Log2 of the Cell size. Default is 14 (16KB).
     *
     * Must be a power of 2. Minimum is 12 (4KB, standard page size).
     */
    static constexpr size_t kCellSizeLog2 = 14;

    /** @brief Cell size in bytes (2^kCellSizeLog2). */
    static constexpr size_t kCellSize = 1ULL << kCellSizeLog2;

    /** @brief Bitmask for aligning pointers to Cell boundaries. */
    static constexpr uintptr_t kCellMask = ~(kCellSize - 1);

    static_assert(kCellSize >= 4096, "Cell size must be at least 4KB (standard page size)");
    static_assert((kCellSize & (kCellSize - 1)) == 0, "Cell size must be a power of 2");

    // -------------------------------------------------------------------------
    // Allocation Tier Configuration
    // -------------------------------------------------------------------------

    /** @brief Log2 of the superblock size. Default is 21 (2MB). */
    static constexpr size_t kSuperblockSizeLog2 = 21;

    /** @brief Superblock size in bytes (2^kSuperblockSizeLog2). */
    static constexpr size_t kSuperblockSize = 1ULL << kSuperblockSizeLog2;

    /** @brief Number of cells carved from each superblock. */
    static constexpr size_t kCellsPerSuperblock = kSuperblockSize / kCellSize;

    /** @brief Number of cells cached per thread (TLS). */
    static constexpr size_t kTlsCacheCapacity = 64;

    /** @brief Number of bins with TLS caching (bins 0-8: 16B to 4KB). */
    static constexpr size_t kTlsBinCacheCount = 9;

    /** @brief Number of blocks cached per bin per thread. */
    static constexpr size_t kTlsBinCacheCapacity = 32;

    /** @brief Number of blocks to refill from global bin at once. */
    static constexpr size_t kTlsBinBatchRefill = 16;

    // Static validation for allocation tiers
    static_assert(kSuperblockSize >= kCellSize, "Superblock must be >= cell size");
    static_assert(kSuperblockSize % kCellSize == 0, "Superblock must be multiple of cell size");
    static_assert(kCellsPerSuperblock >= 1, "Must have at least 1 cell per superblock");
    static_assert(kTlsCacheCapacity >= 1, "TLS cache must hold at least 1 cell");

    // -------------------------------------------------------------------------
    // Sub-Cell Allocation Configuration (Size Classes)
    // -------------------------------------------------------------------------

    /** @brief Number of size class bins for sub-cell allocation. */
    static constexpr size_t kNumSizeBins = 10;

    /** @brief Minimum block size in bytes (must fit a free-list pointer). */
    static constexpr size_t kMinBlockSize = 16;

    /** @brief Maximum size for sub-cell allocation. Larger uses full cells. */
    static constexpr size_t kMaxSubCellSize = 8192;

    /** @brief Size class lookup table (power-of-2 sizes). */
    static constexpr size_t kSizeClasses[kNumSizeBins] = {16,  32,   64,   128,  256,
                                                          512, 1024, 2048, 4096, 8192};

    /** @brief Number of warm cells to keep per bin (avoids thrashing). */
    static constexpr size_t kWarmCellsPerBin = 2;

    /** @brief Marker for full-cell allocations (not sub-cell). */
    static constexpr uint8_t kFullCellMarker = 0xFF;

    // Static validation for sub-cell configuration
    static_assert(kNumSizeBins > 0, "Must have at least 1 size bin");
    static_assert(kMinBlockSize >= sizeof(void *), "Min block must fit a pointer");
    static_assert(kMaxSubCellSize < kCellSize, "Max sub-cell size must be < cell size");
    static_assert(kSizeClasses[0] == kMinBlockSize, "First size class must match min block size");
    static_assert(kSizeClasses[kNumSizeBins - 1] == kMaxSubCellSize,
                  "Last size class must match max");

    /**
     * @brief Configuration for creating a Context.
     */
    struct Config {
        /**
         * @brief Total address space to reserve in bytes.
         *
         * Default: 16GB. Physical RAM is committed lazily.
         */
        size_t reserve_size = 16ULL * 1024 * 1024 * 1024;

#ifdef CELL_ENABLE_BUDGET
        /**
         * @brief Maximum bytes this Context may allocate.
         *
         * When the budget would be exceeded, allocation returns nullptr.
         * Default: 0 (unlimited).
         */
        size_t memory_budget = 0;
#endif
    };

}
