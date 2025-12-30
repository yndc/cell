#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace Cell {

    /**
     * @brief Registry for large allocations (>2MB) that go directly to OS.
     *
     * These allocations bypass the buddy system and are allocated/freed
     * directly via mmap/VirtualAlloc. Optionally uses huge pages.
     *
     * Thread safety: Protected by internal mutex.
     */
    class LargeAllocRegistry {
    public:
        // =====================================================================
        // Constants
        // =====================================================================

        /** @brief Minimum size for large allocations (matches buddy max) */
        static constexpr size_t kMinLargeSize = 2 * 1024 * 1024; // 2MB

        /** @brief Alignment for large allocations */
        static constexpr size_t kLargeAlignment = 2 * 1024 * 1024; // 2MB

        // =====================================================================
        // Construction
        // =====================================================================

        LargeAllocRegistry() = default;
        ~LargeAllocRegistry();

        // Non-copyable, non-movable
        LargeAllocRegistry(const LargeAllocRegistry &) = delete;
        LargeAllocRegistry &operator=(const LargeAllocRegistry &) = delete;
        LargeAllocRegistry(LargeAllocRegistry &&) = delete;
        LargeAllocRegistry &operator=(LargeAllocRegistry &&) = delete;

        // =====================================================================
        // Allocation
        // =====================================================================

        /**
         * @brief Allocates a large block directly from the OS.
         *
         * @param size Size in bytes (should be >= kMinLargeSize).
         * @param tag Memory tag for profiling.
         * @param try_huge_pages Attempt to use huge pages if available.
         * @return Pointer to allocated memory, or nullptr on failure.
         */
        [[nodiscard]] void *alloc(size_t size, uint8_t tag = 0, bool try_huge_pages = true);

        /**
         * @brief Frees a previously allocated large block.
         *
         * @param ptr Pointer returned by alloc().
         */
        void free(void *ptr);

        // =====================================================================
        // Introspection
        // =====================================================================

        /**
         * @brief Checks if a pointer was allocated by this registry.
         */
        [[nodiscard]] bool owns(void *ptr) const;

        /**
         * @brief Returns total bytes currently allocated.
         */
        [[nodiscard]] size_t bytes_allocated() const;

        /**
         * @brief Returns number of active allocations.
         */
        [[nodiscard]] size_t allocation_count() const;

    private:
        /**
         * @brief Metadata for a large allocation.
         */
        struct LargeAlloc {
            size_t size;
            uint8_t tag;
            bool huge_pages;
        };

        std::unordered_map<void *, LargeAlloc> m_allocs;
        mutable std::mutex m_lock;
        size_t m_total_allocated{0};
    };

}
