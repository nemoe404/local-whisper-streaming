#include "lws/spsc_ring.hpp"
#include "ring_test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <ranges>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using lws::test::bits_of;
using lws::test::pattern;

std::vector<float> pop_all(lws::SpscRing& ring, std::size_t count) {
    std::vector<float> out(count);
    out.resize(ring.pop(out));
    return out;
}

} // namespace

// RING-01, RING-02: a span that does not fit is rejected whole, counted, and flagged.
TEST(SpscRingOverrun, SpanThatDoesNotFitIsRejectedWholeAndCounted) {
    lws::SpscRing ring{8};
    const std::vector<float> accepted = pattern(0, 6);
    const std::vector<float> rejected = pattern(6, 3);
    ASSERT_TRUE(ring.push(accepted).has_value());
    const std::expected<void, lws::Error> result = ring.push(rejected);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), lws::Error::Overrun);
    const lws::RingCounters counters = ring.counters();
    EXPECT_EQ(counters.offered, 9U);
    EXPECT_EQ(counters.accepted, 6U);
    EXPECT_EQ(counters.dropped, 3U);
    EXPECT_TRUE(ring.take_discontinuity());
    EXPECT_FALSE(ring.take_discontinuity());
    EXPECT_EQ(bits_of(pop_all(ring, 8)), bits_of(accepted));
}
