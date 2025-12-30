#pragma once

#include "config.h"

#include <atomic>

namespace Cell {

    /**
     * @brief A free cell node for the lock-free stack.
     *
     * Stored inline in the cell's memory when it's free.
     */
    struct FreeCell {
        FreeCell *next;
    };

    /**
     * @brief Multi-tier memory allocator.
     *
     * Tier 1: Thread-local cache (no locks)
     * Tier 2: Global atomic stack (lock-free)
     * Tier 3: OS superblock allocation
     */
    class Allocator {
    public:
        /**
         * @brief Creates an allocator managing the given reserved range.
         * @param base Start of the reserved virtual address space.
         * @param reserved_size Total reserved bytes.
         */
        explicit Allocator(void *base, size_t reserved_size);

        ~Allocator();

        // Non-copyable, non-movable
        Allocator(const Allocator &) = delete;
        Allocator &operator=(const Allocator &) = delete;
        Allocator(Allocator &&) = delete;
        Allocator &operator=(Allocator &&) = delete;

        /**
         * @brief Allocates a cell (Tier 1 → 2 → 3).
         * @return Pointer to an aligned cell, or nullptr on failure.
         */
        [[nodiscard]] void *alloc();

        /**
         * @brief Returns a cell to the TLS cache or global pool.
         * @param cell Pointer to the cell to free.
         */
        void free(void *cell);

        /**
         * @brief Flushes the thread-local cache to the global pool.
         *
         * This should be called before a thread exits or if memory needs to be
         * returned to the global pool for other threads to use.
         */
        void flush_tls_cache();

    private:
        void *refill_from_global();    ///< Tier 2 → Tier 1
        void *refill_from_os();        ///< Tier 3 → Tier 2 → Tier 1
        void push_global(FreeCell *c); ///< Lock-free push to global
        FreeCell *pop_global();        ///< Lock-free pop from global

        void *m_base;                                   ///< Start of reserved range.
        size_t m_reserved_size;                         ///< Total reserved bytes.
        std::atomic<size_t> m_committed_end{0};         ///< High-water mark for commits.
        std::atomic<FreeCell *> m_global_head{nullptr}; ///< Lock-free stack head.
    };

}
