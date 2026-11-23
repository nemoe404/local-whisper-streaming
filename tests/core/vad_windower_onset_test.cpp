#include "lws/vad_windower.hpp"

#include "trace_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <vector>

namespace {

constexpr float kSpeech = 0.9F;
constexpr float kQuiet = 0.1F;

// Frame ids of a segment, in order, so a test can read which frames it holds.
std::vector<float> ids_of(const lws::Segment& segment) {
    std::vector<float> ids;
    std::ranges::for_each(
        std::views::iota(std::size_t{0}, lws::test::frame_count(segment)),
        [&](std::size_t index) { ids.push_back(lws::test::frame_id(segment, index)); });
    return ids;
}

} // namespace

// CORE-03, Edge 1: a run one frame short of the onset (8 frames at defaults) opens nothing.
TEST(OnsetGate, SevenSpeechFramesThenSilenceOpensNothing) {
    auto windower = lws::test::make_windower();
    EXPECT_TRUE(lws::test::feed_run(windower, 1.0F, 7, kSpeech).empty());
    EXPECT_TRUE(lws::test::feed_run(windower, 8.0F, 40, kQuiet).empty());
}

// CORE-03, Edge 1: exactly 8 speech frames open a window; 8 + 21 trailing = 29 frames.
TEST(OnsetGate, EightSpeechFramesOpenAWindow) {
    auto windower = lws::test::make_windower();
    EXPECT_TRUE(lws::test::feed_run(windower, 1.0F, 8, kSpeech).empty());
    EXPECT_TRUE(lws::test::feed_run(windower, 9.0F, 21, kQuiet).empty());
    const auto emitted = lws::test::feed_run(windower, 30.0F, 1, kQuiet);
    ASSERT_EQ(emitted.size(), 1U);
    EXPECT_EQ(lws::test::frame_count(emitted.front()), 29U);
}

// CORE-03, Edge 2: the first sub-threshold frame resets the onset run.
TEST(OnsetGate, SubThresholdResetsTheOnsetRun) {
    auto windower = lws::test::make_windower();
    EXPECT_TRUE(lws::test::feed_run(windower, 1.0F, 7, kSpeech).empty());
    EXPECT_TRUE(lws::test::feed_run(windower, 8.0F, 1, kQuiet).empty());
    EXPECT_TRUE(lws::test::feed_run(windower, 9.0F, 7, kSpeech).empty());
    // The window never opened, so 22 silent frames close nothing.
    EXPECT_TRUE(lws::test::feed_run(windower, 16.0F, 22, kQuiet).empty());
    // A fresh run of 8 then opens one window, closed by 22 silent frames.
    EXPECT_TRUE(lws::test::feed_run(windower, 38.0F, 8, kSpeech).empty());
    const auto emitted = lws::test::feed_run(windower, 46.0F, 22, kQuiet);
    EXPECT_EQ(emitted.size(), 1U);
}

// CORE-03, ROADMAP SC1, Edge 3: 100 cycles of 7 speech frames and 1 silent frame never open.
TEST(OnsetGate, ImpulseTrainOfShortBurstsNeverOpens) {
    auto windower = lws::test::make_windower();
    std::size_t total = 0;
    std::ranges::for_each(
        std::views::iota(std::size_t{0}, std::size_t{100}), [&](std::size_t cycle) {
            const auto first = static_cast<float>(cycle * 8U);
            total += lws::test::feed_run(windower, first, 7, kSpeech).size();
            total += lws::test::feed_run(windower, first + 7.0F, 1, kQuiet).size();
        });
    EXPECT_EQ(total, 0U);
}

// Edge 5: prob equal to the threshold (the default 0.5) counts as speech.
TEST(OnsetGate, ThresholdEqualityCountsAsSpeech) {
    auto windower = lws::test::make_windower();
    EXPECT_TRUE(lws::test::feed_run(windower, 1.0F, 8, 0.5F).empty());
    const auto emitted = lws::test::feed_run(windower, 9.0F, 22, kQuiet);
    ASSERT_EQ(emitted.size(), 1U);
    EXPECT_EQ(lws::test::frame_count(emitted.front()), 29U);
}

// D-15, Edge 6: min_speech_duration_ms = 0 clamps the onset to 1 frame. 1 speech + 21 trailing
// = 22.
TEST(OnsetGate, MinSpeechZeroOpensOnTheFirstSpeechFrame) {
    lws::Options options;
    options.min_speech_duration_ms = 0;
    auto windower = lws::test::make_windower(options);
    EXPECT_TRUE(lws::test::feed_run(windower, 1.0F, 1, kSpeech).empty());
    const auto emitted = lws::test::feed_run(windower, 2.0F, 22, kQuiet);
    ASSERT_EQ(emitted.size(), 1U);
    EXPECT_EQ(lws::test::frame_count(emitted.front()), 22U);
}

// CORE-09, Edge 18: 10 quiet frames ahead of the onset are all kept (pre_roll 300 ms is 10 frames).
TEST(QuietOnset, TenQuietFramesAheadOfTheOnsetAreKept) {
    auto windower = lws::test::make_windower();
    EXPECT_TRUE(lws::test::feed_run(windower, 1.0F, 10, kQuiet).empty());
    EXPECT_TRUE(lws::test::feed_run(windower, 11.0F, 8, kSpeech).empty());
    const auto emitted = lws::test::feed_run(windower, 19.0F, 22, kQuiet);
    ASSERT_EQ(emitted.size(), 1U);
    const std::vector<float> ids = ids_of(emitted.front());
    ASSERT_EQ(ids.size(), 39U); // 10 quiet + 8 speech + 21 trailing
    EXPECT_FLOAT_EQ(ids.front(), 1.0F);
    EXPECT_FLOAT_EQ(ids.at(9), 10.0F);
    EXPECT_FLOAT_EQ(ids.at(10), 11.0F);
}

// CORE-09, Edge 18 boundary: with 11 quiet frames the earliest one is dropped.
TEST(QuietOnset, ElevenQuietFramesDropTheEarliest) {
    auto windower = lws::test::make_windower();
    EXPECT_TRUE(lws::test::feed_run(windower, 1.0F, 11, kQuiet).empty());
    EXPECT_TRUE(lws::test::feed_run(windower, 12.0F, 8, kSpeech).empty());
    const auto emitted = lws::test::feed_run(windower, 20.0F, 22, kQuiet);
    ASSERT_EQ(emitted.size(), 1U);
    EXPECT_FLOAT_EQ(lws::test::frame_id(emitted.front(), 0), 2.0F);
}

// CORE-09, Pitfall 1: 30 quiet frames leave exactly 10 lead-in frames (5120 samples), not the onset
// run eating the pre-roll.
TEST(PreRollTail, ThirtyQuietFramesLeaveExactlyPreRollFramesOfLeadIn) {
    auto windower = lws::test::make_windower();
    EXPECT_TRUE(lws::test::feed_run(windower, 1.0F, 30, kQuiet).empty());
    EXPECT_TRUE(lws::test::feed_run(windower, 31.0F, 8, kSpeech).empty());
    const auto emitted = lws::test::feed_run(windower, 39.0F, 22, kQuiet);
    ASSERT_EQ(emitted.size(), 1U);
    const std::vector<float> ids = ids_of(emitted.front());
    const auto lead_in = std::ranges::count_if(ids, [](float id) { return id < 31.0F; });
    EXPECT_EQ(lead_in, 10);
    EXPECT_EQ(lead_in * 512, 5120);
}

// D1, CORE-09: tail capacity is pre + onset - 1 frames: 10 + 8 - 1 = 17 frames = 8704 samples.
TEST(PreRollTail, CapacityIsPreRollPlusOnsetMinusOneFrames) {
    EXPECT_EQ(lws::preroll_tail_frames(10, 8), 17U);
    EXPECT_EQ(lws::preroll_tail_frames(10, 8) * 512U, 8704U);
    EXPECT_EQ(lws::preroll_tail_frames(0, 1), 0U);
}
