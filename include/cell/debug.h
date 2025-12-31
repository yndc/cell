#pragma once

#include <cstddef>
#include <cstdint>

/**
 * @file debug.h
 * @brief Debug feature configuration and utilities for the Cell library.
 *
 * Compile-time flags:
 * - CELL_DEBUG_GUARDS: Enable guard bytes before/after allocations
 * - CELL_DEBUG_STACKTRACE: Capture stack trace on allocation
 * - CELL_DEBUG_LEAKS: Track all allocations for leak detection
 *
 * All features have zero overhead when disabled.
 */

namespace Cell {

    // =========================================================================
    // Guard Bytes Configuration
    // =========================================================================

#ifdef CELL_DEBUG_GUARDS
    /** @brief Guard byte pattern to detect buffer overflows/underflows. */
    static constexpr uint8_t kGuardPattern = 0xAB;

    /** @brief Number of guard bytes placed before and after each allocation. */
    static constexpr size_t kGuardSize = 16;
#endif

    // =========================================================================
    // Stack Trace Configuration
    // =========================================================================

#ifdef CELL_DEBUG_STACKTRACE
    /** @brief Maximum stack frames to capture per allocation. */
    static constexpr size_t kMaxStackDepth = 16;

    /**
     * @brief Captures the current call stack.
     *
     * Platform support:
     * - Linux/glibc: backtrace()
     * - macOS/iOS: backtrace()
     * - Android: _Unwind_Backtrace()
     * - Windows: CaptureStackBackTrace()
     *
     * @param buffer Array to store stack frame addresses.
     * @param max_depth Maximum frames to capture.
     * @param skip_frames Number of frames to skip from the top (default: 1 for this function).
     * @return Number of frames actually captured.
     */
    size_t capture_stack(void **buffer, size_t max_depth, size_t skip_frames = 1);

    /**
     * @brief Prints a captured stack trace to stderr.
     *
     * @param stack Array of stack frame addresses.
     * @param depth Number of frames in the stack.
     */
    void print_stack(void *const *stack, size_t depth);
#endif

    // =========================================================================
    // Leak Detection Structures
    // =========================================================================

#ifdef CELL_DEBUG_LEAKS
    /**
     * @brief Debug information stored for each tracked allocation.
     */
    struct DebugAllocation {
        void *ptr;   ///< User-visible pointer.
        size_t size; ///< Requested size in bytes.
        uint8_t tag; ///< Application-defined tag.
#ifdef CELL_DEBUG_STACKTRACE
        void *stack[kMaxStackDepth]; ///< Captured stack trace.
        size_t stack_depth;          ///< Number of valid stack frames.
#endif
    };
#endif

} // namespace Cell
