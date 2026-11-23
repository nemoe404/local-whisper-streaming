#include "lws/spsc_ring.hpp"
#include "ring_test_support.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

// RING-01, RING-03: a push followed by a pop returns the same samples in order.
TEST(SpscRingBasic, PushThenPopReturnsSamplesInOrder) {
    lws::SpscRing ring{8};
    const std::vector<float> in = lws::test::pattern(0, 5);
    ASSERT_TRUE(ring.push(in).has_value());
    std::vector<float> out(5);
    EXPECT_EQ(ring.pop(out), 5U);
    EXPECT_EQ(lws::test::bits_of(out), lws::test::bits_of(in));
}
