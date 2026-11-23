// Segmenter over fake engines: the failure contract and the segmentation around it. No ggml
// compute.
#include <algorithm>
#include <cstddef>

#include <gtest/gtest.h>

#include "fake_vad.hpp"
#include "lws/options.hpp"
#include "lws/segment.hpp"
#include "segmenter_support.hpp"
#include "shell/segmenter.hpp"

namespace {

using lws::test::FakeVadLog;
using lws::test::kFailMarker;
using lws::test::kShortMarker;
using lws::test::make_segmenter;
using lws::test::push_chunked;
using lws::test::stream_of;

constexpr float kSpeech = 0.9F;
constexpr float kQuiet = 0.1F;
constexpr std::size_t kFrame = lws::kVadFrameSamples;

bool holds_value(const lws::Segment& segment, float value) {
    return std::ranges::contains(segment.samples, value);
}

TEST(SegmenterFailure, SpeechThenSilenceClosesOnHangover) {
    FakeVadLog log;
    auto segmenter = make_segmenter(log);
    const auto stream = stream_of({{kQuiet, 5}, {kSpeech, 20}, {kQuiet, 30}});

    const auto pushed = push_chunked(segmenter, stream);

    EXPECT_FALSE(pushed.failure.has_value());
    ASSERT_EQ(pushed.segments.size(), 1U);
    const auto& segment = pushed.segments.front();
    EXPECT_EQ(segment.reason, lws::CloseReason::Hangover);
    EXPECT_EQ(segment.start, 0U);
    // Five quiet frames of pre-roll, then the first speech frame.
    EXPECT_EQ(segment.samples.at(4 * kFrame), kQuiet);
    EXPECT_EQ(segment.samples.at(5 * kFrame), kSpeech);
    EXPECT_EQ(segment.samples.at((6 * kFrame) - 1), kSpeech);
    EXPECT_EQ(lws::test::resets(log), 1U);
}

TEST(SegmenterFailure, FailureForceClosesTheOpenWindow) {
    FakeVadLog log;
    auto segmenter = make_segmenter(log);
    const auto speech = stream_of({{kQuiet, 5}, {kSpeech, 20}});
    const auto before = push_chunked(segmenter, speech);
    ASSERT_FALSE(before.failure.has_value());
    ASSERT_TRUE(before.segments.empty());

    const auto failed = push_chunked(segmenter, stream_of({{kFailMarker, 1}}));

    ASSERT_TRUE(failed.failure.has_value());
    const auto failure = lws::test::failure_of(failed.failure);
    EXPECT_EQ(failure.error, lws::Error::VadFailed);
    ASSERT_TRUE(failure.force_closed.has_value());
    const auto closed = lws::test::segment_of(failure.force_closed);
    EXPECT_EQ(closed.reason, lws::CloseReason::Discontinuity);
    EXPECT_GE(closed.samples.size(), 20 * kFrame);
    EXPECT_FALSE(holds_value(closed, kFailMarker));

    const auto after = push_chunked(segmenter, stream_of({{kQuiet, 30}}));
    EXPECT_FALSE(after.failure.has_value());
    EXPECT_TRUE(after.segments.empty());
    EXPECT_EQ(lws::test::resets(log), 1U);
}

TEST(SegmenterFailure, FailureWithNoOpenWindowReturnsNoSegment) {
    FakeVadLog log;
    auto segmenter = make_segmenter(log);

    const auto pushed = push_chunked(segmenter, stream_of({{kQuiet, 10}, {kFailMarker, 1}}));

    ASSERT_TRUE(pushed.failure.has_value());
    const auto failure = lws::test::failure_of(pushed.failure);
    EXPECT_EQ(failure.error, lws::Error::VadFailed);
    EXPECT_FALSE(failure.force_closed.has_value());
}

TEST(SegmenterFailure, ShortProbabilityCountIsVadFailed) {
    FakeVadLog log;
    auto segmenter = make_segmenter(log);
    const auto speech = push_chunked(segmenter, stream_of({{kSpeech, 20}}));
    ASSERT_FALSE(speech.failure.has_value());

    const auto failed = push_chunked(segmenter, stream_of({{kShortMarker, 1}}));

    ASSERT_TRUE(failed.failure.has_value());
    const auto failure = lws::test::failure_of(failed.failure);
    EXPECT_EQ(failure.error, lws::Error::VadFailed);
    ASSERT_TRUE(failure.force_closed.has_value());
    EXPECT_EQ(lws::test::segment_of(failure.force_closed).reason, lws::CloseReason::Discontinuity);
}

TEST(SegmenterFailure, NoFrameOfAFailedCallReachesTheWindower) {
    FakeVadLog log;
    // One speech frame opens a window, so any frame that reached the windower would show.
    auto segmenter = make_segmenter(log, lws::Options{.min_speech_duration_ms = 32});
    const auto call = stream_of({{kSpeech, 4}, {kFailMarker, 1}, {kSpeech, 3}});
    ASSERT_EQ(call.size(), 8 * kFrame);

    auto result = segmenter.push(call);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().error, lws::Error::VadFailed);
    EXPECT_FALSE(result.error().force_closed.has_value());

    const auto quiet = push_chunked(segmenter, stream_of({{kQuiet, 40}}));
    EXPECT_FALSE(quiet.failure.has_value());
    EXPECT_TRUE(quiet.segments.empty());
    EXPECT_FALSE(segmenter.flush().has_value());
}

TEST(SegmenterFailure, SpeechAfterAFailureOpensANewWindow) {
    FakeVadLog log;
    auto segmenter = make_segmenter(log);
    const auto failed = push_chunked(segmenter, stream_of({{kFailMarker, 1}}));
    ASSERT_TRUE(failed.failure.has_value());

    const auto pushed = push_chunked(segmenter, stream_of({{kSpeech, 20}, {kQuiet, 30}}));

    EXPECT_FALSE(pushed.failure.has_value());
    ASSERT_EQ(pushed.segments.size(), 1U);
    EXPECT_EQ(pushed.segments.front().reason, lws::CloseReason::Hangover);
    EXPECT_EQ(lws::test::resets(log), 1U);
}

TEST(SegmenterFailure, InvalidOptionsNeverTouchTheEngine) {
    FakeVadLog log;

    const auto created =
        lws::Segmenter::create(lws::test::fake_engine(log), lws::Options{.threshold = 1.5F});

    ASSERT_FALSE(created.has_value());
    EXPECT_EQ(created.error(), lws::Error::InvalidThreshold);
    EXPECT_TRUE(log.calls.empty());
}

} // namespace
