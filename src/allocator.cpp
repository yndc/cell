#include "cell/allocator.h"
#include "cell/cell.h"

#include "tls_cache.h"

#include <cassert>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace Cell {

    Allocator::Allocator(void *base, size_t reserved_size) {
#if defined(_WIN32)
        // Windows VirtualAlloc has 64KB allocation granularity, which guarantees
        // 16KB (kCellSize) alignment. No further alignment needed.
        m_base = base;
        m_reserved_size = reserved_size;
#ifndef NDEBUG
        // Verify our assumption that VirtualAlloc returns aligned addresses
        auto addr = reinterpret_cast<uintptr_t>(base);
        assert((addr & (kCellSize - 1)) == 0 && "VirtualAlloc should return 16KB-aligned address");
#endif
#else
        // Linux: mmap might not align to kCellSize, so align manually
        auto addr = reinterpret_cast<uintptr_t>(base);
        auto aligned_addr = (addr + kCellSize - 1) & kCellMask;
        size_t alignment_offset = aligned_addr - addr;

        m_base = reinterpret_cast<void *>(aligned_addr);
        m_reserved_size = reserved_size > alignment_offset ? reserved_size - alignment_offset : 0;
#endif

        // Calculate number of superblocks we can fit
        m_num_superblocks = m_reserved_size / kSuperblockSize;
        if (m_num_superblocks > kMaxSuperblocks) {
            m_num_superblocks = kMaxSuperblocks;
            m_reserved_size = m_num_superblocks * kSuperblockSize;
        }

        // Initialize all superblocks as uncommitted
        for (size_t i = 0; i < m_num_superblocks; ++i) {
            m_superblock_states[i].store(SuperblockState::kUncommitted, std::memory_order_relaxed);
            m_free_cells[i].store(0, std::memory_order_relaxed);
        }
    }

    Allocator::~Allocator() {
        // Note: We intentionally don't clear t_cache here because on Windows,
        // thread-local destructors may run after this destructor, causing issues.
        // The Context destructor clears the TLS bin caches which is sufficient.
    }

    void *Allocator::alloc() {
        void *result = nullptr;
        bool from_pool = false; // Track if from TLS or global (not fresh from OS)

        // Tier 1: Try TLS cache first (no locks)
        if (!t_cache.is_empty()) {
            result = t_cache.pop();
            from_pool = true;
        }
        // Tier 2: Try global pool (lock-free)
        else if (FreeCell *cell = pop_global()) {
            result = cell;
            from_pool = true;
        }
        // Tier 3: Allocate from OS (count already set in refill_from_os)
        else {
            result = refill_from_os();
            // from_pool stays false - refill_from_os handles accounting
        }

        // Track cell allocation for superblock state
        // Only decrement for tier 1/2; tier 3 sets count correctly already
        if (result && from_pool) {
            size_t sb_idx = get_superblock_index(result);
            if (sb_idx < m_num_superblocks) {
                uint16_t old_free = m_free_cells[sb_idx].fetch_sub(1, std::memory_order_relaxed);
                if (old_free == kCellsPerSuperblock) {
                    m_superblock_states[sb_idx].store(SuperblockState::kInUse,
                                                      std::memory_order_relaxed);
                }
            }
        }

#ifndef NDEBUG
        if (result) {
            auto *header = static_cast<CellHeader *>(result);
            header->magic = kCellMagic;
        }
#endif

        return result;
    }

    void Allocator::free(void *ptr) {
        if (!ptr)
            return;

#ifndef NDEBUG
        auto *header = static_cast<CellHeader *>(ptr);
        assert(header->magic != kCellFreeMagic && "Double-free detected!");
        assert(header->magic == kCellMagic && "Freeing invalid or corrupted cell!");
        header->magic = kCellFreeMagic;
        header->generation++;
#endif

        // Track cell free for superblock state
        size_t sb_idx = get_superblock_index(ptr);
        if (sb_idx < m_num_superblocks) {
            uint16_t new_free = m_free_cells[sb_idx].fetch_add(1, std::memory_order_relaxed) + 1;
            // Mark as free if all cells are now free
            if (new_free == kCellsPerSuperblock) {
                m_superblock_states[sb_idx].store(SuperblockState::kFree,
                                                  std::memory_order_relaxed);
            }
        }

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
        while (!t_cache.is_empty()) {
            push_global(t_cache.pop());
        }
    }

    size_t Allocator::decommit_unused() {
        std::lock_guard<std::mutex> lock(m_decommit_mutex);
        size_t total_freed = 0;

        for (size_t i = 0; i < m_num_superblocks; ++i) {
            if (m_superblock_states[i].load(std::memory_order_relaxed) == SuperblockState::kFree) {
                void *sb_addr = static_cast<char *>(m_base) + i * kSuperblockSize;

#if defined(_WIN32)
                if (VirtualFree(sb_addr, kSuperblockSize, MEM_DECOMMIT)) {
                    m_superblock_states[i].store(SuperblockState::kDecommitted,
                                                 std::memory_order_relaxed);
                    total_freed += kSuperblockSize;
                }
#else
                if (madvise(sb_addr, kSuperblockSize, MADV_DONTNEED) == 0) {
                    m_superblock_states[i].store(SuperblockState::kDecommitted,
                                                 std::memory_order_relaxed);
                    total_freed += kSuperblockSize;
                }
#endif
            }
        }

        return total_freed;
    }

    size_t Allocator::committed_bytes() const {
        size_t committed = 0;
        for (size_t i = 0; i < m_num_superblocks; ++i) {
            SuperblockState state = m_superblock_states[i].load(std::memory_order_relaxed);
            if (state == SuperblockState::kInUse || state == SuperblockState::kFree) {
                committed += kSuperblockSize;
            }
        }
        return committed;
    }

    size_t Allocator::get_superblock_index(void *ptr) const {
        auto addr = reinterpret_cast<uintptr_t>(ptr);
        auto base_addr = reinterpret_cast<uintptr_t>(m_base);
        if (addr < base_addr)
            return m_num_superblocks; // Invalid
        return (addr - base_addr) / kSuperblockSize;
    }

    bool Allocator::recommit_superblock(size_t index) {
        if (index >= m_num_superblocks)
            return false;
        if (m_superblock_states[index].load(std::memory_order_relaxed) !=
            SuperblockState::kDecommitted)
            return true;

        void *sb_addr = static_cast<char *>(m_base) + index * kSuperblockSize;

#if defined(_WIN32)
        if (!VirtualAlloc(sb_addr, kSuperblockSize, MEM_COMMIT, PAGE_READWRITE)) {
            return false;
        }
#else
        if (mprotect(sb_addr, kSuperblockSize, PROT_READ | PROT_WRITE) != 0) {
            return false;
        }
#endif

        m_superblock_states[index].store(SuperblockState::kInUse, std::memory_order_relaxed);
        return true;
    }

    void *Allocator::refill_from_global() { return pop_global(); }

    void *Allocator::refill_from_os() {
        // Find the next uncommitted superblock
        size_t sb_idx = m_committed_end.load(std::memory_order_relaxed) / kSuperblockSize;

        // Check if we need to recommit a decommitted superblock first
        // (This handles the case where we decommitted and need to reuse)
        for (size_t i = 0; i < m_num_superblocks; ++i) {
            if (m_superblock_states[i].load(std::memory_order_relaxed) ==
                SuperblockState::kDecommitted) {
                if (recommit_superblock(i)) {
                    // Re-carve this superblock
                    void *sb_addr = static_cast<char *>(m_base) + i * kSuperblockSize;
                    auto *base_ptr = static_cast<char *>(sb_addr);

                    // Reset free count (we're about to hand out all cells)
                    m_free_cells[i].store(kCellsPerSuperblock - 1, std::memory_order_relaxed);

                    for (size_t j = 1; j < kCellsPerSuperblock; ++j) {
                        auto *cell = reinterpret_cast<FreeCell *>(base_ptr + j * kCellSize);
                        push_global(cell);
                    }

                    return sb_addr;
                }
            }
        }

        // Atomically claim a new superblock
        size_t current_end = m_committed_end.load(std::memory_order_relaxed);
        size_t new_end;

        do {
            new_end = current_end + kSuperblockSize;
            if (new_end > m_reserved_size) {
                return nullptr;
            }
        } while (!m_committed_end.compare_exchange_weak(
            current_end, new_end, std::memory_order_acq_rel, std::memory_order_relaxed));

        sb_idx = current_end / kSuperblockSize;
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

        // Mark superblock as in-use
        m_superblock_states[sb_idx].store(SuperblockState::kInUse, std::memory_order_relaxed);
        m_free_cells[sb_idx].store(kCellsPerSuperblock - 1, std::memory_order_relaxed);

        // Carve superblock into cells, push all but one to global pool
        auto *base_ptr = static_cast<char *>(superblock_start);

        for (size_t i = 1; i < kCellsPerSuperblock; ++i) {
            auto *cell = reinterpret_cast<FreeCell *>(base_ptr + i * kCellSize);
            push_global(cell);
        }

        return superblock_start;
    }

    void Allocator::push_global(FreeCell *c) {
        FreeCell *old_head = m_global_head.load(std::memory_order_relaxed);
        do {
            c->next = old_head;
        } while (!m_global_head.compare_exchange_weak(old_head, c, std::memory_order_release,
                                                      std::memory_order_relaxed));
    }

    FreeCell *Allocator::pop_global() {
        FreeCell *old_head = m_global_head.load(std::memory_order_acquire);
        while (old_head) {
            FreeCell *new_head = old_head->next;
            if (m_global_head.compare_exchange_weak(old_head, new_head, std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
                return old_head;
            }
        }
        return nullptr;
    }

}
