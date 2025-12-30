#include "cell/buddy.h"

#include <algorithm>
#include <cassert>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace Cell {

    // =========================================================================
    // Construction / Destruction
    // =========================================================================

    BuddyAllocator::BuddyAllocator(void *base, size_t reserved_size)
        : m_base(base), m_reserved_size(reserved_size) {
        // Initialize free lists
        for (size_t i = 0; i < kNumOrders; ++i) {
            m_free_lists[i] = nullptr;
        }
    }

    BuddyAllocator::~BuddyAllocator() {
        // Memory is managed by the caller (Context)
        // We don't decommit here
    }

    // =========================================================================
    // Allocation
    // =========================================================================

    void *BuddyAllocator::alloc(size_t size) {
        if (size == 0)
            return nullptr;

        // Account for header
        size_t total_size = size + sizeof(BlockHeader);
        size_t order = size_to_order(total_size);

        if (order > kMaxOrder) {
            return nullptr; // Too large for buddy
        }

        while (true) {
            std::lock_guard<std::mutex> lock(m_lock);

            // Find smallest order with a free block
            for (size_t o = order; o <= kMaxOrder; ++o) {
                size_t list_idx = o - kMinOrder;

                if (m_free_lists[list_idx]) {
                    // Found one! Pop it from free list
                    FreeBlock *block = m_free_lists[list_idx];
                    remove_from_free_list(block, o);

                    // Split down to requested order
                    while (o > order) {
                        --o;
                        size_t block_size = size_t{1} << o;
                        void *buddy = static_cast<char *>(static_cast<void *>(block)) + block_size;

                        // Add upper half (buddy) to free list
                        add_to_free_list(buddy, o);
                    }

                    // Set up header and return user pointer
                    BlockHeader *header = static_cast<BlockHeader *>(static_cast<void *>(block));
                    header->order = static_cast<uint8_t>(order);
                    std::memset(header->reserved, 0, sizeof(header->reserved));

                    size_t alloc_size = size_t{1} << order;
                    m_allocated += alloc_size;

                    return to_user_ptr(block);
                }
            }

            // No free blocks, allocate new superblock from OS
            // Note: grow() is called while holding the lock, which is fine
            // because it doesn't call alloc()
            if (!grow()) {
                return nullptr;
            }
            // Loop again to retry with the new superblock
        }
    }

    void BuddyAllocator::free(void *user_ptr) {
        if (!user_ptr)
            return;

        void *internal_ptr = to_internal_ptr(user_ptr);
        BlockHeader *header = get_block_header(user_ptr);
        size_t order = header->order;

        assert(order >= kMinOrder && order <= kMaxOrder && "Invalid block order");

        size_t block_size = size_t{1} << order;
        m_allocated -= block_size;

        std::lock_guard<std::mutex> lock(m_lock);

        void *ptr = internal_ptr;

        // Try to merge with buddy
        while (order < kMaxOrder) {
            void *buddy = get_buddy(ptr, order);

            // Check if buddy is in our committed range
            if (buddy < m_base || buddy >= static_cast<char *>(m_base) + m_committed) {
                break;
            }

            // Check if buddy is free (it would be in the free list)
            // We do this by checking if it's in the free list for this order
            bool buddy_is_free = false;
            FreeBlock *buddy_block = static_cast<FreeBlock *>(buddy);

            // Simple check: scan the free list for this order
            for (FreeBlock *b = m_free_lists[order - kMinOrder]; b; b = b->next) {
                if (b == buddy_block) {
                    buddy_is_free = true;
                    break;
                }
            }

            if (!buddy_is_free)
                break;

            // Remove buddy from free list
            remove_from_free_list(buddy_block, order);

            // Merge: use lower address as new block
            ptr = std::min(ptr, buddy);
            ++order;
        }

        // Add merged block to free list
        add_to_free_list(ptr, order);
    }

    // =========================================================================
    // Introspection
    // =========================================================================

    bool BuddyAllocator::owns(void *ptr) const {
        return ptr >= m_base && ptr < static_cast<char *>(m_base) + m_committed;
    }

    size_t BuddyAllocator::bytes_allocated() const {
        return m_allocated.load(std::memory_order_relaxed);
    }

    size_t BuddyAllocator::bytes_committed() const {
        return m_committed.load(std::memory_order_relaxed);
    }

    size_t BuddyAllocator::superblock_count() const { return m_superblock_count; }

    // =========================================================================
    // Internal Methods
    // =========================================================================

    size_t BuddyAllocator::size_to_order(size_t size) {
        if (size <= kMinBlockSize)
            return kMinOrder;

        // Find smallest power of 2 >= size
        size_t order = kMinOrder;
        size_t block_size = kMinBlockSize;
        while (block_size < size && order < kMaxOrder) {
            block_size <<= 1;
            ++order;
        }
        return order;
    }

    bool BuddyAllocator::grow() {
        size_t new_end = m_committed + kMaxBlockSize;
        if (new_end > m_reserved_size) {
            return false; // No more reserved space
        }

        void *commit_addr = static_cast<char *>(m_base) + m_committed;

#ifdef _WIN32
        void *result = VirtualAlloc(commit_addr, kMaxBlockSize, MEM_COMMIT, PAGE_READWRITE);
        if (!result)
            return false;
#else
        // On Linux, we use mmap with MAP_FIXED to commit already-reserved pages
        // Or if using overcommit, the pages are committed on first touch
        // For now, assume the memory is already accessible (reserved with PROT_READ|PROT_WRITE)
        // Just touch the first page to ensure it's committed
        static_cast<volatile char *>(commit_addr)[0] = 0;
#endif

        m_committed += kMaxBlockSize;
        ++m_superblock_count;

        // Add the new 2MB block to the max-order free list
        add_to_free_list(commit_addr, kMaxOrder);

        return true;
    }

    void BuddyAllocator::add_to_free_list(void *ptr, size_t order) {
        size_t list_idx = order - kMinOrder;
        FreeBlock *block = static_cast<FreeBlock *>(ptr);

        // Add to head of doubly-linked list
        block->prev = nullptr;
        block->next = m_free_lists[list_idx];

        if (m_free_lists[list_idx]) {
            m_free_lists[list_idx]->prev = block;
        }

        m_free_lists[list_idx] = block;
    }

    void BuddyAllocator::remove_from_free_list(FreeBlock *block, size_t order) {
        size_t list_idx = order - kMinOrder;

        if (block->prev) {
            block->prev->next = block->next;
        } else {
            m_free_lists[list_idx] = block->next;
        }

        if (block->next) {
            block->next->prev = block->prev;
        }
    }

    void *BuddyAllocator::get_buddy(void *ptr, size_t order) const {
        size_t block_size = size_t{1} << order;
        uintptr_t offset = static_cast<char *>(ptr) - static_cast<char *>(m_base);
        uintptr_t buddy_offset = offset ^ block_size;
        return static_cast<char *>(m_base) + buddy_offset;
    }

    void *BuddyAllocator::to_user_ptr(void *internal) {
        return static_cast<char *>(internal) + sizeof(BlockHeader);
    }

    void *BuddyAllocator::to_internal_ptr(void *user) {
        return static_cast<char *>(user) - sizeof(BlockHeader);
    }

    BuddyAllocator::BlockHeader *BuddyAllocator::get_block_header(void *user) {
        return static_cast<BlockHeader *>(to_internal_ptr(user));
    }

}
