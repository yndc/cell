#pragma once

#include "cell.h"
#include "config.h"

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

        /**
         * @brief Allocates a Cell from this context's pool.
         * @param tag Application-defined tag for profiling (default: 0).
         * @return Pointer to an aligned CellData, or nullptr on failure.
         */
        CellData *alloc(uint8_t tag = 0);

        /**
         * @brief Returns a Cell to this context's pool.
         * @param cell Pointer to the Cell to free.
         */
        void free(CellData *cell);

    private:
        void *m_base = nullptr;      /**< Start of reserved address range. */
        size_t m_reserved_size = 0;  /**< Total reserved bytes. */
        size_t m_committed_size = 0; /**< Currently committed bytes. */
    };

}
