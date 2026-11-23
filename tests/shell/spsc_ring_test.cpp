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

// Push `count` patterned samples starting at pattern index `first`.
bool push_pattern(lws::SpscRing& ring, std::uint64_t first, std::size_t count) {
    const std::vector<float> samples = pattern(first, count);
    return ring.push(samples).has_value();
}

// Move the ring's indices forward by `count` without leaving samples behind.
void advance(lws::SpscRing& ring, std::size_t count) {
    ASSERT_TRUE(push_pattern(ring, 0, count));
    std::vector<float> sink(count);
    ASSERT_EQ(ring.pop(sink), count);
}

std::vector<float> pop_all(lws::SpscRing& ring, std::size_t count) {
    std::vector<float> out(count);
    out.resize(ring.pop(out));
    return out;
}

} // namespace

// A push followed by a pop returns the same samples in order.
TEST(SpscRingBasic, PushThenPopReturnsSamplesInOrder) {
    lws::SpscRing ring{8};
    const std::vector<float> in = pattern(0, 5);
    ASSERT_TRUE(ring.push(in).has_value());
    std::vector<float> out(5);
    EXPECT_EQ(ring.pop(out), 5U);
    EXPECT_EQ(bits_of(out), bits_of(in));
}

TEST(SpscRingBasic, PopOnEmptyReturnsZero) {
    lws::SpscRing ring{8};
    std::vector<float> out(4);
    EXPECT_EQ(ring.pop(out), 0U);
}

TEST(SpscRingBasic, PopIntoEmptySpanReturnsZero) {
    lws::SpscRing ring{8};
    ASSERT_TRUE(push_pattern(ring, 0, 3));
    EXPECT_EQ(ring.pop(std::span<float>{}), 0U);
    EXPECT_EQ(ring.available(), 3U);
}

TEST(SpscRingBasic, PartialPopLeavesRemainderInOrder) {
    lws::SpscRing ring{8};
    const std::vector<float> in = pattern(0, 6);
    ASSERT_TRUE(ring.push(in).has_value());
    std::vector<float> head(4);
    ASSERT_EQ(ring.pop(head), 4U);
    std::vector<float> tail(4);
    ASSERT_EQ(ring.pop(tail), 2U);
    tail.resize(2);
    const std::vector<std::uint32_t> expected = bits_of(in);
    EXPECT_EQ(bits_of(head), std::vector<std::uint32_t>(expected.begin(), expected.begin() + 4));
    EXPECT_EQ(bits_of(tail), std::vector<std::uint32_t>(expected.begin() + 4, expected.end()));
}

TEST(SpscRingBasic, AvailableTracksPushAndPop) {
    lws::SpscRing ring{8};
    EXPECT_EQ(ring.available(), 0U);
    ASSERT_TRUE(push_pattern(ring, 0, 6));
    EXPECT_EQ(ring.available(), 6U);
    std::vector<float> out(4);
    ASSERT_EQ(ring.pop(out), 4U);
    EXPECT_EQ(ring.available(), 2U);
}

TEST(SpscRingBasic, EmptySpanIsAccepted) {
    lws::SpscRing ring{8};
    EXPECT_TRUE(ring.push(std::span<const float>{}).has_value());
    EXPECT_EQ(ring.available(), 0U);
}

TEST(SpscRingBasic, SpanEqualToFreeSpaceIsAccepted) {
    lws::SpscRing ring{8};
    ASSERT_TRUE(push_pattern(ring, 0, 3));
    EXPECT_TRUE(push_pattern(ring, 3, 5));
    EXPECT_EQ(ring.available(), 8U);
}

TEST(SpscRingBasic, SpanOneLargerThanFreeSpaceIsRejectedAndRingUnchanged) {
    lws::SpscRing ring{8};
    const std::vector<float> first = pattern(0, 3);
    ASSERT_TRUE(ring.push(first).has_value());
    const std::vector<float> rejected = pattern(100, 6);
    const std::expected<void, lws::Error> result = ring.push(rejected);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), lws::Error::Overrun);
    EXPECT_EQ(ring.available(), 3U);
    const std::vector<float> out = pop_all(ring, 8);
    EXPECT_EQ(bits_of(out), bits_of(first));
}

TEST(SpscRingBasic, SpanLongerThanCapacityIsRejectedOnEmptyRing) {
    lws::SpscRing ring{8};
    const std::vector<float> rejected = pattern(0, 9);
    const std::expected<void, lws::Error> result = ring.push(rejected);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), lws::Error::Overrun);
    EXPECT_EQ(ring.available(), 0U);
}

TEST(SpscRingBasic, CapacityOneRoundTrips) {
    lws::SpscRing ring{1};
    ASSERT_TRUE(push_pattern(ring, 0, 1));
    EXPECT_FALSE(push_pattern(ring, 1, 1));
    EXPECT_EQ(bits_of(pop_all(ring, 1)), bits_of(pattern(0, 1)));
    EXPECT_TRUE(push_pattern(ring, 2, 1));
    EXPECT_EQ(bits_of(pop_all(ring, 1)), bits_of(pattern(2, 1)));
}

TEST(SpscRingBasic, ContractIsCompileTime) {
    static_assert(
        noexcept(std::declval<lws::SpscRing&>().push(std::declval<std::span<const float>>())));
    static_assert(noexcept(std::declval<lws::SpscRing&>().pop(std::declval<std::span<float>>())));
    static_assert(alignof(lws::SpscRing) >= 64);
    static_assert(!std::is_copy_constructible_v<lws::SpscRing> &&
                  !std::is_move_constructible_v<lws::SpscRing>);
    SUCCEED();
}

TEST(SpscRingStraddle, WrapSplitsTheCopyAndKeepsOrder) {
    const std::array<std::size_t, 3> capacities{8, 10, 7};
    std::vector<std::size_t> failures;
    std::ranges::for_each(capacities, [&failures](std::size_t capacity) {
        lws::SpscRing ring{capacity};
        advance(ring, 6);
        const std::vector<float> in = pattern(10, 5);
        if (!ring.push(in).has_value() || bits_of(pop_all(ring, 5)) != bits_of(in)) {
            failures.push_back(capacity);
        }
    });
    EXPECT_TRUE(failures.empty());
}

TEST(SpscRingStraddle, SpanEndingExactlyAtTheEndNeedsNoSecondCopy) {
    lws::SpscRing ring{8};
    advance(ring, 3);
    const std::vector<float> in = pattern(20, 5);
    ASSERT_TRUE(ring.push(in).has_value());
    EXPECT_EQ(bits_of(pop_all(ring, 5)), bits_of(in));
}

TEST(SpscRingStraddle, EveryOffsetAndLengthRoundTrips) {
    const std::array<std::size_t, 6> capacities{1, 2, 3, 7, 8, 10};
    std::vector<std::tuple<std::size_t, std::size_t, std::size_t>> failures;
    std::ranges::for_each(capacities, [&failures](std::size_t capacity) {
        std::ranges::for_each(std::views::iota(std::size_t{0}, capacity), [&](std::size_t offset) {
            std::ranges::for_each(
                std::views::iota(std::size_t{1}, capacity + 1), [&](std::size_t length) {
                    lws::SpscRing ring{capacity};
                    std::vector<float> sink(offset);
                    const std::vector<float> filler = pattern(0, offset);
                    const bool moved = ring.push(filler).has_value() && ring.pop(sink) == offset;
                    const std::vector<float> in = pattern(offset, length);
                    const bool ok = moved && ring.push(in).has_value() &&
                                    bits_of(pop_all(ring, length)) == bits_of(in);
                    if (!ok) {
                        failures.emplace_back(capacity, offset, length);
                    }
                });
        });
    });
    EXPECT_TRUE(failures.empty());
}

TEST(SpscRingStraddle, SpecialFloatBitsSurviveTheCopy) {
    lws::SpscRing ring{5};
    advance(ring, 3);
    const std::array<std::uint32_t, 7> patterns = lws::test::special_bits();
    std::vector<float> in(patterns.size());
    std::ranges::transform(patterns, in.begin(),
                           [](std::uint32_t bits) { return std::bit_cast<float>(bits); });
    // Five slots: push the first five patterns across the end, then the remaining two.
    const std::span<const float> all{in};
    const std::span<const float> head = all.first(5);
    ASSERT_TRUE(ring.push(head).has_value());
    const std::vector<float> first = pop_all(ring, 5);
    const std::span<const float> tail = all.subspan(5);
    ASSERT_TRUE(ring.push(tail).has_value());
    const std::vector<float> second = pop_all(ring, 2);
    std::vector<std::uint32_t> got = bits_of(first);
    const std::vector<std::uint32_t> second_bits = bits_of(second);
    got.insert(got.end(), second_bits.begin(), second_bits.end());
    EXPECT_EQ(got, std::vector<std::uint32_t>(patterns.begin(), patterns.end()));
}
