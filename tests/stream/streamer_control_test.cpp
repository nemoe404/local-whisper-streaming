#include "stream/stream_fakes.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <span>
#include <utility>

#include "lws/options.hpp"
#include "lws/streamer.hpp"
#include "lws/streamer_options.hpp"
#include "lws/text_event.hpp"
#include "shell/streamer_impl.hpp"

namespace {

using lws::Error;
using lws::test::EventLog;
using lws::test::IndexedVadLog;

constexpr std::size_t kRing = 8192;

lws::Streamer make_streamer(IndexedVadLog& log) {
    lws::StreamerOptions options;
    options.ring_capacity_samples = kRing;
    return lws::StreamerAccess::assemble(lws::test::indexed_engine(log),
                                         lws::test::position_decoder(), options)
        .value();
}

const std::array<float, 512> kSamples{};

std::span<const float> some_samples() { return std::span<const float>{kSamples}; }

void expect_refused(const std::expected<void, Error>& result, Error error) {
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), error);
}

} // namespace

TEST(StreamerControl, PushFlushAndStopBeforeStartAreNotRunning) {
    IndexedVadLog log;
    lws::Streamer streamer = make_streamer(log);
    expect_refused(streamer.push(some_samples()), Error::NotRunning);
    expect_refused(streamer.flush(), Error::NotRunning);
    expect_refused(streamer.stop(lws::StopMode::Drain), Error::NotRunning);
    expect_refused(streamer.stop(lws::StopMode::Immediate), Error::NotRunning);
}

TEST(StreamerControl, ASecondStartAndACallbackAfterStartAreAlreadyStarted) {
    IndexedVadLog log;
    EventLog events;
    lws::Streamer streamer = make_streamer(log);
    ASSERT_TRUE(
        streamer.on_text([&events](const lws::TextEvent& event) noexcept { events.record(event); })
            .has_value());
    ASSERT_TRUE(streamer.start().has_value());
    expect_refused(streamer.start(), Error::AlreadyStarted);
    expect_refused(streamer.on_text([](const lws::TextEvent&) noexcept {}), Error::AlreadyStarted);
    EXPECT_TRUE(streamer.push(some_samples()).has_value());
    EXPECT_TRUE(streamer.flush().has_value());
    ASSERT_TRUE(streamer.stop(lws::StopMode::Drain).has_value());
}

TEST(StreamerControl, AfterStopEveryCallIsRefused) {
    IndexedVadLog log;
    lws::Streamer streamer = make_streamer(log);
    ASSERT_TRUE(streamer.start().has_value());
    ASSERT_TRUE(streamer.stop(lws::StopMode::Drain).has_value());
    expect_refused(streamer.push(some_samples()), Error::NotRunning);
    expect_refused(streamer.flush(), Error::NotRunning);
    expect_refused(streamer.stop(lws::StopMode::Drain), Error::NotRunning);
    expect_refused(streamer.stop(lws::StopMode::Immediate), Error::NotRunning);
    expect_refused(streamer.start(), Error::AlreadyStarted);
    expect_refused(streamer.on_text([](const lws::TextEvent&) noexcept {}), Error::AlreadyStarted);
}

TEST(StreamerControl, AfterImmediateStopTheSameCallsAreRefused) {
    IndexedVadLog log;
    lws::Streamer streamer = make_streamer(log);
    ASSERT_TRUE(streamer.start().has_value());
    ASSERT_TRUE(streamer.stop(lws::StopMode::Immediate).has_value());
    expect_refused(streamer.push(some_samples()), Error::NotRunning);
    expect_refused(streamer.start(), Error::AlreadyStarted);
}

TEST(StreamerControl, CreateRefusesBadOptionsBeforeOpeningAnyModel) {
    lws::StreamerOptions options;
    options.queue_segments = 0;
    options.model_path = std::filesystem::path{"/nonexistent/whisper.bin"};
    options.vad_model_path = std::filesystem::path{"/nonexistent/vad.bin"};
    const auto result = lws::Streamer::create(options);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Error::InvalidStreamerOptions);
}

TEST(StreamerControl, AssembleRefusesBadOptionsToo) {
    IndexedVadLog log;
    lws::StreamerOptions options;
    options.queue_segments = 0;
    const auto result = lws::StreamerAccess::assemble(lws::test::indexed_engine(log),
                                                      lws::test::position_decoder(), options);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Error::InvalidStreamerOptions);
}
