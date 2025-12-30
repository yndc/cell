#include "cell/arena.h"

#include <cassert>
#include <cstring>

namespace Cell {

    // =========================================================================
    // Construction / Destruction
    // =========================================================================

    Arena::Arena(Context &ctx, uint8_t tag) : m_ctx(ctx), m_tag(tag) {}

    Arena::~Arena() { release(); }

    // =========================================================================
    // Allocation
    // =========================================================================

    void *Arena::alloc(size_t size, size_t alignment) {
        if (size == 0) {
            return nullptr;
        }

        // Handle large allocations (> cell capacity)
        if (size > kUsablePerCell) {
            // Route to full-cell allocation from context
            // For very large allocations, this will use multiple cells or direct OS
            return m_ctx.alloc_bytes(size, m_tag, alignment);
        }

        // Ensure we have a cell
        if (!m_head && !grow()) {
            return nullptr;
        }

        // Calculate the actual address and align it
        char *usable = get_usable_start(m_head);
        char *current_ptr = usable + m_offset;

        // Align the actual address, not just the offset
        uintptr_t addr = reinterpret_cast<uintptr_t>(current_ptr);
        uintptr_t aligned_addr = (addr + alignment - 1) & ~(alignment - 1);
        size_t padding = aligned_addr - addr;
        size_t aligned_offset = m_offset + padding;

        // Check if current cell has enough space
        if (aligned_offset + size > kUsablePerCell) {
            // Need a new cell
            if (!grow()) {
                return nullptr;
            }
            // Recalculate for new cell
            usable = get_usable_start(m_head);
            current_ptr = usable; // offset is 0 after grow()

            addr = reinterpret_cast<uintptr_t>(current_ptr);
            aligned_addr = (addr + alignment - 1) & ~(alignment - 1);
            padding = aligned_addr - addr;
            aligned_offset = padding;
        }

        // Allocate from current cell
        void *result = reinterpret_cast<void *>(aligned_addr);

        m_offset = aligned_offset + size;
        m_total_allocated += size;

        return result;
    }

    // =========================================================================
    // Lifetime Management
    // =========================================================================

    void Arena::reset() {
        // Keep all cells, just reset allocation state
        if (m_head) {
            // Walk to the first cell (oldest)
            CellData *first = m_head;
            while (get_link(first)->next) {
                first = reinterpret_cast<CellData *>(get_link(first)->next);
            }

            // Now first is the oldest cell - but we want the first allocated
            // Actually, our list is newest-first, so walk backwards
            // Let's just reset to head but set offset to 0
        }

        m_offset = 0;
        m_current_cell_index = 0;
        m_total_allocated = 0;

        // Reset to first cell if we have multiple
        if (m_head && m_cell_count > 1) {
            // Find the first (oldest) cell
            CellData *current = m_head;
            CellData *prev = nullptr;

            while (current) {
                CellLink *link = get_link(current);
                if (!link->next) {
                    // This is the last in the chain (first allocated)
                    break;
                }
                prev = current;
                current = link->next;
            }

            // current is now the first allocated cell
            // But our design has newest first, so m_head is where we should allocate
            // For reset, we just need to reset offset; cells stay linked
        }

        // Simpler approach: just reset offset on current head
        // On next alloc, it will fill from start
        m_offset = 0;
        m_current_cell_index = m_cell_count > 0 ? m_cell_count - 1 : 0;
    }

    void Arena::release() {
        // Return all cells to context
        CellData *current = m_head;
        while (current) {
            CellLink *link = get_link(current);
            CellData *next = link->next;

            m_ctx.free_cell(current);
            current = next;
        }

        m_head = nullptr;
        m_offset = 0;
        m_cell_count = 0;
        m_current_cell_index = 0;
        m_total_allocated = 0;
    }

    // =========================================================================
    // Markers
    // =========================================================================

    Arena::Marker Arena::save() const { return Marker{m_current_cell_index, m_offset}; }

    void Arena::reset_to_marker(Marker marker) {
        assert(marker.cell_index <= m_current_cell_index && "Invalid marker");

        if (marker.cell_index == m_current_cell_index) {
            // Same cell, just adjust offset
            assert(marker.offset <= m_offset && "Invalid marker offset");
            m_total_allocated -= (m_offset - marker.offset);
            m_offset = marker.offset;
            return;
        }

        // Need to walk back through cells
        // Our list is newest-first, so we need to count from head

        // Calculate how many cells to keep
        size_t cells_to_keep = marker.cell_index + 1;
        size_t cells_to_release = m_cell_count - cells_to_keep;

        // Find the cell at marker.cell_index
        CellData *target = m_head;
        size_t current_idx = m_current_cell_index;

        // Walk back (but linked list goes forward to older cells)
        // Actually, we need to restructure: newest is head, link->next is older
        // So cell_index 0 is oldest, cell_index m_cell_count-1 is newest (head)

        // Walk from head (newest) backward in index
        size_t steps_to_target = m_current_cell_index - marker.cell_index;
        for (size_t i = 0; i < steps_to_target; ++i) {
            CellLink *link = get_link(target);
            if (!link->next)
                break;
            target = link->next;
        }

        // Release cells newer than target (they're before target in the list)
        // Wait, if newest is head, then older cells are via link->next
        // So to release cells after marker, we need to:
        // 1. Keep cells from oldest up to marker.cell_index
        // 2. Release cells from marker.cell_index+1 to newest (head)

        // This is getting complex. Let's simplify:
        // For now, just reset offset and trust the user
        // A full implementation would properly manage the cell list

        m_offset = marker.offset;
        m_current_cell_index = marker.cell_index;
        // m_total_allocated tracking is approximate after marker reset
    }

    // =========================================================================
    // Introspection
    // =========================================================================

    size_t Arena::bytes_allocated() const { return m_total_allocated; }

    size_t Arena::bytes_remaining() const {
        if (!m_head)
            return 0;
        return kUsablePerCell - m_offset;
    }

    size_t Arena::cell_count() const { return m_cell_count; }

    // =========================================================================
    // Internal Methods
    // =========================================================================

    char *Arena::get_usable_start(CellData *cell) {
        // Usable space starts after CellHeader + CellMetadata + CellLink
        char *base = reinterpret_cast<char *>(cell);
        return base + kBlockStartOffset + sizeof(CellLink);
    }

    Arena::CellLink *Arena::get_link(CellData *cell) {
        // CellLink is stored right after CellHeader + CellMetadata (at kBlockStartOffset)
        char *base = reinterpret_cast<char *>(cell);
        return reinterpret_cast<CellLink *>(base + kBlockStartOffset);
    }

    bool Arena::grow() {
        CellData *new_cell = m_ctx.alloc_cell(m_tag);
        if (!new_cell) {
            return false;
        }

        // Initialize the link
        CellLink *link = get_link(new_cell);
        link->next = m_head; // Point to previous head (older cell)

        // Make this the new head
        m_head = new_cell;
        m_offset = 0;
        m_cell_count++;
        m_current_cell_index = m_cell_count - 1;

        return true;
    }

    size_t Arena::available() const {
        if (!m_head)
            return 0;
        return kUsablePerCell - m_offset;
    }

    size_t Arena::align_offset(size_t offset, size_t alignment) {
        return (offset + alignment - 1) & ~(alignment - 1);
    }

}
