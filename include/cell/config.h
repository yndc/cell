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

    // Static validation for allocation tiers
    static_assert(kSuperblockSize >= kCellSize, "Superblock must be >= cell size");
    static_assert(kSuperblockSize % kCellSize == 0, "Superblock must be multiple of cell size");
    static_assert(kCellsPerSuperblock >= 1, "Must have at least 1 cell per superblock");
    static_assert(kTlsCacheCapacity >= 1, "TLS cache must hold at least 1 cell");

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
    };

}
