#include "cell/allocator.h"

#include "tls_cache.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace Cell {

    Allocator::Allocator(void *base, size_t reserved_size)
        : m_base(base), m_reserved_size(reserved_size) {}

    Allocator::~Allocator() {
        // Clear TLS cache to prevent stale pointers after Context destruction
        // Note: This only clears the current thread's cache. Multi-threaded
        // applications should call flush_tls_cache() from each thread before
        // destroying the Context.
        t_cache.count = 0;
    }

    void *Allocator::alloc() {
        // Tier 1: Try TLS cache first (no locks)
        if (!t_cache.is_empty()) {
            return t_cache.pop();
        }

        // Tier 2: Try global pool (lock-free)
        if (FreeCell *cell = pop_global()) {
            return cell;
        }

        // Tier 3: Allocate from OS
        return refill_from_os();
    }

    void Allocator::free(void *ptr) {
        if (!ptr)
            return;

        auto *cell = static_cast<FreeCell *>(ptr);

        // Tier 1: Return to TLS cache if not full
        if (!t_cache.is_full()) {
            t_cache.push(cell);
            return;
        }

        // Tier 2: Return to global pool
        push_global(cell);
    }

    void Allocator::flush_tls_cache() {
        // Move all cells from TLS cache to global pool
        while (!t_cache.is_empty()) {
            push_global(t_cache.pop());
        }
    }

    void *Allocator::refill_from_global() { return pop_global(); }

    void *Allocator::refill_from_os() {
        // Atomically claim a superblock worth of address space
        size_t current_end = m_committed_end.load(std::memory_order_relaxed);
        size_t new_end;

        do {
            new_end = current_end + kSuperblockSize;
            if (new_end > m_reserved_size) {
                return nullptr; // Out of reserved space
            }
        } while (!m_committed_end.compare_exchange_weak(
            current_end, new_end, std::memory_order_acq_rel, std::memory_order_relaxed));

        // Commit the superblock
        void *superblock_start = static_cast<char *>(m_base) + current_end;

#if defined(_WIN32)
        if (!VirtualAlloc(superblock_start, kSuperblockSize, MEM_COMMIT, PAGE_READWRITE)) {
            return nullptr;
        }
#else
        if (mprotect(superblock_start, kSuperblockSize, PROT_READ | PROT_WRITE) != 0) {
            return nullptr;
        }
#endif

        // Carve superblock into cells, push all but one to global pool
        auto *base_ptr = static_cast<char *>(superblock_start);

        for (size_t i = 1; i < kCellsPerSuperblock; ++i) {
            auto *cell = reinterpret_cast<FreeCell *>(base_ptr + i * kCellSize);
            push_global(cell);
        }

        // Return the first cell directly
        return superblock_start;
    }

    void Allocator::push_global(FreeCell *c) {
        // Lock-free push to atomic stack
        FreeCell *old_head = m_global_head.load(std::memory_order_relaxed);
        do {
            c->next = old_head;
        } while (!m_global_head.compare_exchange_weak(old_head, c, std::memory_order_release,
                                                      std::memory_order_relaxed));
    }

    FreeCell *Allocator::pop_global() {
        // Lock-free pop from atomic stack
        FreeCell *old_head = m_global_head.load(std::memory_order_acquire);
        while (old_head) {
            FreeCell *new_head = old_head->next;
            if (m_global_head.compare_exchange_weak(old_head, new_head, std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
                return old_head;
            }
            // old_head is updated by compare_exchange_weak on failure
        }
        return nullptr;
    }

}
