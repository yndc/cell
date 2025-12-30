#pragma once

#include "context.h"

#include <cstddef>
#include <cstdint>

namespace Cell {

    /**
     * @brief A fast linear allocator with bulk deallocation.
     *
     * Allocations are O(1) pointer bumps. Individual frees are not supported.
     * Call reset() to free all allocations at once.
     *
     * Thread safety: NOT thread-safe. Use one Arena per thread.
     */
    class Arena {
    public:
        /**
         * @brief Creates an arena backed by the given context.
         *
         * @param ctx Context to allocate cells from.
         * @param tag Memory tag for profiling (applied to all cells).
         */
        explicit Arena(Context &ctx, uint8_t tag = 0);

        /**
         * @brief Destroys the arena, returning all cells to the context.
         */
        ~Arena();

        // Non-copyable, non-movable
        Arena(const Arena &) = delete;
        Arena &operator=(const Arena &) = delete;
        Arena(Arena &&) = delete;
        Arena &operator=(Arena &&) = delete;

        // =====================================================================
        // Allocation
        // =====================================================================

        /**
         * @brief Allocates memory from the arena.
         *
         * @param size Size in bytes to allocate.
         * @param alignment Required alignment (default: 8, must be power of 2).
         * @return Pointer to allocated memory, or nullptr if out of space.
         */
        [[nodiscard]] void *alloc(size_t size, size_t alignment = 8);

        /**
         * @brief Allocates memory for a single object of type T.
         *
         * @tparam T Type to allocate.
         * @return Pointer to uninitialized memory, or nullptr if out of space.
         */
        template <typename T> [[nodiscard]] T *alloc() {
            return static_cast<T *>(alloc(sizeof(T), alignof(T)));
        }

        /**
         * @brief Allocates memory for an array of objects.
         *
         * @tparam T Element type.
         * @param count Number of elements.
         * @return Pointer to uninitialized memory, or nullptr if out of space.
         */
        template <typename T> [[nodiscard]] T *alloc_array(size_t count) {
            return static_cast<T *>(alloc(sizeof(T) * count, alignof(T)));
        }

        // =====================================================================
        // Lifetime Management
        // =====================================================================

        /**
         * @brief Resets the arena, freeing all allocations.
         *
         * After reset, all previously returned pointers are invalid.
         * Cells are NOT returned to the context — they're kept for reuse.
         */
        void reset();

        /**
         * @brief Resets the arena AND returns all cells to the context.
         *
         * Use when the arena won't be used for a while to reduce memory.
         */
        void release();

        // =====================================================================
        // Markers (Nested Scopes)
        // =====================================================================

        /**
         * @brief Opaque marker representing the current allocation point.
         */
        struct Marker {
            size_t cell_index;
            size_t offset;
            size_t total_allocated;
        };

        /**
         * @brief Saves the current allocation point.
         * @return Marker that can be passed to reset_to_marker().
         */
        [[nodiscard]] Marker save() const;

        /**
         * @brief Resets to a previously saved marker.
         *
         * All allocations made after the marker was saved become invalid.
         * Cells allocated after the marker are returned for reuse (not to context).
         *
         * @param marker Previously saved marker.
         */
        void reset_to_marker(Marker marker);

        // =====================================================================
        // Introspection
        // =====================================================================

        /**
         * @brief Returns total bytes allocated from this arena.
         */
        [[nodiscard]] size_t bytes_allocated() const;

        /**
         * @brief Returns total bytes available before needing a new cell.
         */
        [[nodiscard]] size_t bytes_remaining() const;

        /**
         * @brief Returns number of cells currently held by this arena.
         */
        [[nodiscard]] size_t cell_count() const;

    private:
        // =====================================================================
        // Internal Types
        // =====================================================================

        /**
         * @brief Inline linked list node stored at start of each cell's payload.
         *
         * This uses the first 8 bytes of usable cell space to chain cells.
         */
        struct CellLink {
            CellData *next;
        };

        /** @brief Usable space per cell after header and link. */
        static constexpr size_t kUsablePerCell = kCellSize - kBlockStartOffset - sizeof(CellLink);

        // =====================================================================
        // Members
        // =====================================================================

        Context &m_ctx;
        uint8_t m_tag;

        CellData *m_head = nullptr;      ///< Current cell (allocating from this).
        size_t m_offset = 0;             ///< Current offset in head cell's usable space.
        size_t m_cell_count = 0;         ///< Number of cells held.
        size_t m_current_cell_index = 0; ///< Index of current cell (for markers).
        size_t m_total_allocated = 0;    ///< Total bytes allocated.

        // =====================================================================
        // Internal Methods
        // =====================================================================

        /**
         * @brief Gets the start of usable space in a cell (after link).
         */
        static char *get_usable_start(CellData *cell);

        /**
         * @brief Gets the CellLink from a cell.
         */
        static CellLink *get_link(CellData *cell);

        /**
         * @brief Allocates a new cell and makes it the head.
         * @return true if successful, false if allocation failed.
         */
        bool grow();

        /**
         * @brief Available space remaining in current cell.
         */
        size_t available() const;

        /**
         * @brief Aligns an offset up to the given alignment.
         */
        static size_t align_offset(size_t offset, size_t alignment);
    };

}
