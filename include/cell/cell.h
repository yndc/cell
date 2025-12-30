#pragma once

#include <cstdint>

#include "config.h"

namespace Cell {

    /**
     * @brief Header stored at the beginning of each Cell.
     *
     * Contains metadata for profiling and management.
     */
    struct CellHeader {
        uint8_t tag;         /**< Application-defined memory tag for profiling. */
        uint8_t reserved[7]; /**< Reserved for future use (alignment, generation counters, etc.) */
    };

    /**
     * @brief A fixed-size, aligned memory unit.
     *
     * The usable payload starts after the CellHeader.
     */
    struct CellData {
        CellHeader header; /**< Metadata header at the start of the cell. */
        // Remaining bytes are available for allocation
    };

    /**
     * @brief Locates the CellHeader for any pointer within a Cell.
     *
     * Performs a constant-time alignment mask.
     *
     * @param ptr Any pointer within a Cell's memory range.
     * @return Pointer to the CellHeader at the start of the Cell.
     */
    inline CellHeader *get_header(void *ptr) {
        auto addr = reinterpret_cast<uintptr_t>(ptr);
        return reinterpret_cast<CellHeader *>(addr & kCellMask);
    }

}
