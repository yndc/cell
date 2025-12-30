#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace Cell {

#ifdef CELL_ENABLE_STATS

    /**
     * @brief Memory statistics for tracking allocations.
     *
     * All counters are atomic for thread-safe updates.
     * Only compiled when CELL_ENABLE_STATS is defined.
     */
    struct MemoryStats {
        // =====================================================================
        // Global Counters
        // =====================================================================

        std::atomic<size_t> total_allocated{0};   ///< Cumulative bytes allocated
        std::atomic<size_t> total_freed{0};       ///< Cumulative bytes freed
        std::atomic<size_t> current_allocated{0}; ///< Currently allocated bytes
        std::atomic<size_t> peak_allocated{0};    ///< Peak allocated bytes

        // =====================================================================
        // Per-Allocator Counters
        // =====================================================================

        std::atomic<size_t> cell_allocs{0};    ///< Full cell allocations
        std::atomic<size_t> cell_frees{0};     ///< Full cell frees
        std::atomic<size_t> subcell_allocs{0}; ///< Sub-cell allocations
        std::atomic<size_t> subcell_frees{0};  ///< Sub-cell frees
        std::atomic<size_t> buddy_allocs{0};   ///< Buddy allocations
        std::atomic<size_t> buddy_frees{0};    ///< Buddy frees
        std::atomic<size_t> large_allocs{0};   ///< Large (>2MB) allocations
        std::atomic<size_t> large_frees{0};    ///< Large frees

        // =====================================================================
        // Per-Tag Tracking
        // =====================================================================

        std::array<std::atomic<size_t>, 256> per_tag_current{}; ///< Current bytes per tag

        // =====================================================================
        // Methods
        // =====================================================================

        /**
         * @brief Records an allocation.
         */
        void record_alloc(size_t size, uint8_t tag) {
            total_allocated.fetch_add(size, std::memory_order_relaxed);
            size_t current = current_allocated.fetch_add(size, std::memory_order_relaxed) + size;

            // Update peak (relaxed is fine, approximate is okay)
            size_t peak = peak_allocated.load(std::memory_order_relaxed);
            while (current > peak) {
                if (peak_allocated.compare_exchange_weak(peak, current,
                                                         std::memory_order_relaxed)) {
                    break;
                }
            }

            per_tag_current[tag].fetch_add(size, std::memory_order_relaxed);
        }

        /**
         * @brief Records a deallocation.
         */
        void record_free(size_t size, uint8_t tag) {
            total_freed.fetch_add(size, std::memory_order_relaxed);
            current_allocated.fetch_sub(size, std::memory_order_relaxed);
            per_tag_current[tag].fetch_sub(size, std::memory_order_relaxed);
        }

        /**
         * @brief Resets all counters.
         */
        void reset() {
            total_allocated = 0;
            total_freed = 0;
            current_allocated = 0;
            peak_allocated = 0;
            cell_allocs = 0;
            cell_frees = 0;
            subcell_allocs = 0;
            subcell_frees = 0;
            buddy_allocs = 0;
            buddy_frees = 0;
            large_allocs = 0;
            large_frees = 0;
            for (auto &tag : per_tag_current) {
                tag = 0;
            }
        }

        /**
         * @brief Prints stats to stdout.
         */
        void dump() const {
            printf("=== Cell Memory Stats ===\n");
            printf("Total allocated:   %zu bytes\n", total_allocated.load());
            printf("Total freed:       %zu bytes\n", total_freed.load());
            printf("Current allocated: %zu bytes\n", current_allocated.load());
            printf("Peak allocated:    %zu bytes\n", peak_allocated.load());
            printf("\n");
            printf("Cell allocs/frees:    %zu / %zu\n", cell_allocs.load(), cell_frees.load());
            printf("SubCell allocs/frees: %zu / %zu\n", subcell_allocs.load(),
                   subcell_frees.load());
            printf("Buddy allocs/frees:   %zu / %zu\n", buddy_allocs.load(), buddy_frees.load());
            printf("Large allocs/frees:   %zu / %zu\n", large_allocs.load(), large_frees.load());

            // Print non-zero tags
            bool has_tags = false;
            for (size_t i = 0; i < 256; ++i) {
                size_t val = per_tag_current[i].load();
                if (val > 0) {
                    if (!has_tags) {
                        printf("\nPer-tag current:\n");
                        has_tags = true;
                    }
                    printf("  Tag %3zu: %zu bytes\n", i, val);
                }
            }
        }
    };

#endif // CELL_ENABLE_STATS

}
