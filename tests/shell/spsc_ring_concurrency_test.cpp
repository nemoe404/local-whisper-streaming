#include "lws/spsc_ring.hpp"
#include "ring_test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <thread>
#include <tuple>
#include <vector>

namespace {

using lws::test::bits_of;
using lws::test::pattern;

// Walks `total` samples in spans whose sizes come from size_at(span index), the last one clipped to
// what is left, and calls emit(span index, first sample, size) once per span.
template <typename SizeAt, typename Emit>
void for_each_span(std::size_t total, SizeAt size_at, Emit emit) {
    std::size_t first = 0;
    std::ignore = std::ranges::find_if(std::views::iota(std::size_t{0}), [&](std::size_t index) {
        const std::size_t size = std::min(size_at(index), total - first);
        emit(index, first, size);
        first += size;
        return first == total;
    });
}

// Span sizes cycle through 1 to 211.
std::size_t cycling_size(std::size_t index) { return (index % 211) + 1; }

// The retry lives here in the test, never in the ring.
void push_until_accepted(lws::SpscRing& ring, std::span<const float> samples) {
    std::ignore = std::ranges::find_if(std::views::iota(0), [&](std::int32_t) {
        if (ring.push(samples).has_value()) {
            return true;
        }
        std::this_thread::yield();
        return false;
    });
}

} // namespace

// RING-04, success criterion 1: one producer and one consumer move two million samples through a
// small ring in order and bit for bit. The consumer calls nothing but pop and stops on a count: a
// completion flag or a counters() call would add a happens-before edge from the producer and could
// hide a weakened release store. Worker threads call no gtest macro: GoogleTest is not
// TSan-instrumented, and the assertions run on the main thread after both workers have joined.
TEST(SpscRingConcurrency, LosslessStreamArrivesInOrderBitExact) {
    constexpr std::size_t kTotal = 2'000'000;
    constexpr std::size_t kCapacity = 1021; // prime: neither span size nor pop size divides it
    constexpr std::size_t kPopSize = 97;
    lws::SpscRing ring{kCapacity};
    const std::vector<float> source = pattern(0, kTotal);
    std::vector<float> received;
    received.reserve(kTotal);
    std::vector<float> buffer(kPopSize);
    {
        const std::jthread producer{[&] {
            for_each_span(kTotal, cycling_size,
                          [&](std::size_t, std::size_t first, std::size_t size) {
                              push_until_accepted(ring, std::span{source}.subspan(first, size));
                          });
        }};
        const std::jthread consumer{[&] {
            std::ignore = std::ranges::find_if(std::views::iota(0), [&](std::int32_t) {
                const std::size_t popped = ring.pop(buffer);
                received.append_range(std::span{buffer}.first(popped));
                if (popped == 0) {
                    std::this_thread::yield();
                }
                return received.size() == kTotal;
            });
        }};
    }
    EXPECT_EQ(bits_of(received), bits_of(source));
    const lws::RingCounters counters = ring.counters();
    EXPECT_EQ(counters.accepted, kTotal);
    EXPECT_EQ(counters.accepted + counters.dropped, counters.offered);
}
