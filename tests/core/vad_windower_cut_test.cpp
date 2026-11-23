#include "lws/vad_windower.hpp"

#include "trace_support.hpp"

#include <gtest/gtest.h>

#include <cstddef>

namespace {

constexpr float kSpeech = 0.9F;
constexpr float kQuiet = 0.1F;

} // namespace

// CORE-05, CORE-08: with the default options the ceiling is 438 frames. Steady speech from the
// first frame emits nothing until the 438th, then cuts. All probabilities are equal, so min_element
// picks the first minimum of the search span [438 - 47, 438), which is index 391. The remainder
// starts 8 frames before the cut (frame 383) and is closed by a hangover.
TEST(CeilingTracer, SteadySpeechCutsAtTheCeilingAndTheRemainderIsClosedByHangover) {
    auto windower = lws::test::make_windower();
    EXPECT_TRUE(lws::test::feed_run(windower, 0.0F, 437, kSpeech).empty());
    const auto cut = lws::test::feed_run(windower, 437.0F, 1, kSpeech);
    ASSERT_EQ(cut.size(), 1U);
    const lws::Segment& first = cut.front();
    EXPECT_EQ(lws::test::frame_count(first), 391U);
    EXPECT_EQ(first.start, 0U);
    EXPECT_EQ(first.reason, lws::CloseReason::Ceiling);
    EXPECT_FALSE(first.continues_previous);

    EXPECT_TRUE(lws::test::feed_run(windower, 438.0F, 21, kQuiet).empty());
    const auto closed = lws::test::feed_run(windower, 459.0F, 1, kQuiet);
    ASSERT_EQ(closed.size(), 1U);
    const lws::Segment& second = closed.front();
    EXPECT_EQ(second.reason, lws::CloseReason::Hangover);
    EXPECT_TRUE(second.continues_previous);
    EXPECT_EQ(second.start, 383U * lws::kVadFrameSamples);
    EXPECT_EQ(lws::test::frame_count(second), 55U + 21U);
    EXPECT_FLOAT_EQ(lws::test::frame_id(second, 0), 383.0F);
}
