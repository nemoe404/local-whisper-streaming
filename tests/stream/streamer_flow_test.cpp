#include "stream/stream_fakes.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <ranges>
#include <span>
#include <vector>

#include "lws/streamer.hpp"
#include "lws/streamer_options.hpp"
#include "lws/text_event.hpp"
#include "shell/streamer_impl.hpp"

namespace {

using lws::test::EventLog;
using lws::test::IndexedVadLog;

lws::Streamer make_streamer(IndexedVadLog& log, EventLog& events, std::size_t ring_samples) {
    lws::StreamerOptions options;
    options.ring_capacity_samples = ring_samples;
    auto streamer = lws::StreamerAccess::assemble(lws::test::indexed_engine(log),
                                                  lws::test::position_decoder(), options)
                        .value();
    EXPECT_TRUE(
        streamer.on_text([&events](const lws::TextEvent& event) noexcept { events.record(event); })
            .has_value());
    return streamer;
}

void push_in_pieces(lws::Streamer& streamer, std::span<const float> stream, std::ptrdiff_t piece) {
    std::ranges::for_each(stream | std::views::chunk(piece), [&](auto samples) {
        EXPECT_TRUE(
            streamer.push(std::span<const float>{samples.begin(), samples.end()}).has_value());
    });
}

// The event a segment must produce: its exact stream range and the frames it holds.
lws::TextEvent expected_event(const lws::Segment& segment) {
    const std::uint64_t end = segment.start + segment.samples.size();
    return lws::TextEvent{.kind = lws::EventKind::Text,
                          .t_start = segment.start,
                          .t_end = end,
                          .text = lws::test::position_text(segment.start / lws::kVadFrameSamples,
                                                           (end / lws::kVadFrameSamples) - 1),
                          .after_gap = false};
}

} // namespace

TEST(StreamerFlow, TwoUtterancesComeBackInStreamOrder) {
    const std::vector<float> stream = lws::test::indexed_stream(
        lws::test::values_of({{0.0F, 20}, {0.9F, 30}, {0.0F, 40}, {0.9F, 30}, {0.0F, 40}}));
    const std::vector<lws::Segment> segments = lws::test::direct_segments(stream, 1000);
    ASSERT_EQ(segments.size(), 2U);
    const std::vector<lws::TextEvent> expected =
        segments | std::views::transform(expected_event) | std::ranges::to<std::vector>();

    IndexedVadLog log;
    EventLog events;
    lws::Streamer streamer = make_streamer(log, events, stream.size());
    ASSERT_TRUE(streamer.start().has_value());
    push_in_pieces(streamer, stream, 1000);
    ASSERT_TRUE(streamer.stop(lws::StopMode::Drain).has_value());

    EXPECT_EQ(events.events(), expected);
}

TEST(StreamerFlow, DrainClosesAnUtteranceTheStreamEndsInside) {
    const std::vector<float> stream =
        lws::test::indexed_stream(lws::test::values_of({{0.0F, 20}, {0.9F, 30}}));

    IndexedVadLog log;
    EventLog events;
    lws::Streamer streamer = make_streamer(log, events, stream.size());
    ASSERT_TRUE(streamer.start().has_value());
    push_in_pieces(streamer, stream, 1000);
    ASSERT_TRUE(streamer.stop(lws::StopMode::Drain).has_value());

    const std::vector<lws::TextEvent> got = events.events();
    ASSERT_EQ(got.size(), 1U);
    EXPECT_EQ(got.front().kind, lws::EventKind::Text);
    EXPECT_EQ(got.front().t_end, std::uint64_t{50} * lws::kVadFrameSamples);
}
