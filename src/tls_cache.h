#pragma once

#include "cell/allocator.h"
#include "cell/config.h"

namespace Cell {

    /**
     * @brief Per-thread cell cache.
     *
     * Fixed-size array, no locking required.
     */
    struct TlsCache {
        FreeCell *cells[kTlsCacheCapacity] = {};
        size_t count = 0;

        [[nodiscard]] bool is_empty() const { return count == 0; }
        [[nodiscard]] bool is_full() const { return count >= kTlsCacheCapacity; }

        void push(FreeCell *c) { cells[count++] = c; }
        [[nodiscard]] FreeCell *pop() { return cells[--count]; }
    };

    /** @brief Thread-local cache instance. */
    inline thread_local TlsCache t_cache;

}
