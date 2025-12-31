#include "cell/debug.h"

#ifdef CELL_DEBUG_STACKTRACE

#include <cstdio>

// Platform-specific stack trace implementation
// -----------------------------------------------------------------------------

#if defined(__ANDROID__)
// Android: Use NDK's unwind API
#include <unwind.h>

namespace {
    struct AndroidUnwindState {
        void **buffer;
        size_t max_depth;
        size_t skip_frames;
        size_t current;
    };

    _Unwind_Reason_Code android_unwind_callback(_Unwind_Context *context, void *arg) {
        auto *state = static_cast<AndroidUnwindState *>(arg);

        void *ip = reinterpret_cast<void *>(_Unwind_GetIP(context));
        if (ip == nullptr) {
            return _URC_END_OF_STACK;
        }

        if (state->skip_frames > 0) {
            state->skip_frames--;
            return _URC_NO_REASON;
        }

        if (state->current < state->max_depth) {
            state->buffer[state->current++] = ip;
            return _URC_NO_REASON;
        }

        return _URC_END_OF_STACK;
    }
} // namespace

namespace Cell {
    size_t capture_stack(void **buffer, size_t max_depth, size_t skip_frames) {
        AndroidUnwindState state{buffer, max_depth, skip_frames + 1, 0};
        _Unwind_Backtrace(android_unwind_callback, &state);
        return state.current;
    }

    void print_stack(void *const *stack, size_t depth) {
        std::fprintf(stderr, "  Stack trace (%zu frames):\n", depth);
        for (size_t i = 0; i < depth; ++i) {
            std::fprintf(stderr, "    [%zu] %p\n", i, stack[i]);
        }
        // Note: Android doesn't have backtrace_symbols; use addr2line or ndk-stack
    }
} // namespace Cell

#elif defined(__linux__) || defined(__APPLE__)
// Linux (glibc), macOS, iOS: Use execinfo.h
#include <cstdlib>
#include <execinfo.h>

namespace Cell {
    size_t capture_stack(void **buffer, size_t max_depth, size_t skip_frames) {
        // Capture extra frames to account for skipping
        constexpr size_t kExtraFrames = 8;
        void *temp[128];
        size_t temp_size = max_depth + skip_frames + kExtraFrames;
        if (temp_size > 128) {
            temp_size = 128;
        }

        int captured = backtrace(temp, static_cast<int>(temp_size));
        if (captured <= 0) {
            return 0;
        }

        // Skip the requested frames
        size_t start = skip_frames + 1; // +1 for this function
        if (start >= static_cast<size_t>(captured)) {
            return 0;
        }

        size_t count = static_cast<size_t>(captured) - start;
        if (count > max_depth) {
            count = max_depth;
        }

        for (size_t i = 0; i < count; ++i) {
            buffer[i] = temp[start + i];
        }

        return count;
    }

    void print_stack(void *const *stack, size_t depth) {
        std::fprintf(stderr, "  Stack trace (%zu frames):\n", depth);

        char **symbols = backtrace_symbols(const_cast<void **>(stack), static_cast<int>(depth));
        if (symbols) {
            for (size_t i = 0; i < depth; ++i) {
                std::fprintf(stderr, "    [%zu] %s\n", i, symbols[i]);
            }
            std::free(symbols);
        } else {
            for (size_t i = 0; i < depth; ++i) {
                std::fprintf(stderr, "    [%zu] %p\n", i, stack[i]);
            }
        }
    }
} // namespace Cell

#elif defined(_WIN32)
// Windows: Use CaptureStackBackTrace
#include <windows.h>

namespace Cell {
    size_t capture_stack(void **buffer, size_t max_depth, size_t skip_frames) {
        // CaptureStackBackTrace has a max of 62 frames
        DWORD frames_to_capture = static_cast<DWORD>(max_depth);
        if (frames_to_capture > 62) {
            frames_to_capture = 62;
        }

        DWORD frames_to_skip = static_cast<DWORD>(skip_frames + 1); // +1 for this function

        USHORT captured = CaptureStackBackTrace(frames_to_skip, frames_to_capture, buffer, nullptr);
        return static_cast<size_t>(captured);
    }

    void print_stack(void *const *stack, size_t depth) {
        std::fprintf(stderr, "  Stack trace (%zu frames):\n", depth);
        for (size_t i = 0; i < depth; ++i) {
            std::fprintf(stderr, "    [%zu] %p\n", i, stack[i]);
        }
        // Note: For symbol resolution on Windows, use SymFromAddr from DbgHelp
    }
} // namespace Cell

#else
// Unsupported platform: No-op implementation
namespace Cell {
    size_t capture_stack(void ** /*buffer*/, size_t /*max_depth*/, size_t /*skip_frames*/) {
        return 0;
    }

    void print_stack(void *const * /*stack*/, size_t /*depth*/) {
        std::fprintf(stderr, "  Stack trace: (not supported on this platform)\n");
    }
} // namespace Cell

#endif

#endif // CELL_DEBUG_STACKTRACE
