#include "core/lifecycle.hpp"
#include "lws/streamer_options.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace {

using lws::Control;
using lws::Error;
using lws::RunState;

struct Cell {
    RunState state;
    Control call;
    std::expected<RunState, Error> want;
};

constexpr Cell cell(RunState state, Control call, std::expected<RunState, Error> want) {
    return Cell{.state = state, .call = call, .want = want};
}

constexpr std::unexpected<Error> refused(Error error) { return std::unexpected(error); }

// Rows are the state, columns the call: a state means success, an error means refusal.
constexpr std::array kTable{
    cell(RunState::Created, Control::SetCallback, RunState::Created),
    cell(RunState::Created, Control::Start, RunState::Running),
    cell(RunState::Created, Control::Push, refused(Error::NotRunning)),
    cell(RunState::Created, Control::Flush, refused(Error::NotRunning)),
    cell(RunState::Created, Control::Stop, refused(Error::NotRunning)),
    cell(RunState::Running, Control::SetCallback, refused(Error::AlreadyStarted)),
    cell(RunState::Running, Control::Start, refused(Error::AlreadyStarted)),
    cell(RunState::Running, Control::Push, RunState::Running),
    cell(RunState::Running, Control::Flush, RunState::Running),
    cell(RunState::Running, Control::Stop, RunState::Stopped),
    cell(RunState::Stopped, Control::SetCallback, refused(Error::AlreadyStarted)),
    cell(RunState::Stopped, Control::Start, refused(Error::AlreadyStarted)),
    cell(RunState::Stopped, Control::Push, refused(Error::NotRunning)),
    cell(RunState::Stopped, Control::Flush, refused(Error::NotRunning)),
    cell(RunState::Stopped, Control::Stop, refused(Error::NotRunning)),
};

static_assert(lws::next_state(RunState::Created, Control::Start) == RunState::Running);
static_assert(lws::next_state(RunState::Stopped, Control::Start) ==
              std::unexpected(Error::AlreadyStarted));
static_assert(std::ranges::all_of(kTable, [](const Cell& cell) {
    return lws::next_state(cell.state, cell.call) == cell.want;
}));

lws::StreamerOptions with_ring(std::size_t samples) {
    lws::StreamerOptions options;
    options.ring_capacity_samples = samples;
    return options;
}

lws::StreamerOptions with_queue(std::size_t segments) {
    lws::StreamerOptions options;
    options.queue_segments = segments;
    return options;
}

lws::StreamerOptions with_poll(std::chrono::milliseconds poll) {
    lws::StreamerOptions options;
    options.poll = poll;
    return options;
}

} // namespace

TEST(StreamerLifecycle, AllFifteenCellsMatchTheTable) {
    EXPECT_EQ(kTable.size(), 15U);
    std::ranges::for_each(kTable, [](const Cell& cell) {
        EXPECT_EQ(lws::next_state(cell.state, cell.call), cell.want)
            << "state " << static_cast<int>(cell.state) << " call " << static_cast<int>(cell.call);
    });
}

TEST(StreamerLifecycle, TheAppendedErrorsFollowTheDecoderOptionsError) {
    EXPECT_EQ(static_cast<std::uint8_t>(Error::InvalidDecoderOptions) + 1,
              static_cast<std::uint8_t>(Error::NotRunning));
    EXPECT_EQ(static_cast<std::uint8_t>(Error::NotRunning) + 1,
              static_cast<std::uint8_t>(Error::AlreadyStarted));
    EXPECT_EQ(static_cast<std::uint8_t>(Error::AlreadyStarted) + 1,
              static_cast<std::uint8_t>(Error::InvalidStreamerOptions));
}

TEST(StreamerOptionsValidate, DefaultsComeBackUnchanged) {
    const lws::StreamerOptions defaults;
    const auto result = lws::validate(defaults);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->ring_capacity_samples, defaults.ring_capacity_samples);
    EXPECT_EQ(result->queue_segments, defaults.queue_segments);
    EXPECT_EQ(result->poll, defaults.poll);
}

TEST(StreamerOptionsValidate, RingBoundaryIsOneVadFrame) {
    const auto below = lws::validate(with_ring(511));
    ASSERT_FALSE(below.has_value());
    EXPECT_EQ(below.error(), Error::InvalidStreamerOptions);
    EXPECT_TRUE(lws::validate(with_ring(512)).has_value());
}

TEST(StreamerOptionsValidate, QueueBoundaryIsOneSegment) {
    const auto zero = lws::validate(with_queue(0));
    ASSERT_FALSE(zero.has_value());
    EXPECT_EQ(zero.error(), Error::InvalidStreamerOptions);
    EXPECT_TRUE(lws::validate(with_queue(1)).has_value());
}

TEST(StreamerOptionsValidate, PollBoundaryIsOneToOneThousandMilliseconds) {
    using std::chrono::milliseconds;
    EXPECT_EQ(lws::validate(with_poll(milliseconds{0})).error(), Error::InvalidStreamerOptions);
    EXPECT_EQ(lws::validate(with_poll(milliseconds{1001})).error(), Error::InvalidStreamerOptions);
    EXPECT_TRUE(lws::validate(with_poll(milliseconds{1})).has_value());
    EXPECT_TRUE(lws::validate(with_poll(milliseconds{1000})).has_value());
}

TEST(StreamerOptionsValidate, VadAndDecoderErrorsComeBackUnchanged) {
    lws::StreamerOptions bad_vad;
    bad_vad.vad.threshold = 1.5F;
    bad_vad.queue_segments = 0;
    const auto vad = lws::validate(bad_vad);
    ASSERT_FALSE(vad.has_value());
    EXPECT_EQ(vad.error(), Error::InvalidThreshold);

    lws::StreamerOptions bad_decoder;
    bad_decoder.decoder.n_threads = 0;
    bad_decoder.queue_segments = 0;
    const auto decoder = lws::validate(bad_decoder);
    ASSERT_FALSE(decoder.has_value());
    EXPECT_EQ(decoder.error(), Error::InvalidDecoderOptions);
}
