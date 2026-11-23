#include "lws/vad_windower.hpp"

#include "trace_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <utility>
#include <vector>

namespace {

constexpr float kSpeech = 0.9F;
constexpr float kQuiet = 0.1F;
constexpr std::size_t kFrame = lws::kVadFrameSamples;

using lws::test::feed_run;
using lws::test::frame_count;
using lws::test::frame_id;
using lws::test::segment_of;

// One part of a trace: count frames at prob, ids first_id upward.
struct Part {
    float first_id;
    std::size_t count;
    float prob;
};

// Feeds each part in order and returns everything emitted, in order.
std::vector<lws::Segment> replay(lws::VadWindower& windower, const std::vector<Part>& parts) {
    std::vector<lws::Segment> emitted;
    std::ranges::for_each(parts, [&](const Part& part) {
        std::ranges::move(feed_run(windower, part.first_id, part.count, part.prob),
                          std::back_inserter(emitted));
    });
    return emitted;
}

lws::Segment sole(std::vector<lws::Segment> emitted) {
    EXPECT_EQ(emitted.size(), 1U);
    return std::move(emitted.at(0));
}

// Two utterances with a pause: 20 quiet, 40 speech, 30 quiet, 15 quiet, 60 speech, 22 quiet.
std::vector<Part> two_utterances() {
    return {{1.0F, 20, kQuiet},  {21.0F, 40, kSpeech},  {61.0F, 30, kQuiet},
            {91.0F, 15, kQuiet}, {106.0F, 60, kSpeech}, {166.0F, 22, kQuiet}};
}

bool same(const lws::Segment& a, const lws::Segment& b) {
    return a.start == b.start && a.reason == b.reason &&
           a.continues_previous == b.continues_previous &&
           a.discontinuous_before == b.discontinuous_before && a.samples == b.samples;
}

} // namespace

// ROADMAP SC1, SC3: a pause longer than the hangover gives two Hangover segments, each with its own
// pre-roll. seg1 is 10 pre-roll + 40 speech + 21 trailing = 71 frames, from id 11. seg2 is 10
// pre-roll (quiet ids 96..105, kept by the tail after the closing frame id 82) + 60 speech + 21
// trailing = 91 frames, from id 96.
TEST(ReplayTwoUtterances, PauseSeparatesTwoHangoverSegmentsWithTheRightPreRoll) {
    auto windower = lws::test::make_windower();
    const auto emitted = replay(windower, two_utterances());
    ASSERT_EQ(emitted.size(), 2U);
    const lws::Segment& seg1 = emitted.at(0);
    const lws::Segment& seg2 = emitted.at(1);
    EXPECT_EQ(seg1.reason, lws::CloseReason::Hangover);
    EXPECT_EQ(seg2.reason, lws::CloseReason::Hangover);
    EXPECT_FALSE(seg1.continues_previous);
    EXPECT_FALSE(seg2.continues_previous);
    EXPECT_EQ(frame_count(seg1), 71U);
    EXPECT_FLOAT_EQ(frame_id(seg1, 0), 11.0F);
    EXPECT_EQ(frame_count(seg2), 91U);
    EXPECT_FLOAT_EQ(frame_id(seg2, 0), 96.0F);
}

// CORE-04: a 5-frame dip inside an utterance is shorter than the hangover, so it stays one segment.
// 8 speech + 5 dip + 8 speech = 21 frames, then 21 trailing; the 22nd closes and is out.
TEST(ReplayDip, ShortGapInsideAnUtteranceStaysOneSegment) {
    auto windower = lws::test::make_windower();
    const lws::Segment seg = sole(
        replay(windower,
               {{1.0F, 8, kSpeech}, {9.0F, 5, kQuiet}, {14.0F, 8, kSpeech}, {22.0F, 22, kQuiet}}));
    EXPECT_EQ(frame_count(seg), 42U);
    EXPECT_FLOAT_EQ(frame_id(seg, 0), 1.0F);
}

// D-02, D5: 440 frames of speech cut once at the ceiling; flush emits the 57-frame continuation
// (440 - 383) with the flag kept and no discontinuity.
TEST(ReplayCeilingThenFlush, ContinuationEndsWithFlushAndKeepsTheFlag) {
    auto windower = lws::test::make_windower();
    const lws::Segment cut = sole(feed_run(windower, 0.0F, 440, kSpeech));
    EXPECT_EQ(cut.reason, lws::CloseReason::Ceiling);
    EXPECT_EQ(frame_count(cut), 391U);
    EXPECT_FALSE(cut.continues_previous);
    const lws::Segment rest = segment_of(windower.flush());
    EXPECT_EQ(rest.reason, lws::CloseReason::Flush);
    EXPECT_TRUE(rest.continues_previous);
    EXPECT_FALSE(rest.discontinuous_before);
    EXPECT_EQ(frame_count(rest), 57U);
    EXPECT_EQ(rest.start, 383U * kFrame);
}

// D-05, T-02-05: after a force close the resumed utterance holds none of the closed audio.
TEST(ReplayForceCloseThenResume, ResumedUtteranceHasNoPreCloseAudio) {
    auto windower = lws::test::make_windower();
    EXPECT_TRUE(feed_run(windower, 1.0F, 8, kSpeech).empty());
    const lws::Segment closed = segment_of(windower.force_close());
    EXPECT_EQ(closed.reason, lws::CloseReason::Discontinuity);
    EXPECT_EQ(frame_count(closed), 8U);
    EXPECT_FALSE(closed.discontinuous_before);
    const lws::Segment seg = sole(replay(windower, {{9.0F, 8, kSpeech}, {17.0F, 22, kQuiet}}));
    EXPECT_EQ(frame_count(seg), 29U);
    EXPECT_FLOAT_EQ(frame_id(seg, 0), 9.0F);
    EXPECT_FALSE(seg.discontinuous_before);
    EXPECT_EQ(seg.start, 8U * kFrame);
}

// D-05, user answer 3, T-02-05: the same trace with a notify. The open window closes at the gap and
// the flag lands on the resumed window instead.
TEST(ReplayNotifyThenResume, GapClosesTheOpenWindowAndFlagsTheNextOne) {
    auto windower = lws::test::make_windower();
    EXPECT_TRUE(feed_run(windower, 1.0F, 8, kSpeech).empty());
    const lws::Segment closed = segment_of(windower.notify_discontinuity());
    EXPECT_EQ(closed.reason, lws::CloseReason::Discontinuity);
    EXPECT_EQ(frame_count(closed), 8U);
    EXPECT_FALSE(closed.discontinuous_before);
    EXPECT_FALSE(closed.continues_previous);
    const lws::Segment seg = sole(replay(windower, {{9.0F, 8, kSpeech}, {17.0F, 22, kQuiet}}));
    EXPECT_EQ(frame_count(seg), 29U);
    EXPECT_FLOAT_EQ(frame_id(seg, 0), 9.0F);
    EXPECT_TRUE(seg.discontinuous_before);
    EXPECT_FALSE(seg.continues_previous);
    EXPECT_EQ(seg.start, 8U * kFrame);
}

// ROADMAP SC1: the same trace through two fresh windowers gives identical segments.
TEST(ReplayDeterminism, SameTraceGivesIdenticalSegments) {
    auto first = lws::test::make_windower();
    auto second = lws::test::make_windower();
    const auto a = replay(first, two_utterances());
    const auto b = replay(second, two_utterances());
    ASSERT_EQ(a.size(), 2U);
    ASSERT_EQ(a.size(), b.size());
    EXPECT_TRUE(std::ranges::equal(a, b, same));
}
