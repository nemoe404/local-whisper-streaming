#include "stream/stream_fakes.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <ranges>
#include <span>
#include <thread>
#include <vector>

#include <fmt/core.h>

#include "lws/streamer.hpp"
#include "lws/text_event.hpp"

namespace {

using lws::test::DecodeGate;
using lws::test::EventLog;
using lws::test::IndexedVadLog;
using std::chrono::milliseconds;

constexpr milliseconds kBound{10000};
constexpr milliseconds kImmediateBound{2000};
constexpr std::size_t kFrame = lws::kVadFrameSamples;

// The leg this build is, for the printed latency line.
constexpr const char* leg_name() {
#if LWS_STREAM_SANITIZER_LEG == 0
    return "release";
#elifdef __has_feature
#if __has_feature(thread_sanitizer)
    return "tsan";
#else
    return "asan-ubsan";
#endif
#else
    return "asan-ubsan";
#endif
}

// One utterance of `speech` frames after `lead` frames of silence and before `tail` frames of it.
std::vector<float> utterance_values(std::size_t lead, std::size_t speech, std::size_t tail) {
    return lws::test::values_of({{0.0F, lead}, {0.9F, speech}, {0.0F, tail}});
}

// Pushes the stream in pieces, calls flush at each position in turn (in the middle of the pushes),
// and pushes what is left. The positions are in increasing order.
void push_with_flushes(lws::Streamer& streamer, std::span<const float> stream, std::size_t piece,
                       std::span<const std::size_t> positions) {
    std::size_t at = 0;
    std::ranges::for_each(positions, [&](std::size_t position) {
        ASSERT_TRUE(lws::test::push_pieces(streamer, stream.subspan(at, position - at), piece));
        ASSERT_TRUE(streamer.flush().has_value());
        at = position;
    });
    ASSERT_TRUE(lws::test::push_pieces(streamer, stream.subspan(at), piece));
}

// Runs a whole stream through a streamer with the flushes and a Drain stop, and returns the events.
std::vector<lws::TextEvent> run_stream(std::span<const float> stream, std::size_t piece,
                                       std::span<const std::size_t> positions) {
    IndexedVadLog log;
    EventLog events;
    lws::Streamer streamer =
        lws::test::make_test_streamer(log, events, lws::test::position_decoder(), stream.size());
    EXPECT_TRUE(streamer.start().has_value());
    push_with_flushes(streamer, stream, piece, positions);
    EXPECT_TRUE(streamer.stop(lws::StopMode::Drain).has_value());
    return events.events();
}

std::vector<lws::TextEvent> oracle(std::span<const float> stream,
                                   std::span<const std::size_t> positions) {
    return lws::test::expected_text_events(
        lws::test::direct_segments_flushed(stream, 1000, positions));
}

// The stream of `count` separate utterances, each closed by its own silence.
std::vector<float> repeated_utterances(std::size_t count) {
    return lws::test::indexed_stream(std::views::repeat(utterance_values(20, 30, 40), count) |
                                     std::views::join | std::ranges::to<std::vector>());
}

// Starts the streamer, pushes the whole stream and flushes, and waits until the decoder is inside
// its first decode.
bool fill_and_wait(lws::Streamer& streamer, std::span<const float> stream, DecodeGate& gate) {
    return streamer.start().has_value() && lws::test::push_pieces(streamer, stream, 1000) &&
           streamer.flush().has_value() && gate.wait_entered(kBound);
}

// With the decoder held on the first of four segments, Drain does not return until the decoder is
// let go, and then every segment has its event, in order.
void drain_waits_for_the_queue() {
    const std::vector<float> stream = repeated_utterances(4);
    const std::vector<lws::TextEvent> expected =
        lws::test::expected_text_events(lws::test::direct_segments(stream, 1000));
    IndexedVadLog log;
    EventLog events;
    DecodeGate gate;
    lws::Streamer streamer = lws::test::make_test_streamer(
        log, events, lws::test::gated_position_decoder(gate), stream.size());
    ASSERT_TRUE(fill_and_wait(streamer, stream, gate));

    auto stopped = std::async(std::launch::async, [&streamer] {
        return streamer.stop(lws::StopMode::Drain).has_value();
    });
    EXPECT_EQ(stopped.wait_for(milliseconds{100}), std::future_status::timeout);
    gate.release();
    ASSERT_EQ(stopped.wait_for(kBound), std::future_status::ready);
    EXPECT_TRUE(stopped.get());
    EXPECT_EQ(events.events(), expected);
}

// With a decode that only a stop request can end, Immediate returns within the bound and no event
// is delivered, neither for the interrupted segment nor for the queued ones.
void immediate_cuts_the_decode() {
    const std::vector<float> stream = repeated_utterances(3);
    IndexedVadLog log;
    EventLog events;
    DecodeGate gate;
    lws::Streamer streamer = lws::test::make_test_streamer(
        log, events, lws::test::stop_blocking_decoder(gate), stream.size());
    ASSERT_TRUE(fill_and_wait(streamer, stream, gate));

    const auto before = std::chrono::steady_clock::now();
    const bool stopped = streamer.stop(lws::StopMode::Immediate).has_value();
    const auto latency =
        std::chrono::duration_cast<milliseconds>(std::chrono::steady_clock::now() - before);
    fmt::println("STOP-IMMEDIATE leg={} latency_ms={}", leg_name(), latency.count());

    EXPECT_TRUE(stopped);
    EXPECT_LT(latency, kImmediateBound);
    EXPECT_TRUE(events.events().empty());
}

} // namespace

TEST(StreamerStop, AFlushClosesTheWindowAtItsExactSample) {
    const std::vector<float> stream =
        lws::test::indexed_stream(lws::test::values_of({{0.0F, 20}, {0.9F, 60}, {0.0F, 40}}));
    const std::array<std::size_t, 1> positions{(50 * kFrame) + 100};
    const std::vector<lws::TextEvent> expected = oracle(stream, positions);
    ASSERT_GE(expected.size(), 2U);
    ASSERT_EQ(expected.front().t_end, 50 * kFrame);

    EXPECT_EQ(run_stream(stream, 1000, positions), expected);
}

TEST(StreamerStop, TwoFlushesInARowAreBothHonoured) {
    const std::vector<float> stream =
        lws::test::indexed_stream(lws::test::values_of({{0.0F, 20}, {0.9F, 60}, {0.0F, 40}}));
    const std::array<std::size_t, 2> positions{(30 * kFrame) + 7, 40 * kFrame};
    const std::vector<lws::TextEvent> expected = oracle(stream, positions);
    ASSERT_GE(expected.size(), 3U);

    EXPECT_EQ(run_stream(stream, 700, positions), expected);
}

TEST(StreamerStop, AFlushBeforeDrainLeavesTheImpliedFlushNothingToClose) {
    const std::vector<float> stream =
        lws::test::indexed_stream(lws::test::values_of({{0.0F, 20}, {0.9F, 30}}));
    const std::array<std::size_t, 1> positions{stream.size()};
    const std::vector<lws::TextEvent> expected = oracle(stream, positions);
    ASSERT_EQ(expected.size(), 1U);

    const std::vector<lws::TextEvent> got = run_stream(stream, 1000, positions);
    EXPECT_EQ(got, expected);
    EXPECT_EQ(got.front().t_end, 50 * kFrame);
}

TEST(StreamerStop, DrainDecodesEveryQueuedSegmentBeforeItReturns) {
    lws::test::run_bounded("drain-queued", kBound, drain_waits_for_the_queue);
}

TEST(StreamerStop, ImmediateInterruptsTheDecodeAndDeliversNothing) {
    lws::test::run_bounded("stop-immediate", kBound, immediate_cuts_the_decode);
}

TEST(StreamerStop, TheEventListDoesNotDependOnThePushSize) {
    const std::vector<float> stream = lws::test::indexed_stream(lws::test::values_of(
        {{0.0F, 20}, {0.9F, 30}, {0.0F, 40}, {0.9F, 30}, {0.0F, 40}, {0.9F, 30}, {0.0F, 10}}));
    const std::array<std::size_t, 1> positions{(100 * kFrame) + 37};
    const std::vector<lws::TextEvent> expected = oracle(stream, positions);
    ASSERT_GE(expected.size(), 4U);

    const std::array<std::size_t, 7> sizes{1, 160, 500, 512, 1000, 4096, stream.size()};
    std::ranges::for_each(sizes, [&](std::size_t piece) {
        EXPECT_EQ(run_stream(stream, piece, positions), expected) << "push size " << piece;
    });
}
