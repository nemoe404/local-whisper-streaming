// A stop requested through a std::stop_token reaches whisper through its abort callback, which the
// CPU backend polls after each graph node. The decode that was cancelled returns a cancelled
// outcome with no text, and a cancelled decode must leave the context usable for the next segment.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <fmt/base.h>
#include <gtest/gtest.h>

#include "decode/decode_support.hpp"
#include "lws/decode_policy.hpp"
#include "lws/options.hpp"
#include "lws/segment.hpp"
#include "shell/decoder.hpp"

namespace {

using Clock = std::chrono::steady_clock;

constexpr double kStopBoundMs = 2000.0;

double milliseconds_between(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
}

std::string joined_text(const std::vector<lws::TextSpan>& spans) {
    return spans | std::views::transform([](const lws::TextSpan& s) { return s.text + " "; }) |
           std::views::join | std::ranges::to<std::string>();
}

} // namespace

TEST(DecoderStopModel, StopRequestedBeforeTheCallCancelsAndTheContextStaysUsable) {
    const auto segments = lws::test::recorded_segments();
    ASSERT_TRUE(segments.has_value()) << segments.error();
    ASSERT_FALSE(segments->empty());
    const lws::Segment& segment = segments->front();
    const auto reference = lws::test::reference_words();
    ASSERT_TRUE(reference.has_value()) << reference.error();

    auto decoder = lws::Decoder::open(LWS_WHISPER_MODEL_PATH, lws::DecoderOptions{});
    ASSERT_TRUE(decoder.has_value());

    const std::stop_source stopped;
    stopped.request_stop();
    const Clock::time_point begin = Clock::now();
    const auto cancelled = decoder->decode(segment, stopped.get_token());
    const Clock::time_point end = Clock::now();
    const double elapsed_ms = milliseconds_between(begin, end);
    fmt::println("STOP-BEFORE model={} elapsed_ms={:.3f}", LWS_WHISPER_MODEL_NAME, elapsed_ms);

    ASSERT_TRUE(cancelled.has_value());
    EXPECT_EQ(cancelled->status, lws::DecodeStatus::Cancelled);
    EXPECT_TRUE(cancelled->spans.empty());
    EXPECT_EQ(decoder->whisper_calls(), 1U);
    EXPECT_LT(elapsed_ms, kStopBoundMs);

    const std::stop_source running;
    const auto again = decoder->decode(segment, running.get_token());
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->status, lws::DecodeStatus::Decoded);
    ASSERT_FALSE(again->spans.empty());
    EXPECT_EQ(decoder->whisper_calls(), 2U);
    const std::size_t errors =
        lws::test::word_errors(lws::test::normalised_words(joined_text(again->spans)), *reference);
    EXPECT_LE(errors, static_cast<std::size_t>(LWS_DECODE_MAX_WORD_ERRORS));
}
