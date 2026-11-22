// Seed for the debug leg (tests/gates/leg_matrix.sh). An assert on a function result that is false
// at run time (the input is volatile, so neither the compiler nor clang-tidy can see it). NDEBUG
// removes the assert, so release stays green. The sanitizer legs are Debug builds with assertions
// on and redden too; the matrix records whether debug has a defect only it catches.
#include <cassert>
#include <cstdint>

#include <gtest/gtest.h>

namespace {

std::int32_t successor(std::int32_t value) { return value + 1; }

} // namespace

TEST(SeedDebug, AssertFalseAtRunTime) {
    const volatile std::int32_t input = 3;
    const std::int32_t result = successor(input);
    assert(result == 5);
    EXPECT_EQ(result, 4);
}
