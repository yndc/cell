#include "cell/context.h"

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
        if (m_base) {
            m_allocator = std::make_unique<Allocator>(m_base, m_reserved_size);
        }
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

    CellData *Context::alloc(uint8_t tag) {
        if (!m_allocator)
            return nullptr;

        void *ptr = m_allocator->alloc();
        if (!ptr)
            return nullptr;

        auto *cell = static_cast<CellData *>(ptr);
        cell->header.tag = tag;
        return cell;
    }

    void Context::free(CellData *cell) {
        if (m_allocator && cell) {
            m_allocator->free(cell);
        }
    }

}
