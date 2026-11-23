#include "lws/vad_windower.hpp"

#include "trace_support.hpp"

#include <gtest/gtest.h>

#include <array>
#include <limits>

namespace {

constexpr float kSpeech = 0.9F;
constexpr float kQuiet = 0.1F;

} // namespace

// CORE-04, CORE-08: with the default options, 8 speech frames open a window and the 22nd
// sub-threshold frame closes it. The window holds 8 + 21 = 29 frames; the closing frame is not in
// it.
TEST(HangoverTracer, EightSpeechThenTwentyTwoSilenceEmitsOneHangoverSegment) {
    auto windower = lws::test::make_windower();
    EXPECT_TRUE(lws::test::feed_run(windower, 1.0F, 8, kSpeech).empty());
    EXPECT_TRUE(lws::test::feed_run(windower, 9.0F, 21, kQuiet).empty());
    const auto emitted = lws::test::feed_run(windower, 30.0F, 1, kQuiet);
    ASSERT_EQ(emitted.size(), 1U);
    const lws::Segment& segment = emitted.front();
    EXPECT_EQ(lws::test::frame_count(segment), 29U);
    EXPECT_EQ(segment.start, 0U);
    EXPECT_EQ(segment.reason, lws::CloseReason::Hangover);
    EXPECT_FALSE(segment.continues_previous);
    EXPECT_FALSE(segment.discontinuous_before);
    EXPECT_FLOAT_EQ(lws::test::frame_id(segment, 0), 1.0F);
    EXPECT_FLOAT_EQ(lws::test::frame_id(segment, 28), 29.0F);
}

// CORE-04, Edge 7: 21 trailing frames (672 ms) stay in the segment; the 22nd closes it and is out.
TEST(Hangover, TwentyOneTrailingFramesStayTheTwentySecondClosesAndIsExcluded) {
    auto windower = lws::test::make_windower();
    EXPECT_TRUE(lws::test::feed_run(windower, 1.0F, 8, kSpeech).empty());
    EXPECT_TRUE(lws::test::feed_run(windower, 9.0F, 21, kQuiet).empty());
    const auto emitted = lws::test::feed_run(windower, 30.0F, 1, kQuiet);
    ASSERT_EQ(emitted.size(), 1U);
    EXPECT_EQ(lws::test::frame_count(emitted.front()), 29U);
    EXPECT_FLOAT_EQ(lws::test::frame_id(emitted.front(), 28), 29.0F);
}

// CORE-04, Edge 4: a speech frame inside the hangover refreshes it. 8 + 3 + 1 + 21 = 33 frames, and
// the full 22 sub-threshold frames are counted again from the dip.
TEST(Hangover, SpeechDipInsideTheHangoverRefreshesIt) {
    auto windower = lws::test::make_windower();
    EXPECT_TRUE(lws::test::feed_run(windower, 1.0F, 8, kSpeech).empty());
    EXPECT_TRUE(lws::test::feed_run(windower, 9.0F, 3, kQuiet).empty());
    EXPECT_TRUE(lws::test::feed_run(windower, 12.0F, 1, kSpeech).empty());
    EXPECT_TRUE(lws::test::feed_run(windower, 13.0F, 21, kQuiet).empty());
    const auto emitted = lws::test::feed_run(windower, 34.0F, 1, kQuiet);
    ASSERT_EQ(emitted.size(), 1U);
    EXPECT_EQ(lws::test::frame_count(emitted.front()), 33U);
}

// D-15: min_silence_duration_ms = 0 clamps the hangover to one frame; the sub-threshold frame
// closes the window and is absent from it.
TEST(Hangover, MinSilenceZeroClosesOnTheFirstSubThresholdFrame) {
    lws::Options options;
    options.min_silence_duration_ms = 0;
    auto windower = lws::test::make_windower(options);
    EXPECT_TRUE(lws::test::feed_run(windower, 1.0F, 8, kSpeech).empty());
    const auto emitted = lws::test::feed_run(windower, 9.0F, 1, kQuiet);
    ASSERT_EQ(emitted.size(), 1U);
    EXPECT_EQ(lws::test::frame_count(emitted.front()), 8U);
    EXPECT_FLOAT_EQ(lws::test::frame_id(emitted.front(), 7), 8.0F);
}

// CORE-02, Edge 7: the frame that closed a window seeds the next pre-roll. After the tracer close
// (closing frame id 30), quiet ids 31..39 give a 10-frame tail 30..39 ahead of speech 40..47.
TEST(Hangover, ClosingFrameSeedsTheNextPreRoll) {
    auto windower = lws::test::make_windower();
    EXPECT_TRUE(lws::test::feed_run(windower, 1.0F, 8, kSpeech).empty());
    EXPECT_EQ(lws::test::feed_run(windower, 9.0F, 22, kQuiet).size(), 1U);
    EXPECT_TRUE(lws::test::feed_run(windower, 31.0F, 9, kQuiet).empty());
    EXPECT_TRUE(lws::test::feed_run(windower, 40.0F, 8, kSpeech).empty());
    const auto emitted = lws::test::feed_run(windower, 48.0F, 22, kQuiet);
    ASSERT_EQ(emitted.size(), 1U);
    const lws::Segment& second = emitted.front();
    EXPECT_EQ(lws::test::frame_count(second), 39U); // 10 lead-in + 8 speech + 21 trailing
    EXPECT_FLOAT_EQ(lws::test::frame_id(second, 0), 30.0F);
    EXPECT_FLOAT_EQ(lws::test::frame_id(second, 9), 39.0F);
    EXPECT_FLOAT_EQ(lws::test::frame_id(second, 10), 40.0F);
}

// Edge 19, CORE-02: birth clears the tail, so a second window opens with its own lead-in. The first
// window holds ids 1..29; after 40 more quiet frames the second begins at id 61, outside that
// range.
TEST(Hangover, BirthClearsTheTailSoASecondWindowHasItsOwnHead) {
    auto windower = lws::test::make_windower();
    EXPECT_TRUE(lws::test::feed_run(windower, 1.0F, 8, kSpeech).empty());
    const auto first = lws::test::feed_run(windower, 9.0F, 22, kQuiet);
    ASSERT_EQ(first.size(), 1U);
    EXPECT_TRUE(lws::test::feed_run(windower, 31.0F, 40, kQuiet).empty());
    EXPECT_TRUE(lws::test::feed_run(windower, 71.0F, 8, kSpeech).empty());
    const auto second = lws::test::feed_run(windower, 79.0F, 22, kQuiet);
    ASSERT_EQ(second.size(), 1U);
    EXPECT_FLOAT_EQ(lws::test::frame_id(first.front(), 28), 29.0F);
    // Quiet ids 31..70 then speech 71..77 fill a 17-frame tail: the earliest is id 61.
    EXPECT_FLOAT_EQ(lws::test::frame_id(second.front(), 0), 61.0F);
}

// Discretion, start index: 30 quiet frames keep ids 21..30 as lead-in, which are 0-based frame
// indices 20..29, so start is 20 * 512 = 10240.
TEST(Segment, StartIsTheAbsoluteSampleIndexOfTheFirstSample) {
    auto windower = lws::test::make_windower();
    EXPECT_TRUE(lws::test::feed_run(windower, 1.0F, 30, kQuiet).empty());
    EXPECT_TRUE(lws::test::feed_run(windower, 31.0F, 8, kSpeech).empty());
    const auto emitted = lws::test::feed_run(windower, 39.0F, 22, kQuiet);
    ASSERT_EQ(emitted.size(), 1U);
    EXPECT_FLOAT_EQ(lws::test::frame_id(emitted.front(), 0), 21.0F);
    EXPECT_EQ(emitted.front().start, 10240U);
}

#ifdef NDEBUG
// D11: NaN fails the ordered comparison, so it is sub-threshold and never opens a window. Release
// only: in Debug a NaN probability trips the range assert.
TEST(ProbabilityBoundary, NaNIsSubThresholdInReleaseOnly) {
    auto windower = lws::test::make_windower();
    constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
    EXPECT_TRUE(lws::test::feed_run(windower, 1.0F, 8, kNaN).empty());
    EXPECT_TRUE(lws::test::feed_run(windower, 9.0F, 30, kNaN).empty());
}
#endif

#ifndef NDEBUG
// D11, D-06: Debug asserts reject a probability outside [0, 1], NaN, and a frame that is not 512
// samples. Any death message matches.
TEST(ProbabilityBoundary, OutOfRangeProbabilityAndWrongFrameSizeAssertInDebug) {
    auto windower = lws::test::make_windower();
    const auto frame = lws::test::frame_of(1.0F);
    const std::array<float, 100> short_frame{};
    constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
    EXPECT_DEATH(static_cast<void>(windower.feed(frame, 1.5F)), "");
    EXPECT_DEATH(static_cast<void>(windower.feed(short_frame, 0.5F)), "");
    EXPECT_DEATH(static_cast<void>(windower.feed(frame, kNaN)), "");
}
#endif
