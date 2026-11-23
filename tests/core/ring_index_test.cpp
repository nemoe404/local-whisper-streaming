#include "lws/ring_index.hpp"

#include <gtest/gtest.h>

// CORE-01: equal counters, nothing used.
TEST(RingIndexEmpty, FreeSpaceIsCapacityWhenNothingUsed) {
    const lws::RingIndex ring{8};
    EXPECT_EQ(ring.used(5, 5), 0U);
    EXPECT_EQ(ring.free_space(ring.used(5, 5)), 8U);
}

// CORE-01: equal wrapped indices, but the counters differ by the capacity.
TEST(RingIndexFull, FreeSpaceIsZeroWhenFull) {
    const lws::RingIndex ring{8};
    EXPECT_EQ(ring.used(3, 11), 8U);
    EXPECT_EQ(ring.free_space(ring.used(3, 11)), 0U);
}

// CORE-01: a position wraps modulo the capacity.
TEST(RingIndexWrap, PositionsWrapModuloCapacity) {
    const lws::RingIndex ring{8};
    EXPECT_EQ(ring.wrap(7), 7U);
    EXPECT_EQ(ring.wrap(8), 0U);
    EXPECT_EQ(ring.wrap(19), 3U);
}

// CORE-01: the wrapped write index sits behind the wrapped read index.
TEST(RingIndexWrapped, WrappedOccupancyCountsTheGap) {
    const lws::RingIndex ring{8};
    EXPECT_EQ(ring.wrap(10), 2U);
    EXPECT_EQ(ring.wrap(6), 6U);
    // The type derives the occupancy: 10 - 6 = 4 used, 8 - 4 = 4 free.
    EXPECT_EQ(ring.used(6, 10), 4U);
    EXPECT_EQ(ring.free_space(ring.used(6, 10)), 4U);
}

// CORE-01: the constructed capacity is reported back.
TEST(RingIndexCapacity, ReportsConstructedCapacity) {
    const lws::RingIndex ring{4096};
    EXPECT_EQ(ring.capacity(), 4096U);
}
