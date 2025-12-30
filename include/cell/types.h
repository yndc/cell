#pragma once

#include <cstdint>

namespace Cell {

/// Tags for memory profiling and subsystem identification.
enum class MemoryTag : uint8_t {
  Unknown = 0,
  General,
  // Add application-specific tags here
};

/// Header stored at the beginning of each Cell.
/// Contains metadata for profiling and management.
struct CellHeader {
  MemoryTag tag;
  // Reserved for future use (alignment, generation counters, etc.)
  uint8_t reserved[7];
};

/// A fixed-size, aligned memory unit.
/// The usable payload starts after the CellHeader.
struct CellData {
  CellHeader header;
  // Remaining bytes are available for allocation
};

} // namespace Cell
