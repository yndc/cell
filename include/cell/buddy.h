#pragma once

#include "config.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace Cell {

    /**
     * @brief Power-of-2 buddy allocator for medium-sized allocations (32KB - 2MB).
     *
     * Uses buddy system for efficient splitting and coalescing of blocks.
     * All allocations are contiguous and safe for array indexing.
     *
     * Thread safety: Protected by internal mutex.
     */
    class BuddyAllocator {
    public:
        // =====================================================================
        // Constants
        // =====================================================================

        /** @brief Minimum order: 2^15 = 32KB */
        static constexpr size_t kMinOrder = 15;

        /** @brief Maximum order: 2^21 = 2MB (superblock size) */
        static constexpr size_t kMaxOrder = 21;

        /** @brief Number of orders: 15, 16, 17, 18, 19, 20, 21 = 7 orders */
        static constexpr size_t kNumOrders = kMaxOrder - kMinOrder + 1;

        /** @brief Minimum block size: 32KB */
        static constexpr size_t kMinBlockSize = size_t{1} << kMinOrder;

        /** @brief Maximum block size / superblock size: 2MB */
        static constexpr size_t kMaxBlockSize = size_t{1} << kMaxOrder;

        // =====================================================================
        // Construction
        // =====================================================================

        /**
         * @brief Creates a buddy allocator with reserved virtual memory.
         *
         * @param base Base address of reserved memory region.
         * @param reserved_size Total reserved size (should be multiple of 2MB).
         */
        BuddyAllocator(void *base, size_t reserved_size);

        ~BuddyAllocator();

        // Non-copyable, non-movable
        BuddyAllocator(const BuddyAllocator &) = delete;
        BuddyAllocator &operator=(const BuddyAllocator &) = delete;
        BuddyAllocator(BuddyAllocator &&) = delete;
        BuddyAllocator &operator=(BuddyAllocator &&) = delete;

        // =====================================================================
        // Allocation
        // =====================================================================

        /**
         * @brief Allocates a block of at least the requested size.
         *
         * Size is rounded up to the next power-of-2 >= 32KB.
         *
         * @param size Requested size in bytes.
         * @return Pointer to allocated memory, or nullptr on failure.
         */
        [[nodiscard]] void *alloc(size_t size);

        /**
         * @brief Frees a previously allocated block.
         *
         * @param ptr Pointer returned by alloc().
         */
        void free(void *ptr);

        // =====================================================================
        // Introspection
        // =====================================================================

        /**
         * @brief Checks if a pointer is within this allocator's range.
         */
        [[nodiscard]] bool owns(void *ptr) const;

        /**
         * @brief Returns total bytes currently allocated.
         */
        [[nodiscard]] size_t bytes_allocated() const;

        /**
         * @brief Returns total bytes committed from OS.
         */
        [[nodiscard]] size_t bytes_committed() const;

        /**
         * @brief Returns number of superblocks in use.
         */
        [[nodiscard]] size_t superblock_count() const;

    private:
        // =====================================================================
        // Internal Types
        // =====================================================================

        /**
         * @brief Intrusive free list node.
         */
        struct FreeBlock {
            FreeBlock *next;
            FreeBlock *prev;
        };

        /**
         * @brief Metadata for a 2MB superblock.
         *
         * Each superblock tracks which blocks are allocated using a bitmap.
         * We use a simple scheme: store the order in a small header before
         * returning the pointer to the user.
         */
        struct BlockHeader {
            uint8_t order; ///< Allocation order (15-21)
            uint8_t reserved[7];
        };

        static_assert(sizeof(BlockHeader) == 8, "BlockHeader should be 8 bytes");

        // =====================================================================
        // Members
        // =====================================================================

        void *m_base;                       ///< Base of reserved region
        size_t m_reserved_size;             ///< Total reserved size
        std::atomic<size_t> m_committed{0}; ///< Bytes committed from OS
        std::atomic<size_t> m_allocated{0}; ///< Bytes currently allocated
        size_t m_superblock_count{0};       ///< Number of superblocks

        FreeBlock *m_free_lists[kNumOrders]{}; ///< Free list per order
        std::mutex m_lock;                     ///< Protects free lists

        // =====================================================================
        // Internal Methods
        // =====================================================================

        /**
         * @brief Converts size to order (index into free list).
         */
        static size_t size_to_order(size_t size);

        /**
         * @brief Allocates a new superblock from reserved memory.
         */
        bool grow();

        /**
         * @brief Adds a block to a free list.
         */
        void add_to_free_list(void *ptr, size_t order);

        /**
         * @brief Removes a block from its free list.
         */
        void remove_from_free_list(FreeBlock *block, size_t order);

        /**
         * @brief Gets the buddy address for a block.
         */
        void *get_buddy(void *ptr, size_t order) const;

        /**
         * @brief Gets the user pointer from internal pointer (after header).
         */
        static void *to_user_ptr(void *internal);

        /**
         * @brief Gets the internal pointer from user pointer.
         */
        static void *to_internal_ptr(void *user);

        /**
         * @brief Gets the header for a user pointer.
         */
        static BlockHeader *get_block_header(void *user);
    };

}
