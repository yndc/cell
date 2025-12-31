#include "cell/large.h"

#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <malloc.h> // For _aligned_malloc, _aligned_free
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace Cell {

    // =========================================================================
    // Destruction
    // =========================================================================

    LargeAllocRegistry::~LargeAllocRegistry() {
        // Free all remaining allocations
        std::lock_guard<std::mutex> lock(m_lock);
        for (auto &[ptr, alloc] : m_allocs) {
#ifdef _WIN32
            VirtualFree(ptr, 0, MEM_RELEASE);
#else
            munmap(ptr, alloc.size);
#endif
        }
        m_allocs.clear();
    }

    // =========================================================================
    // Allocation
    // =========================================================================

    void *LargeAllocRegistry::alloc(size_t size, uint8_t tag, bool try_huge_pages) {
        if (size == 0)
            return nullptr;

        void *ptr = nullptr;
        bool used_huge = false;

#ifdef _WIN32
        // Windows: Try large pages first if requested
        if (try_huge_pages && size >= kMinLargeSize) {
            // Note: MEM_LARGE_PAGES requires SeLockMemoryPrivilege
            ptr = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE | MEM_LARGE_PAGES,
                               PAGE_READWRITE);
            if (ptr) {
                used_huge = true;
            }
        }
        if (!ptr) {
            ptr = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        }
#else
        // Linux: Try huge pages first if requested
        if (try_huge_pages && size >= kMinLargeSize) {
            ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
            if (ptr != MAP_FAILED) {
                used_huge = true;
            } else {
                ptr = nullptr;
            }
        }
        if (!ptr) {
            ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (ptr == MAP_FAILED) {
                ptr = nullptr;
            }
        }
#endif

        if (ptr) {
            std::lock_guard<std::mutex> lock(m_lock);
            m_allocs[ptr] = LargeAlloc{size, ptr, tag, used_huge, false};
            m_total_allocated += size;
        }

        return ptr;
    }

    void LargeAllocRegistry::free(void *ptr) {
        if (!ptr)
            return;

        std::lock_guard<std::mutex> lock(m_lock);
        auto it = m_allocs.find(ptr);
        if (it == m_allocs.end()) {
            return; // Not our allocation
        }

        size_t size = it->second.size;
        void *original = it->second.original_ptr;
        bool aligned = it->second.aligned;
        m_allocs.erase(it);
        m_total_allocated -= size;

#ifdef _WIN32
        if (aligned) {
            _aligned_free(original);
        } else {
            VirtualFree(original, 0, MEM_RELEASE);
        }
#else
        if (aligned) {
            ::free(original); // posix_memalign uses standard free
        } else {
            munmap(original, size);
        }
#endif
    }

    void *LargeAllocRegistry::alloc_aligned(size_t size, size_t alignment, uint8_t tag) {
        if (size == 0 || alignment == 0) {
            return nullptr;
        }

        // Validate alignment is power of 2
        if ((alignment & (alignment - 1)) != 0) {
            return nullptr;
        }

        void *ptr = nullptr;

#ifdef _WIN32
        ptr = _aligned_malloc(size, alignment);
#else
        // posix_memalign requires alignment to be at least sizeof(void*)
        // and a power of 2
        size_t min_align = sizeof(void *);
        if (alignment < min_align) {
            alignment = min_align;
        }
        if (posix_memalign(&ptr, alignment, size) != 0) {
            ptr = nullptr;
        }
#endif

        if (ptr) {
            std::lock_guard<std::mutex> lock(m_lock);
            m_allocs[ptr] = LargeAlloc{size, ptr, tag, false, true};
            m_total_allocated += size;
        }

        return ptr;
    }

    // =========================================================================
    // Introspection
    // =========================================================================

    bool LargeAllocRegistry::owns(void *ptr) const {
        std::lock_guard<std::mutex> lock(m_lock);
        return m_allocs.find(ptr) != m_allocs.end();
    }

    size_t LargeAllocRegistry::bytes_allocated() const {
        std::lock_guard<std::mutex> lock(m_lock);
        return m_total_allocated;
    }

    size_t LargeAllocRegistry::allocation_count() const {
        std::lock_guard<std::mutex> lock(m_lock);
        return m_allocs.size();
    }

}
