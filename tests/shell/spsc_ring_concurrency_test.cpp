#include "lws/spsc_ring.hpp"
#include "ring_test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <latch>
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

// What the producer knows about one offered span, kept in a container only the producer touches.
struct SpanRecord {
    std::size_t first;
    std::size_t size;
    bool accepted;
};

template <typename Pred> std::uint64_t sum_sizes(const std::vector<SpanRecord>& log, Pred pred) {
    return std::ranges::fold_left(log | std::views::filter(pred) |
                                      std::views::transform(&SpanRecord::size),
                                  std::uint64_t{0}, std::plus<>{});
}

// The samples of the accepted spans, in offer order: what a lossy consumer must receive.
std::vector<float> accepted_stream(std::span<const float> source,
                                   const std::vector<SpanRecord>& log) {
    std::vector<float> stream;
    std::ranges::for_each(log | std::views::filter(&SpanRecord::accepted),
                          [&](const SpanRecord& record) {
                              stream.append_range(source.subspan(record.first, record.size));
                          });
    return stream;
}

// Pops until `done` was seen set and a pop that followed returned nothing, so every sample pushed
// before the producer stored `done` has been handed to `sink`.
template <typename Sink>
void drain_until_done(lws::SpscRing& ring, const std::atomic<bool>& done, std::span<float> buffer,
                      Sink sink) {
    std::ignore = std::ranges::find_if(std::views::iota(0), [&](std::int32_t) {
        const bool finished = done.load(std::memory_order_acquire);
        const std::size_t popped = ring.pop(buffer);
        sink(buffer.first(popped));
        return finished && popped == 0;
    });
}

// The counters must equal what the producer's own log says happened. dropped > 0 is asserted: a
// test that cannot reach the reject path proves nothing.
void expect_counters_match_log(const lws::RingCounters& counters,
                               const std::vector<SpanRecord>& log) {
    EXPECT_EQ(counters.offered, sum_sizes(log, [](const SpanRecord&) { return true; }));
    EXPECT_EQ(counters.accepted, sum_sizes(log, &SpanRecord::accepted));
    EXPECT_EQ(counters.dropped,
              sum_sizes(log, [](const SpanRecord& record) { return !record.accepted; }));
    EXPECT_EQ(counters.accepted + counters.dropped, counters.offered);
    EXPECT_GT(counters.dropped, 0U);
}

// What a monitor thread saw: only the monitor writes it, the main thread reads it after join.
struct SnapshotTally {
    std::uint64_t snapshots = 0;
    std::uint64_t overcounts = 0;
    std::uint64_t regressions = 0;
};

void record_snapshot(SnapshotTally& tally, const lws::RingCounters& previous,
                     const lws::RingCounters& now) {
    ++tally.snapshots;
    tally.overcounts += static_cast<std::uint64_t>(now.accepted + now.dropped > now.offered);
    tally.regressions += static_cast<std::uint64_t>(now.offered < previous.offered ||
                                                    now.accepted < previous.accepted ||
                                                    now.dropped < previous.dropped);
}

} // namespace

// One producer and one consumer move two million samples through a
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
    {
        const std::jthread producer{[&] {
            for_each_span(kTotal, cycling_size,
                          [&](std::size_t, std::size_t first, std::size_t size) {
                              push_until_accepted(ring, std::span{source}.subspan(first, size));
                          });
        }};
        const std::jthread consumer{[&] {
            std::vector<float> buffer(kPopSize);
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

// Under concurrency: the producer never retries. The consumer is held on a latch
// until the producer has offered 200 spans, so overruns are certain. Whatever it then receives is
// exactly the concatenation of the accepted spans, and the counters match the producer's own log.
TEST(SpscRingConcurrency, LossyStreamDeliversExactlyTheAcceptedSpans) {
    constexpr std::size_t kTotal = 400'000;
    constexpr std::size_t kCapacity = 509;
    constexpr std::size_t kSpansBeforeRelease = 200;
    constexpr std::size_t kPopSize = 61;
    lws::SpscRing ring{kCapacity};
    const std::vector<float> source = pattern(0, kTotal);
    std::vector<SpanRecord> log;
    log.reserve(4096);
    std::vector<float> received;
    received.reserve(kTotal);
    std::latch overrun_certain{1};
    std::atomic<bool> producer_done{false};
    {
        const std::jthread producer{[&] {
            for_each_span(kTotal, cycling_size,
                          [&](std::size_t index, std::size_t first, std::size_t size) {
                              const bool accepted =
                                  ring.push(std::span{source}.subspan(first, size)).has_value();
                              log.push_back({.first = first, .size = size, .accepted = accepted});
                              if (index + 1 == kSpansBeforeRelease) {
                                  overrun_certain.count_down();
                              }
                          });
            producer_done.store(true, std::memory_order_release);
        }};
        const std::jthread consumer{[&] {
            overrun_certain.wait();
            std::vector<float> buffer(kPopSize);
            drain_until_done(ring, producer_done, buffer,
                             [&](std::span<const float> popped) { received.append_range(popped); });
        }};
    }
    ASSERT_GT(log.size(), kSpansBeforeRelease);
    EXPECT_EQ(bits_of(received), bits_of(accepted_stream(source, log)));
    expect_counters_match_log(ring.counters(), log);
}

// Under concurrency: a consumer that sees the flag set always finds a non-zero dropped
// count, because the producer stores the count before it sets the flag.
TEST(SpscRingConcurrency, FlagSeenByConsumerImpliesDroppedSamples) {
    constexpr std::size_t kTotal = 600'000;
    constexpr std::size_t kCapacity = 128;
    constexpr std::size_t kPopSize = 4;
    lws::SpscRing ring{kCapacity};
    const std::vector<float> source = pattern(0, kTotal);
    std::atomic<bool> producer_done{false};
    std::uint64_t violations = 0;
    std::uint64_t flags_seen = 0;
    {
        const std::jthread producer{[&] {
            for_each_span(
                kTotal, [](std::size_t index) { return 64 + (index % 33); },
                [&](std::size_t, std::size_t first, std::size_t size) {
                    std::ignore = ring.push(std::span{source}.subspan(first, size));
                });
            producer_done.store(true, std::memory_order_release);
        }};
        const std::jthread consumer{[&] {
            std::vector<float> buffer(kPopSize);
            std::ignore = std::ranges::find_if(std::views::iota(0), [&](std::int32_t) {
                const bool finished = producer_done.load(std::memory_order_acquire);
                if (ring.take_discontinuity()) {
                    ++flags_seen;
                    violations += static_cast<std::uint64_t>(ring.counters().dropped == 0);
                }
                return ring.pop(buffer) == 0 && finished;
            });
        }};
    }
    EXPECT_EQ(violations, 0U);
    const lws::RingCounters counters = ring.counters();
    EXPECT_GT(counters.dropped, 0U); // a producer that never overran proves nothing
    const bool still_pending = ring.take_discontinuity();
    EXPECT_GE(flags_seen + static_cast<std::uint64_t>(still_pending), 1U);
}

// A monitor thread reads counters while the producer overruns and
// the consumer pops never sees a snapshot that over-counts or goes backwards.
TEST(SpscRingConcurrency, CounterSnapshotsNeverOvercount) {
    constexpr std::size_t kTotal = 500'000;
    constexpr std::size_t kCapacity = 256;
    constexpr std::size_t kPopSize = 16;
    constexpr std::uint64_t kMinSnapshots = 100;
    lws::SpscRing ring{kCapacity};
    const std::vector<float> source = pattern(0, kTotal);
    std::atomic<bool> producer_done{false};
    SnapshotTally tally;
    {
        const std::jthread producer{[&] {
            for_each_span(
                kTotal, [](std::size_t index) { return 40 + (index % 61); },
                [&](std::size_t, std::size_t first, std::size_t size) {
                    std::ignore = ring.push(std::span{source}.subspan(first, size));
                });
            producer_done.store(true, std::memory_order_release);
        }};
        const std::jthread consumer{[&] {
            std::vector<float> buffer(kPopSize);
            drain_until_done(ring, producer_done, buffer, [](std::span<const float>) {});
        }};
        const std::jthread monitor{[&] {
            lws::RingCounters previous{};
            std::ignore = std::ranges::find_if(std::views::iota(0), [&](std::int32_t) {
                const bool finished = producer_done.load(std::memory_order_acquire);
                const lws::RingCounters now = ring.counters();
                record_snapshot(tally, previous, now);
                previous = now;
                // Keep sampling past the producer's end until the count is non-vacuous.
                return finished && tally.snapshots >= kMinSnapshots;
            });
        }};
    }
    EXPECT_EQ(tally.overcounts, 0U);
    EXPECT_EQ(tally.regressions, 0U);
    EXPECT_GE(tally.snapshots, kMinSnapshots);
    const lws::RingCounters counters = ring.counters();
    EXPECT_EQ(counters.offered, kTotal);
    EXPECT_EQ(counters.accepted + counters.dropped, counters.offered);
    EXPECT_GT(counters.dropped, 0U);
}
