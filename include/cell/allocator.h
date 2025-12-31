#pragma once

#include "config.h"

#include <atomic>
#include <cstdint>
#include <mutex>

namespace Cell {

    /**
     * @brief State of a superblock for memory management.
     */
    enum class SuperblockState : uint8_t {
        kUncommitted, ///< Never used, no physical pages allocated.
        kInUse,       ///< Has at least one allocated cell.
        kFree,        ///< All cells free, physical pages still committed.
        kDecommitted  ///< All cells free, physical pages released to OS.
    };

    /**
     * @brief A free cell node for the lock-free stack.
     *
     * Stored inline in the cell's memory when it's free.
     */
    struct FreeCell {
        FreeCell *next;
    };

    /**
     * @brief Multi-tier memory allocator with memory decommit support.
     *
     * Tier 1: Thread-local cache (no locks)
     * Tier 2: Global atomic stack (lock-free)
     * Tier 3: OS superblock allocation
     */
    class Allocator {
    public:
        /** @brief Maximum superblocks supported (for 8GB reserved = 4096 superblocks). */
        static constexpr size_t kMaxSuperblocks = 8192;

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
         */
        void flush_tls_cache();

        /**
         * @brief Decommits all fully-free superblocks.
         * @return Number of bytes released to the OS.
         */
        size_t decommit_unused();

        /**
         * @brief Returns currently committed physical memory.
         */
        [[nodiscard]] size_t committed_bytes() const;

    private:
        void *refill_from_global();    ///< Tier 2 → Tier 1
        void *refill_from_os();        ///< Tier 3 → Tier 2 → Tier 1
        void push_global(FreeCell *c); ///< Lock-free push to global
        FreeCell *pop_global();        ///< Lock-free pop from global

        size_t get_superblock_index(void *ptr) const;
        bool recommit_superblock(size_t index);

        void *m_base;                                   ///< Start of reserved range.
        size_t m_reserved_size;                         ///< Total reserved bytes.
        std::atomic<size_t> m_committed_end{0};         ///< High-water mark for commits.
        std::atomic<FreeCell *> m_global_head{nullptr}; ///< Lock-free stack head.

        // Superblock tracking for decommit
        size_t m_num_superblocks{0}; ///< Total superblocks possible.
        std::atomic<SuperblockState>
            m_superblock_states[kMaxSuperblocks]{};            ///< Per-superblock state.
        std::atomic<uint16_t> m_free_cells[kMaxSuperblocks]{}; ///< Free cell count per superblock.
        std::mutex m_decommit_mutex;                           ///< Protects decommit operations.
    };

}
