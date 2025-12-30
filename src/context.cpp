#include "cell/cell.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace Cell {

    Context::Context(const Config &config) : m_reserved_size(config.reserve_size) {
#if defined(_WIN32)
        m_base = VirtualAlloc(nullptr, m_reserved_size, MEM_RESERVE, PAGE_NOACCESS);
#else
        m_base = mmap(nullptr, m_reserved_size, PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (m_base == MAP_FAILED) {
            m_base = nullptr;
        }
#endif
    }

    Context::~Context() {
        if (m_base) {
#if defined(_WIN32)
            VirtualFree(m_base, 0, MEM_RELEASE);
#else
            munmap(m_base, m_reserved_size);
#endif
        }
    }

    CellData *Context::alloc(MemoryTag tag) {
        // TODO: Implement pool/TLS allocation
        // For now, return nullptr (stub)
        (void)tag;
        return nullptr;
    }

    void Context::free(CellData *cell) {
        // TODO: Implement pool/TLS return
        (void)cell;
    }

} // namespace Cell
