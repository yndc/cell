#pragma once

#include <cstddef>
#include <cstdint>

namespace Cell {

/// Log2 of the Cell size. Default is 14 (16KB).
/// Must be a power of 2. Minimum is 12 (4KB, standard page size).
static constexpr size_t kCellSizeLog2 = 14;
static constexpr size_t kCellSize = 1ULL << kCellSizeLog2;
static constexpr uintptr_t kCellMask = ~(kCellSize - 1);

static_assert(kCellSize >= 4096,
              "Cell size must be at least 4KB (standard page size)");
static_assert((kCellSize & (kCellSize - 1)) == 0,
              "Cell size must be a power of 2");

/// Configuration for creating a Context.
struct Config {
  /// Total address space to reserve in bytes.
  /// Default: 16GB. Physical RAM is committed lazily.
  size_t reserve_size = 16ULL * 1024 * 1024 * 1024;
};

} // namespace Cell
