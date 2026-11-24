#include "stream/stream_fakes.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include "lws/streamer.hpp"
#include "lws/text_event.hpp"

// Destroying a streamer must join both threads and return, whatever it was doing. Every case runs
// its whole body, destruction included, under a bound: a hang ends the process with a named line
// instead of waiting for the test runner's timeout.
namespace {

using lws::test::DecodeGate;
using lws::test::EventLog;
using lws::test::IndexedVadLog;
using std::chrono::milliseconds;

constexpr milliseconds kBound{10000};
constexpr std::size_t kCycles = 50;

// `count` utterances in a row, each closed by its own silence: 90 frames each.
std::vector<float> utterances(std::size_t count) {
    return lws::test::indexed_stream(
        std::views::repeat(lws::test::values_of({{0.0F, 20}, {0.9F, 30}, {0.0F, 40}}), count) |
        std::views::join | std::ranges::to<std::vector>());
}

// Starts, pushes the whole stream and flushes: the decoder has work and the pump has samples.
bool start_and_fill(lws::Streamer& streamer, std::span<const float> stream) {
    return streamer.start().has_value() && lws::test::push_pieces(streamer, stream, 1000) &&
           streamer.flush().has_value();
}

void never_started() {
    IndexedVadLog log;
    EventLog events;
    const lws::Streamer streamer =
        lws::test::make_test_streamer(log, events, lws::test::position_decoder(), 4096);
}

void started_idle() {
    IndexedVadLog log;
    EventLog events;
    lws::Streamer streamer =
        lws::test::make_test_streamer(log, events, lws::test::position_decoder(), 4096);
    ASSERT_TRUE(streamer.start().has_value());
}

// 2000 frames in a ring that holds them all; the streamer is destroyed while the pump is still
// working through them.
void mid_stream() {
    const std::vector<float> stream = utterances(23);
    IndexedVadLog log;
    EventLog events;
    lws::Streamer streamer = lws::test::make_test_streamer(
        log, events, lws::test::position_decoder(), stream.size(), 64);
    ASSERT_TRUE(streamer.start().has_value());
    ASSERT_TRUE(lws::test::push_pieces(streamer, stream, 4096));
}

// A decode that only a stop request ends: destruction ends it.
void mid_decode() {
    const std::vector<float> stream = utterances(1);
    IndexedVadLog log;
    EventLog events;
    DecodeGate gate;
    lws::Streamer streamer = lws::test::make_test_streamer(
        log, events, lws::test::stop_blocking_decoder(gate), stream.size());
    ASSERT_TRUE(start_and_fill(streamer, stream));
    ASSERT_TRUE(gate.wait_entered(kBound));
}

void after_stop(lws::StopMode mode) {
    const std::vector<float> stream = utterances(2);
    IndexedVadLog log;
    EventLog events;
    lws::Streamer streamer =
        lws::test::make_test_streamer(log, events, lws::test::position_decoder(), stream.size());
    ASSERT_TRUE(start_and_fill(streamer, stream));
    ASSERT_TRUE(streamer.stop(mode).has_value());
}

// The moved-from streamer is destroyed first and must do nothing; the other still joins.
void moved_from() {
    const std::vector<float> stream = utterances(2);
    IndexedVadLog log;
    EventLog events;
    std::optional<lws::Streamer> first;
    first.emplace(
        lws::test::make_test_streamer(log, events, lws::test::position_decoder(), stream.size()));
    ASSERT_TRUE(start_and_fill(*first, stream));
    std::optional<lws::Streamer> second;
    second = std::move(first);
    first.reset();
    second.reset();
}

// Assigning over a running streamer stops and joins it inside the assignment.
void move_assign_over_running() {
    const std::vector<float> stream = utterances(2);
    IndexedVadLog running_log;
    IndexedVadLog fresh_log;
    EventLog events;
    lws::Streamer running = lws::test::make_test_streamer(
        running_log, events, lws::test::position_decoder(), stream.size());
    ASSERT_TRUE(start_and_fill(running, stream));
    running = lws::test::make_test_streamer(fresh_log, events, lws::test::position_decoder(),
                                            stream.size());
    ASSERT_TRUE(running.start().has_value());
}

// One lifecycle: create, start, push two utterances, stop, destroy. Returns the events delivered.
std::vector<lws::TextEvent> one_cycle(std::span<const float> stream, lws::StopMode mode) {
    IndexedVadLog log;
    EventLog events;
    {
        lws::Streamer streamer = lws::test::make_test_streamer(
            log, events, lws::test::position_decoder(), stream.size());
        EXPECT_TRUE(streamer.start().has_value());
        EXPECT_TRUE(lws::test::push_pieces(streamer, stream, 1000));
        EXPECT_TRUE(streamer.stop(mode).has_value());
    }
    return events.events();
}

void fifty_cycles() {
    const std::vector<float> stream = utterances(2);
    const std::vector<lws::TextEvent> expected =
        lws::test::expected_text_events(lws::test::direct_segments(stream, 1000));
    ASSERT_EQ(expected.size(), 2U);
    std::ranges::for_each(std::views::iota(std::size_t{0}, kCycles), [&](std::size_t cycle) {
        const bool drain = cycle % 2 == 0;
        const std::vector<lws::TextEvent> got =
            one_cycle(stream, drain ? lws::StopMode::Drain : lws::StopMode::Immediate);
        if (drain) {
            EXPECT_EQ(got, expected) << "cycle " << cycle;
        } else {
            EXPECT_LE(got.size(), expected.size()) << "cycle " << cycle;
        }
    });
}

} // namespace

TEST(StreamerTeardown, NeverStarted) {
    lws::test::run_bounded("never-started", kBound, never_started);
}

TEST(StreamerTeardown, StartedIdle) {
    lws::test::run_bounded("started-idle", kBound, started_idle);
}

TEST(StreamerTeardown, MidStream) { lws::test::run_bounded("mid-stream", kBound, mid_stream); }

TEST(StreamerTeardown, MidDecode) { lws::test::run_bounded("mid-decode", kBound, mid_decode); }

TEST(StreamerTeardown, AfterDrain) {
    lws::test::run_bounded("after-drain", kBound, [] { after_stop(lws::StopMode::Drain); });
}

TEST(StreamerTeardown, AfterImmediate) {
    lws::test::run_bounded("after-immediate", kBound, [] { after_stop(lws::StopMode::Immediate); });
}

TEST(StreamerTeardown, MovedFrom) { lws::test::run_bounded("moved-from", kBound, moved_from); }

TEST(StreamerTeardown, MoveAssignOverRunning) {
    lws::test::run_bounded("move-assign-over-running", kBound, move_assign_over_running);
}

TEST(StreamerTeardown, FiftyCycles) {
    lws::test::run_bounded("fifty-cycles", kBound, fifty_cycles);
}
