#include "lws/vad_windower.hpp"

#include "trace_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace {

constexpr float kSpeech = 0.9F;
constexpr float kQuiet = 0.1F;

// count frames at kSpeech, with the listed (index, probability) overrides.
std::vector<float> speech_trace(std::size_t count,
                                std::initializer_list<std::pair<std::size_t, float>> overrides) {
    std::vector<float> probs(count, kSpeech);
    std::ranges::for_each(overrides, [&](const std::pair<std::size_t, float>& entry) {
        probs.at(entry.first) = entry.second;
    });
    return probs;
}

// Feeds one frame per probability, ids first_id, first_id + 1, ... Returns each emitted segment.
std::vector<lws::Segment> feed_trace(lws::VadWindower& windower, std::span<const float> probs,
                                     float first_id = 0.0F) {
    std::vector<lws::Segment> emitted;
    float id = first_id;
    std::ranges::for_each(probs, [&](float prob) {
        if (auto segment = windower.feed(lws::test::frame_of(id), prob)) {
            emitted.push_back(std::move(*segment));
        }
        id += 1.0F;
    });
    return emitted;
}

lws::Options short_ceiling_options() {
    lws::Options options;
    options.max_speech_duration_s = 0.6F; // 19 frames; 19 > 10 + 8 so it validates
    return options;
}

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

// CORE-05, Edge 10: the minimum inside the last 47 frames wins. Index 380 is lower but sits outside
// the search span [391, 438), so it is ignored.
TEST(CutSearch, MinimumInsideTheLastFortySevenFramesIsChosen) {
    auto windower = lws::test::make_windower();
    const auto trace = speech_trace(438, {{400, 0.55F}, {380, 0.52F}});
    const auto emitted = feed_trace(windower, trace);
    ASSERT_EQ(emitted.size(), 1U);
    EXPECT_EQ(lws::test::frame_count(emitted.front()), 400U);
    EXPECT_EQ(emitted.front().reason, lws::CloseReason::Ceiling);
}

// Edge 10 boundary: index 391 is the first frame of the span, index 390 is just outside it.
TEST(CutSearch, FirstFrameOfTheSpanIsIncluded) {
    auto windower = lws::test::make_windower();
    const auto trace = speech_trace(438, {{390, 0.5F}, {391, 0.55F}});
    const auto emitted = feed_trace(windower, trace);
    ASSERT_EQ(emitted.size(), 1U);
    EXPECT_EQ(lws::test::frame_count(emitted.front()), 391U);
}

// CORE-05, Edge 12: equal minima resolve to the earliest frame.
TEST(CutSearch, TiesResolveToTheEarliestFrame) {
    auto windower = lws::test::make_windower();
    const auto trace = speech_trace(438, {{400, 0.55F}, {420, 0.55F}});
    const auto emitted = feed_trace(windower, trace);
    ASSERT_EQ(emitted.size(), 1U);
    EXPECT_EQ(lws::test::frame_count(emitted.front()), 400U);
}

// CORE-05, Edge 11: the minimum at index 0 clamps to 1, so a cut never emits an empty segment.
TEST(CutSearch, ClampKeepsTheCutAboveZero) {
    auto windower = lws::test::make_windower(short_ceiling_options());
    const auto trace = speech_trace(19, {{0, 0.5F}});
    const auto emitted = feed_trace(windower, trace);
    ASSERT_EQ(emitted.size(), 1U);
    EXPECT_EQ(lws::test::frame_count(emitted.front()), 1U);
    EXPECT_EQ(emitted.front().reason, lws::CloseReason::Ceiling);
}

// CORE-06, D2: 250 ms rounds up to 8 frames. The cut is at frame 400, the remainder starts at frame
// 392: 8 frames, which is 8 * 512 == 4096 samples (256 ms).
TEST(OverlapDefault, RemainderStartsEightFramesBeforeTheCutAsFourThousandNinetySixSamples) {
    auto windower = lws::test::make_windower();
    const auto trace = speech_trace(438, {{400, 0.55F}});
    const auto cut = feed_trace(windower, trace);
    ASSERT_EQ(cut.size(), 1U);
    EXPECT_EQ(lws::test::frame_count(cut.front()), 400U);
    EXPECT_TRUE(lws::test::feed_run(windower, 438.0F, 21, kQuiet).empty());
    const auto closed = lws::test::feed_run(windower, 459.0F, 1, kQuiet);
    ASSERT_EQ(closed.size(), 1U);
    const lws::Segment& second = closed.front();
    EXPECT_EQ(second.start, 392U * lws::kVadFrameSamples);
    EXPECT_EQ((400U * lws::kVadFrameSamples) - second.start, 8U * 512U);
    EXPECT_EQ((400U * lws::kVadFrameSamples) - second.start, 4096U);
    EXPECT_FLOAT_EQ(lws::test::frame_id(second, 0), 392.0F);
}

// Edge 17, D-02, D-03: with no notice, every birth latches discontinuous_before = false. The cut
// segment keeps its birth flags; the remainder sets continues_previous only.
TEST(CutLatch, CutSegmentsCarryTheLatchedFlagsAndTheRemainderSetsContinuesOnly) {
    auto windower = lws::test::make_windower();
    const auto trace = speech_trace(438, {{400, 0.55F}});
    const auto cut = feed_trace(windower, trace);
    ASSERT_EQ(cut.size(), 1U);
    EXPECT_FALSE(cut.front().continues_previous);
    EXPECT_FALSE(cut.front().discontinuous_before);
    EXPECT_TRUE(lws::test::feed_run(windower, 438.0F, 21, kQuiet).empty());
    const auto closed = lws::test::feed_run(windower, 459.0F, 1, kQuiet);
    ASSERT_EQ(closed.size(), 1U);
    EXPECT_TRUE(closed.front().continues_previous);
    EXPECT_FALSE(closed.front().discontinuous_before);
}

// D-07 (user, 2026-11-23): the overlap is min(overlap_frames, cut - 1). A cut at frame 3 with an
// 8-frame overlap removes 3 - 2 = 1 frame, so the remainder starts at frame 1. The literal rule
// min(overlap, cut) = 3 would remove nothing (remainder start 0) and stall. With flat
// probabilities a small ceiling cuts after every frame or two; that is open question Q9 (accepted),
// not a defect of this test.
TEST(CutOverlapRule, CutAtFrameThreeLeavesARemainderThatStartsAtFrameOne) {
    auto windower = lws::test::make_windower(short_ceiling_options());
    const auto trace = speech_trace(19, {{3, 0.5F}});
    const auto first = feed_trace(windower, trace);
    ASSERT_EQ(first.size(), 1U);
    EXPECT_EQ(lws::test::frame_count(first.front()), 3U);
    EXPECT_EQ(first.front().start, 0U);
    EXPECT_EQ(first.front().reason, lws::CloseReason::Ceiling);

    const auto second = lws::test::feed_run(windower, 19.0F, 1, kSpeech);
    ASSERT_EQ(second.size(), 1U);
    EXPECT_EQ(second.front().start, 1U * lws::kVadFrameSamples);
    EXPECT_FLOAT_EQ(lws::test::frame_id(second.front(), 0), 1.0F);
}

// CORE-06, D-08, D6: overlap_ms = 0 starts the remainder at the cut and turns the seam-merge query
// off. continues_previous still holds, because the remainder is still the continuation.
TEST(OverlapZero, DisablesTheOverlapAndTheSeamMergeQuery) {
    lws::Options options;
    options.overlap_ms = 0;
    auto windower = lws::test::make_windower(options);
    const auto trace = speech_trace(438, {{400, 0.55F}});
    const auto cut = feed_trace(windower, trace);
    ASSERT_EQ(cut.size(), 1U);
    EXPECT_EQ(lws::test::frame_count(cut.front()), 400U);
    EXPECT_TRUE(lws::test::feed_run(windower, 438.0F, 21, kQuiet).empty());
    const auto closed = lws::test::feed_run(windower, 459.0F, 1, kQuiet);
    ASSERT_EQ(closed.size(), 1U);
    const lws::Segment& second = closed.front();
    EXPECT_EQ(second.start, 400U * lws::kVadFrameSamples);
    EXPECT_FLOAT_EQ(lws::test::frame_id(second, 0), 400.0F);
    EXPECT_TRUE(second.continues_previous);
    EXPECT_FALSE(lws::seam_merge_enabled(options));
    EXPECT_TRUE(lws::seam_merge_enabled(lws::Options{}));
}

// CORE-08, D-02: three ceiling cuts in a row (at 438, 821 and 1204 frames fed). Only the first
// segment is born outside a continuation; every later one continues the previous.
TEST(ContinuationChain, ARemainderThatIsCutAgainKeepsContinuesPrevious) {
    auto windower = lws::test::make_windower();
    const auto emitted = lws::test::feed_run(windower, 0.0F, std::size_t{438} * 3, kSpeech);
    ASSERT_GE(emitted.size(), 3U);
    EXPECT_FALSE(emitted[0].continues_previous);
    EXPECT_TRUE(emitted[1].continues_previous);
    EXPECT_EQ(emitted[1].reason, lws::CloseReason::Ceiling);
    EXPECT_TRUE(std::ranges::all_of(std::span(emitted).subspan(1), [](const lws::Segment& segment) {
        return segment.continues_previous;
    }));
}

// T-02-04: the window never grows past the ceiling. This pins bounded memory, not a cut rate: with
// flat probabilities a 19-frame ceiling cuts after nearly every frame (open question Q9, accepted).
TEST(BoundedShortCeiling, SteadySpeechWithNineteenFrameCeilingNeverGrowsPastTheCeiling) {
    auto windower = lws::test::make_windower(short_ceiling_options());
    const auto emitted = lws::test::feed_run(windower, 0.0F, 300, kSpeech);
    EXPECT_GE(emitted.size(), 10U);
    EXPECT_TRUE(std::ranges::all_of(emitted, [](const lws::Segment& segment) {
        return lws::test::frame_count(segment) <= 19U &&
               segment.reason == lws::CloseReason::Ceiling;
    }));
}

#ifdef NDEBUG
// D11b: a NaN probability is stored as 0.0F in the cut history, so the cut picks it. The frame is
// still sub-threshold. Release only: Debug builds trip the range assert in feed() first. A NaN left
// in the history would never be picked by min_element, so this pins the 0.0F rule.
TEST(NaNHistory, NaNProbabilityIsStoredAsZeroSoTheCutPicksIt) {
    auto windower = lws::test::make_windower(short_ceiling_options());
    EXPECT_TRUE(lws::test::feed_run(windower, 0.0F, 12, kSpeech).empty());
    EXPECT_TRUE(
        lws::test::feed_run(windower, 12.0F, 1, std::numeric_limits<float>::quiet_NaN()).empty());
    const auto emitted = lws::test::feed_run(windower, 13.0F, 6, kSpeech);
    ASSERT_EQ(emitted.size(), 1U);
    EXPECT_EQ(emitted.front().reason, lws::CloseReason::Ceiling);
    EXPECT_EQ(lws::test::frame_count(emitted.front()), 12U);
}
#endif
