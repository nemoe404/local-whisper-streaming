#include "lws/vad_windower.hpp"

#include "trace_support.hpp"

#include <gtest/gtest.h>

#include <cstddef>

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
