// Seed for the asan-ubsan leg, UBSan half (tests/gates/leg_matrix.sh). The largest int32 plus a
// volatile one overflows at run time (no constant folding, so no compile-time diagnostic). With
// -fno-sanitize-recover=all UBSan aborts; every other leg wraps silently and stays green.
#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

TEST(SeedUbsan, SignedOverflow) {
    const volatile std::int32_t one = 1;
    const std::int32_t sum = std::numeric_limits<std::int32_t>::max() + one;
    volatile std::int32_t sink = sum;
    EXPECT_EQ(sink, sum);
}
