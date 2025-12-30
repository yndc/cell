#include "cell/cell.h"

#include <gtest/gtest.h>

TEST(ContextTest, CreateAndDestroy) {
  Cell::Config config;
  config.reserve_size = 1024 * 1024; // 1MB for testing

  Cell::Context ctx(config);
  // Context should be created without crashing
  SUCCEED();
}

TEST(ContextTest, GetHeaderAlignment) {
  // Simulate a pointer inside a hypothetical Cell
  uintptr_t fake_cell_base = 0x10000; // 64KB aligned
  uintptr_t ptr_inside = fake_cell_base + 1024;

  auto *header = Cell::get_header(reinterpret_cast<void *>(ptr_inside));

  // The header should snap back to the cell base
  EXPECT_EQ(reinterpret_cast<uintptr_t>(header), fake_cell_base);
}
