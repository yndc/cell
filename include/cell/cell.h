#pragma once

#include "config.h"
#include "types.h"

namespace Cell {

    /**
     * @brief A memory environment owning a reserved virtual address range.
     *
     * RAII: Memory is released when the Context is destroyed.
     */
    class Context {
    public:
        /// Creates a new memory environment with the given configuration.
        explicit Context(const Config &config = Config{});

        /// Releases all virtual and physical memory.
        ~Context();

        // Non-copyable, non-movable (owns OS resources)
        Context(const Context &) = delete;
        Context &operator=(const Context &) = delete;
        Context(Context &&) = delete;
        Context &operator=(Context &&) = delete;

        /**
         * @brief Allocates a Cell from this context's pool.
         * @param tag Metadata tag for profiling.
         * @return Pointer to an aligned CellData, or nullptr on failure.
         */
        CellData *alloc(MemoryTag tag = MemoryTag::General);

        /**
         * @brief Returns a Cell to this context's pool.
         * @param cell Pointer to the Cell to free.
         */
        void free(CellData *cell);

    private:
        void *m_base = nullptr;      ///< Start of reserved address range
        size_t m_reserved_size = 0;  ///< Total reserved bytes
        size_t m_committed_size = 0; ///< Currently committed bytes
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

} // namespace Cell
