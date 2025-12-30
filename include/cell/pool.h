#pragma once

#include "arena.h"
#include "context.h"

#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>

namespace Cell {

    /**
     * @brief A typed object pool for fast allocation/deallocation of objects.
     *
     * Pool<T> is a thin wrapper around Context that provides type-safe allocation
     * with optional constructor/destructor support.
     *
     * Thread safety: Same as Context (per-bin locking for sub-cell allocations).
     *
     * @tparam T The type of objects to pool.
     */
    template <typename T> class Pool {
    public:
        /**
         * @brief Creates a pool backed by the given context.
         *
         * @param ctx Context to allocate from.
         * @param tag Memory tag for profiling.
         */
        explicit Pool(Context &ctx, uint8_t tag = 0) : m_ctx(ctx), m_tag(tag) {}

        // Non-copyable, movable
        Pool(const Pool &) = delete;
        Pool &operator=(const Pool &) = delete;
        Pool(Pool &&) = default;
        Pool &operator=(Pool &&) = default;

        // =====================================================================
        // Allocation (No Construction)
        // =====================================================================

        /**
         * @brief Allocates memory for one object without calling constructor.
         *
         * @return Pointer to uninitialized memory, or nullptr on failure.
         */
        [[nodiscard]] T *alloc() {
            return static_cast<T *>(m_ctx.alloc_bytes(sizeof(T), m_tag, alignof(T)));
        }

        /**
         * @brief Allocates memory for an array without calling constructors.
         *
         * @param count Number of elements to allocate.
         * @return Pointer to uninitialized memory, or nullptr on failure.
         */
        [[nodiscard]] T *alloc_array(size_t count) {
            return static_cast<T *>(m_ctx.alloc_bytes(sizeof(T) * count, m_tag, alignof(T)));
        }

        /**
         * @brief Frees memory without calling destructor.
         *
         * @param ptr Pointer previously returned by alloc() or alloc_array().
         */
        void free(T *ptr) { m_ctx.free_bytes(ptr); }

        // =====================================================================
        // Allocation with Construction
        // =====================================================================

        /**
         * @brief Allocates and constructs one object using placement new.
         *
         * @tparam Args Constructor argument types.
         * @param args Arguments to forward to T's constructor.
         * @return Pointer to constructed object, or nullptr on allocation failure.
         */
        template <typename... Args> [[nodiscard]] T *create(Args &&...args) {
            T *ptr = alloc();
            if (ptr) {
                new (ptr) T(std::forward<Args>(args)...);
            }
            return ptr;
        }

        /**
         * @brief Destroys and frees one object.
         *
         * Calls the destructor then frees the memory.
         *
         * @param ptr Pointer previously returned by create().
         */
        void destroy(T *ptr) {
            if (ptr) {
                ptr->~T();
                free(ptr);
            }
        }

        // =====================================================================
        // Batch Allocation
        // =====================================================================

        /**
         * @brief Allocates multiple objects into a caller-provided array.
         *
         * @param out Array to store pointers (must have space for count elements).
         * @param count Number of objects to allocate.
         * @return Number of objects actually allocated (may be less on failure).
         */
        size_t alloc_batch(T **out, size_t count) {
            size_t allocated = 0;
            for (size_t i = 0; i < count; ++i) {
                out[i] = alloc();
                if (!out[i])
                    break;
                ++allocated;
            }
            return allocated;
        }

        /**
         * @brief Frees multiple objects.
         *
         * @param ptrs Array of pointers to free.
         * @param count Number of pointers in the array.
         */
        void free_batch(T **ptrs, size_t count) {
            for (size_t i = 0; i < count; ++i) {
                free(ptrs[i]);
            }
        }

        // =====================================================================
        // Introspection
        // =====================================================================

        /**
         * @brief Returns the size of each object.
         */
        static constexpr size_t object_size() { return sizeof(T); }

        /**
         * @brief Returns the alignment of each object.
         */
        static constexpr size_t object_alignment() { return alignof(T); }

        /**
         * @brief Returns the memory tag used by this pool.
         */
        uint8_t tag() const { return m_tag; }

    private:
        Context &m_ctx;
        uint8_t m_tag;
    };

    // =========================================================================
    // Arena Scope Guard (Stack Allocator Pattern)
    // =========================================================================

    /**
     * @brief RAII scope guard for Arena marker-based allocation.
     *
     * Automatically resets the arena to the saved marker on destruction.
     * Use this for temporary allocations within a function scope.
     *
     * Usage:
     * @code
     *     Arena arena(ctx);
     *     {
     *         ArenaScope scope(arena);
     *         auto* temp = arena.alloc<TempData>();
     *         // ... use temp ...
     *     } // temp is automatically freed here
     * @endcode
     */
    class ArenaScope {
    public:
        /**
         * @brief Saves the current arena position.
         */
        explicit ArenaScope(Arena &arena) : m_arena(arena), m_marker(arena.save()) {}

        /**
         * @brief Resets the arena to the saved position.
         */
        ~ArenaScope() { m_arena.reset_to_marker(m_marker); }

        // Non-copyable, non-movable
        ArenaScope(const ArenaScope &) = delete;
        ArenaScope &operator=(const ArenaScope &) = delete;
        ArenaScope(ArenaScope &&) = delete;
        ArenaScope &operator=(ArenaScope &&) = delete;

    private:
        Arena &m_arena;
        Arena::Marker m_marker;
    };

}
