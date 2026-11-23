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

// Push a patterned span and add its size to the tally of what the result says happened to it.
void offer_and_tally(lws::SpscRing& ring, std::size_t first, std::size_t size,
                     lws::RingCounters& tally) {
    tally.offered += size;
    const bool accepted = ring.push(pattern(first, size)).has_value();
    (accepted ? tally.accepted : tally.dropped) += size;
}

} // namespace

// A span that does not fit is rejected whole, counted, and flagged.
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

TEST(SpscRingOverrun, RejectedSpanNeverEntersBetweenAcceptedOnes) {
    lws::SpscRing ring{10};
    const std::vector<float> a = pattern(0, 4);
    const std::vector<float> b = pattern(100, 8);
    const std::vector<float> c = pattern(4, 3);
    ASSERT_TRUE(ring.push(a).has_value());
    const std::expected<void, lws::Error> rejected = ring.push(b);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error(), lws::Error::Overrun);
    ASSERT_TRUE(ring.push(c).has_value());
    const std::vector<float> out = pop_all(ring, 16);
    EXPECT_EQ(out.size(), 7U);
    EXPECT_EQ(bits_of(out), bits_of(pattern(0, 7)));
}

TEST(SpscRingOverrun, SpanLongerThanCapacityIsRejectedAndCounted) {
    lws::SpscRing ring{8};
    const std::vector<float> in = pattern(0, 9);
    const std::expected<void, lws::Error> result = ring.push(in);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), lws::Error::Overrun);
    const lws::RingCounters counters = ring.counters();
    EXPECT_EQ(counters.offered, 9U);
    EXPECT_EQ(counters.accepted, 0U);
    EXPECT_EQ(counters.dropped, 9U);
    EXPECT_TRUE(ring.take_discontinuity());
}

TEST(SpscRingOverrun, LaterSpanThatFitsIsAcceptedAfterARejection) {
    lws::SpscRing ring{8};
    ASSERT_TRUE(ring.push(pattern(0, 6)).has_value());
    ASSERT_FALSE(ring.push(pattern(6, 5)).has_value());
    ASSERT_TRUE(ring.push(pattern(6, 2)).has_value());
    EXPECT_EQ(bits_of(pop_all(ring, 8)), bits_of(pattern(0, 8)));
    const lws::RingCounters counters = ring.counters();
    EXPECT_EQ(counters.offered, 13U);
    EXPECT_EQ(counters.accepted, 8U);
    EXPECT_EQ(counters.dropped, 5U);
}

TEST(SpscRingOverrun, EmptySpanChangesNoCounter) {
    lws::SpscRing ring{8};
    ASSERT_TRUE(ring.push(pattern(0, 3)).has_value());
    const lws::RingCounters before = ring.counters();
    ASSERT_TRUE(ring.push(std::span<const float>{}).has_value());
    const lws::RingCounters after = ring.counters();
    EXPECT_EQ(after.offered, before.offered);
    EXPECT_EQ(after.accepted, before.accepted);
    EXPECT_EQ(after.dropped, before.dropped);
    EXPECT_FALSE(ring.take_discontinuity());
}

// With no pop at all, accepted + dropped == offered, and each equals the test's tally.
TEST(SpscRingCounters, StalledConsumerCountersAddUp) {
    lws::SpscRing ring{64};
    lws::RingCounters tally;
    std::ranges::for_each(std::views::iota(std::size_t{0}, std::size_t{40}),
                          [&](std::size_t i) { offer_and_tally(ring, i, (i % 13) + 1, tally); });
    const lws::RingCounters counters = ring.counters();
    EXPECT_EQ(counters.offered, tally.offered);
    EXPECT_EQ(counters.accepted, tally.accepted);
    EXPECT_EQ(counters.dropped, tally.dropped);
    EXPECT_EQ(counters.accepted + counters.dropped, counters.offered);
    EXPECT_LE(counters.accepted, 64U);
    EXPECT_GT(counters.dropped, 0U);
}

TEST(SpscRingCounters, CountersAreInSamples) {
    lws::SpscRing ring{16};
    ASSERT_TRUE(ring.push(pattern(0, 5)).has_value());
    const lws::RingCounters counters = ring.counters();
    EXPECT_EQ(counters.offered, 5U);
    EXPECT_EQ(counters.accepted, 5U);
    EXPECT_EQ(counters.dropped, 0U);
}

TEST(SpscRingCounters, PopDoesNotChangeCounters) {
    lws::SpscRing ring{16};
    ASSERT_TRUE(ring.push(pattern(0, 9)).has_value());
    const lws::RingCounters before = ring.counters();
    std::vector<float> out(4);
    ASSERT_EQ(ring.pop(out), 4U);
    const lws::RingCounters after = ring.counters();
    EXPECT_EQ(after.offered, before.offered);
    EXPECT_EQ(after.accepted, before.accepted);
    EXPECT_EQ(after.dropped, before.dropped);
}

TEST(SpscRingCounters, AcceptedTracksPublishedSamplesAcrossWraps) {
    lws::SpscRing ring{7};
    std::vector<float> sink(5);
    std::size_t round_trips = 0;
    std::ranges::for_each(std::views::iota(std::size_t{0}, std::size_t{50}), [&](std::size_t i) {
        const bool pushed = ring.push(pattern(i * 5, 5)).has_value();
        round_trips += (pushed && ring.pop(sink) == 5U) ? 1U : 0U;
    });
    EXPECT_EQ(round_trips, 50U);
    const lws::RingCounters counters = ring.counters();
    EXPECT_EQ(counters.accepted, 250U);
    EXPECT_GT(counters.accepted, ring.capacity());
    EXPECT_EQ(counters.dropped, 0U);
    EXPECT_EQ(counters.offered, 250U);
}

TEST(SpscRingFlag, FlagIsClearOnANewRing) {
    lws::SpscRing ring{8};
    EXPECT_FALSE(ring.take_discontinuity());
}

TEST(SpscRingFlag, OverrunSetsFlagAndTakeClearsIt) {
    lws::SpscRing ring{4};
    ASSERT_FALSE(ring.push(pattern(0, 5)).has_value());
    EXPECT_TRUE(ring.take_discontinuity());
    EXPECT_FALSE(ring.take_discontinuity());
}

// The flag is level-triggered: accepted pushes and pops in between do not clear it.
TEST(SpscRingFlag, FlagStaysSetUntilTakenAcrossAcceptedPushes) {
    lws::SpscRing ring{8};
    ASSERT_FALSE(ring.push(pattern(0, 9)).has_value());
    ASSERT_TRUE(ring.push(pattern(0, 3)).has_value());
    EXPECT_EQ(pop_all(ring, 8).size(), 3U);
    ASSERT_TRUE(ring.push(pattern(3, 3)).has_value());
    EXPECT_TRUE(ring.take_discontinuity());
}

TEST(SpscRingFlag, LaterOverrunSetsFlagAgain) {
    lws::SpscRing ring{4};
    ASSERT_FALSE(ring.push(pattern(0, 5)).has_value());
    EXPECT_TRUE(ring.take_discontinuity());
    EXPECT_FALSE(ring.take_discontinuity());
    ASSERT_FALSE(ring.push(pattern(0, 5)).has_value());
    EXPECT_TRUE(ring.take_discontinuity());
}

TEST(SpscRingFlag, AcceptedPushesAndPopsNeverSetFlag) {
    lws::SpscRing ring{16};
    std::vector<float> sink(16);
    std::ranges::for_each(std::views::iota(std::size_t{0}, std::size_t{100}), [&](std::size_t i) {
        ASSERT_TRUE(ring.push(pattern(i, (i % 16) + 1)).has_value());
        ASSERT_GT(ring.pop(sink), 0U);
    });
    EXPECT_FALSE(ring.take_discontinuity());
}

TEST(SpscRingFlag, FlagImpliesDroppedSamples) {
    lws::SpscRing ring{12};
    std::vector<float> sink(12);
    std::size_t takes_true = 0;
    std::ranges::for_each(std::views::iota(std::size_t{0}, std::size_t{60}), [&](std::size_t i) {
        // Sizes 1 to 9 against capacity 12: some pushes fit, some overrun while the ring fills.
        std::ignore = ring.push(pattern(i, (i % 9) + 1));
        if (i % 4 == 3) {
            std::ignore = ring.pop(sink);
        }
        if (ring.take_discontinuity()) {
            ++takes_true;
            EXPECT_GE(ring.counters().dropped, 1U);
        }
    });
    EXPECT_GT(takes_true, 0U);
}
