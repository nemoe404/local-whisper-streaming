// A stop requested through a std::stop_token reaches whisper through its abort callback, which the
// CPU backend polls after each graph node. The decode that was cancelled returns a cancelled
// outcome with no text, and a cancelled decode must leave the context usable for the next segment.
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <ranges>
#include <stop_token>
#include <string>
#include <thread>
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

// The decode result and the time from request_stop() on the stopper thread to decode's return.
struct StoppedDecode {
    std::expected<lws::DecodeResult, lws::Error> result;
    double latency_ms = 0.0;
};

// Decodes with a stop requested from a second thread after `delay`. The stopper stores the clock
// reading just before it requests the stop; the caller reads it only after joining the stopper.
StoppedDecode decode_with_stop_after(lws::Decoder& decoder, const lws::Segment& segment,
                                     std::chrono::milliseconds delay) {
    const std::stop_source source;
    std::atomic<std::int64_t> stop_ns{0};
    std::jthread stopper([&source, &stop_ns, delay] {
        std::this_thread::sleep_for(delay);
        stop_ns.store(Clock::now().time_since_epoch().count());
        source.request_stop();
    });
    auto result = decoder.decode(segment, source.get_token());
    const Clock::time_point returned = Clock::now();
    stopper.join();
    const Clock::time_point requested{Clock::duration{stop_ns.load()}};
    return StoppedDecode{.result = std::move(result),
                         .latency_ms = milliseconds_between(requested, returned)};
}

// Three copies of the recorded clip with gaps: about 33 s, far longer than the delays below.
std::expected<lws::Segment, std::string> long_segment() {
    const auto clip = lws::test::load_fixture_wav("clean_en_sample.wav");
    if (!clip) {
        return std::unexpected(clip.error());
    }
    return lws::Segment{.samples = lws::test::three_copies(*clip)};
}

using Lines = std::vector<std::string>;

// The names of the promises the stopped decode breaks. A decode that finished first is named on
// its own, because then the test measured nothing.
Lines broken_promises(const StoppedDecode& stopped, std::uint64_t calls,
                      std::chrono::milliseconds delay) {
    if (!stopped.result.has_value()) {
        return {"decode returned an error"};
    }
    if (stopped.result->status == lws::DecodeStatus::Decoded) {
        return {"decode finished before the stop at " + std::to_string(delay.count()) +
                " ms; the input is too short for this delay on this machine"};
    }
    const std::array<std::pair<bool, const char*>, 4> checks{{
        {stopped.result->status == lws::DecodeStatus::Cancelled, "status is not Cancelled"},
        {stopped.result->spans.empty(), "a cancelled decode returned spans"},
        {calls == 1U, "the decoder did not make exactly one whisper call"},
        {stopped.latency_ms < kStopBoundMs, "the stop took 2000 ms or more to return"},
    }};
    return checks | std::views::filter([](const auto& c) { return !c.first; }) |
           std::views::transform([](const auto& c) { return std::string(c.second); }) |
           std::ranges::to<std::vector>();
}

void expect_cancelled_after(std::chrono::milliseconds delay) {
    const auto segment = long_segment();
    ASSERT_TRUE(segment.has_value()) << segment.error();
    auto decoder = lws::Decoder::open(LWS_WHISPER_MODEL_PATH, lws::DecoderOptions{});
    ASSERT_TRUE(decoder.has_value());

    const StoppedDecode stopped = decode_with_stop_after(*decoder, *segment, delay);
    fmt::println("STOP-LATENCY model={} delay_ms={} latency_ms={:.3f}", LWS_WHISPER_MODEL_NAME,
                 delay.count(), stopped.latency_ms);
    EXPECT_EQ(broken_promises(stopped, decoder->whisper_calls(), delay), Lines{});
}

} // namespace

TEST(DecoderStopModel, StopAfter20msReturnsCancelled) {
    expect_cancelled_after(std::chrono::milliseconds{20});
}

TEST(DecoderStopModel, StopAfter50msReturnsCancelled) {
    expect_cancelled_after(std::chrono::milliseconds{50});
}

TEST(DecoderStopModel, StopAfter100msReturnsCancelled) {
    expect_cancelled_after(std::chrono::milliseconds{100});
}

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
    EXPECT_TRUE(std::cmp_less_equal(errors, LWS_DECODE_MAX_WORD_ERRORS));
}
