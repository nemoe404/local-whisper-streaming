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
// Default options: a 14 s ceiling holds 438 frames and cuts 391 of them; the remainder keeps 55.
constexpr std::size_t kCeilingFrames = 438;
constexpr std::size_t kCutFrames = 391;
constexpr std::size_t kRemainderFrames = 55;

using lws::test::feed_run;
using lws::test::frame_count;
using lws::test::frame_id;
using lws::test::segment_of;

// 8 speech frames open a window, 22 sub-threshold frames close it: 29 frames, ids
// first_id..first_id+7 then the 21 trailing ones. Returns what the 30 feeds emitted.
std::vector<lws::Segment> utterance(lws::VadWindower& windower, float first_id) {
    std::vector<lws::Segment> emitted = feed_run(windower, first_id, 8, kSpeech);
    std::ranges::move(feed_run(windower, first_id + 8.0F, 22, kQuiet), std::back_inserter(emitted));
    return emitted;
}

// The one segment an utterance must have produced.
lws::Segment sole(std::vector<lws::Segment> emitted) {
    EXPECT_EQ(emitted.size(), 1U);
    return std::move(emitted.at(0));
}

// A windower holding the first segment's worth of a continuous 438-frame speech run (ids 0..437):
// the ceiling cut has fired and the 55-frame continuation is open. Returns the cut segment.
lws::Segment ceiling_cut(lws::VadWindower& windower) {
    return sole(feed_run(windower, 0.0F, kCeilingFrames, kSpeech));
}

} // namespace

// A force close emits the open window as is, never truncated, with the
// Discontinuity reason. The window was born without a notice, so the flag stays false.
TEST(ForceClose, OpenWindowIsEmittedAsIsWithDiscontinuityAndNeverTruncated) {
    auto windower = lws::test::make_windower();
    EXPECT_TRUE(lws::test::feed_run(windower, 1.0F, 8, kSpeech).empty());
    const lws::Segment closed = lws::test::segment_of(windower.force_close());
    EXPECT_EQ(closed.reason, lws::CloseReason::Discontinuity);
    EXPECT_FALSE(closed.discontinuous_before);
    EXPECT_FALSE(closed.continues_previous);
    EXPECT_EQ(lws::test::frame_count(closed), 8U);
    EXPECT_EQ(closed.start, 0U);
    EXPECT_FLOAT_EQ(lws::test::frame_id(closed, 0), 1.0F);
    EXPECT_FLOAT_EQ(lws::test::frame_id(closed, 7), 8.0F);
    EXPECT_FALSE(windower.force_close().has_value());
}

// A force close clears the tail, so the next window holds none of ids 1..8.
TEST(ForceClose, ClearsTheTailSoTheNextWindowHasNoPreCloseAudio) {
    auto windower = lws::test::make_windower();
    EXPECT_TRUE(lws::test::feed_run(windower, 1.0F, 8, kSpeech).empty());
    EXPECT_TRUE(windower.force_close().has_value());
    EXPECT_TRUE(lws::test::feed_run(windower, 9.0F, 8, kSpeech).empty());
    const auto emitted = lws::test::feed_run(windower, 17.0F, 22, kQuiet);
    ASSERT_EQ(emitted.size(), 1U);
    EXPECT_FLOAT_EQ(lws::test::frame_id(emitted.front(), 0), 9.0F);
    EXPECT_FALSE(emitted.front().discontinuous_before);
    EXPECT_EQ(emitted.front().start, 8U * kFrame);
}

// (a) The idle clear drops the quiet pre-roll. (b) It restarts the onset streak.
TEST(ForceClose, IdleForceCloseClearsThePreRollAndTheStreak) {
    {
        auto windower = lws::test::make_windower();
        EXPECT_TRUE(feed_run(windower, 1.0F, 5, kQuiet).empty());
        EXPECT_FALSE(windower.force_close().has_value());
        const lws::Segment seg = sole(utterance(windower, 6.0F));
        EXPECT_FLOAT_EQ(frame_id(seg, 0), 6.0F);
        EXPECT_EQ(frame_count(seg), 29U);
    }
    {
        auto windower = lws::test::make_windower();
        EXPECT_TRUE(feed_run(windower, 1.0F, 7, kSpeech).empty());
        EXPECT_FALSE(windower.force_close().has_value());
        EXPECT_TRUE(feed_run(windower, 8.0F, 1, kSpeech).empty());
        EXPECT_TRUE(feed_run(windower, 9.0F, 22, kQuiet).empty());
    }
}

// A continuation that is force closed keeps continues_previous. 438 frames give a
// 391-frame ceiling cut and a 55-frame open remainder starting at frame 383.
TEST(ForceClose, ContinuationForceClosedKeepsContinuesPrevious) {
    auto windower = lws::test::make_windower();
    const lws::Segment cut = ceiling_cut(windower);
    EXPECT_EQ(cut.reason, lws::CloseReason::Ceiling);
    EXPECT_EQ(frame_count(cut), kCutFrames);
    EXPECT_FALSE(cut.continues_previous);
    const lws::Segment rest = segment_of(windower.force_close());
    EXPECT_EQ(rest.reason, lws::CloseReason::Discontinuity);
    EXPECT_TRUE(rest.continues_previous);
    EXPECT_FALSE(rest.discontinuous_before);
    EXPECT_EQ(frame_count(rest), kRemainderFrames);
    EXPECT_EQ(rest.start, 383U * kFrame);
}

// Settled 2026-11-23: a window born flagged keeps the flag when a force close emits it, and
// the notice was consumed by the birth, so the next window is not flagged.
TEST(ForceClose, OpenWindowBornFlaggedKeepsItsFlag) {
    auto windower = lws::test::make_windower();
    EXPECT_FALSE(windower.notify_discontinuity().has_value());
    EXPECT_TRUE(feed_run(windower, 1.0F, 8, kSpeech).empty());
    const lws::Segment closed = segment_of(windower.force_close());
    EXPECT_EQ(closed.reason, lws::CloseReason::Discontinuity);
    EXPECT_TRUE(closed.discontinuous_before);
    EXPECT_EQ(frame_count(closed), 8U);
    const lws::Segment next = sole(utterance(windower, 9.0F));
    EXPECT_FALSE(next.discontinuous_before);
}

// Flush emits with the Flush reason and adds no flag.
TEST(Flush, OpenWindowIsEmittedWithFlushAndNoFlagAdded) {
    auto windower = lws::test::make_windower();
    EXPECT_TRUE(feed_run(windower, 1.0F, 8, kSpeech).empty());
    const lws::Segment closed = segment_of(windower.flush());
    EXPECT_EQ(closed.reason, lws::CloseReason::Flush);
    EXPECT_FALSE(closed.discontinuous_before);
    EXPECT_EQ(frame_count(closed), 8U);
    EXPECT_FALSE(windower.flush().has_value());
}

// The flag on a flushed window is its birth value, not a forced false.
TEST(Flush, OpenWindowFlagIsItsBirthValueNotForcedFalse) {
    auto windower = lws::test::make_windower();
    EXPECT_FALSE(windower.notify_discontinuity().has_value());
    EXPECT_TRUE(feed_run(windower, 1.0F, 8, kSpeech).empty());
    const lws::Segment closed = segment_of(windower.flush());
    EXPECT_EQ(closed.reason, lws::CloseReason::Flush);
    EXPECT_TRUE(closed.discontinuous_before);
}

// Flush with no window returns nothing and restarts the onset streak.
TEST(Flush, WithNoWindowReturnsNothingAndResetsTheOnsetStreak) {
    auto windower = lws::test::make_windower();
    EXPECT_TRUE(feed_run(windower, 1.0F, 7, kSpeech).empty());
    EXPECT_FALSE(windower.flush().has_value());
    EXPECT_TRUE(feed_run(windower, 8.0F, 1, kSpeech).empty());
    EXPECT_TRUE(feed_run(windower, 9.0F, 22, kQuiet).empty());
}

// The idle flush drops the quiet pre-roll. On an open window the tail is already empty, so
// this idle case is the one that proves the clear.
TEST(Flush, IdleFlushClearsThePreRoll) {
    auto windower = lws::test::make_windower();
    EXPECT_TRUE(feed_run(windower, 1.0F, 5, kQuiet).empty());
    EXPECT_FALSE(windower.flush().has_value());
    const lws::Segment seg = sole(utterance(windower, 6.0F));
    EXPECT_FLOAT_EQ(frame_id(seg, 0), 6.0F);
    EXPECT_EQ(frame_count(seg), 29U);
}

// A notice raised in silence flags the nearest window born after it, and only that
// one.
TEST(Notice, NotifyDuringSilenceFlagsOnlyTheNearestFollowingWindow) {
    auto windower = lws::test::make_windower();
    EXPECT_TRUE(feed_run(windower, 1.0F, 3, kQuiet).empty());
    EXPECT_FALSE(windower.notify_discontinuity().has_value());
    EXPECT_TRUE(sole(utterance(windower, 4.0F)).discontinuous_before);
    EXPECT_FALSE(sole(utterance(windower, 34.0F)).discontinuous_before);
}

// A notice closes the open window now; the flag lands on the next
// one.
TEST(Notice, NotifyDuringAnOpenWindowClosesItAndFlagsTheWindowAfterIt) {
    auto windower = lws::test::make_windower();
    EXPECT_TRUE(feed_run(windower, 1.0F, 8, kSpeech).empty());
    const lws::Segment closed = segment_of(windower.notify_discontinuity());
    EXPECT_EQ(closed.reason, lws::CloseReason::Discontinuity);
    EXPECT_FALSE(closed.discontinuous_before);
    EXPECT_EQ(frame_count(closed), 8U);
    EXPECT_TRUE(feed_run(windower, 9.0F, 22, kQuiet).empty());
    EXPECT_TRUE(sole(utterance(windower, 31.0F)).discontinuous_before);
}

// The open window is emitted as is, never truncated, with its birth flags
// kept.
TEST(Notice, OpenWindowIsEmittedWithDiscontinuityAndBirthFlagsKept) {
    {
        auto windower = lws::test::make_windower();
        EXPECT_TRUE(feed_run(windower, 1.0F, 8, kSpeech).empty());
        const lws::Segment closed = segment_of(windower.notify_discontinuity());
        EXPECT_EQ(closed.reason, lws::CloseReason::Discontinuity);
        EXPECT_FALSE(closed.discontinuous_before);
        EXPECT_FALSE(closed.continues_previous);
        EXPECT_EQ(frame_count(closed), 8U);
        EXPECT_EQ(closed.start, 0U);
        EXPECT_FLOAT_EQ(frame_id(closed, 0), 1.0F);
        EXPECT_FLOAT_EQ(frame_id(closed, 7), 8.0F);
        EXPECT_FALSE(windower.notify_discontinuity().has_value());
    }
    {
        auto windower = lws::test::make_windower();
        EXPECT_FALSE(windower.notify_discontinuity().has_value());
        EXPECT_TRUE(feed_run(windower, 1.0F, 8, kSpeech).empty());
        const lws::Segment closed = segment_of(windower.notify_discontinuity());
        EXPECT_EQ(closed.reason, lws::CloseReason::Discontinuity);
        EXPECT_TRUE(closed.discontinuous_before);
    }
}

// A continuation closed by a notice keeps continues_previous.
TEST(Notice, ContinuationClosedByNotifyKeepsContinuesPrevious) {
    auto windower = lws::test::make_windower();
    EXPECT_EQ(frame_count(ceiling_cut(windower)), kCutFrames);
    const lws::Segment rest = segment_of(windower.notify_discontinuity());
    EXPECT_EQ(rest.reason, lws::CloseReason::Discontinuity);
    EXPECT_TRUE(rest.continues_previous);
    EXPECT_FALSE(rest.discontinuous_before);
    EXPECT_EQ(frame_count(rest), kRemainderFrames);
    EXPECT_EQ(rest.start, 383U * kFrame);
}

// An idle notify returns nothing and stays armed.
TEST(Notice, IdleNotifyReturnsNullopt) {
    auto windower = lws::test::make_windower();
    EXPECT_FALSE(windower.notify_discontinuity().has_value());
    EXPECT_TRUE(feed_run(windower, 1.0F, 22, kQuiet).empty());
    EXPECT_FALSE(windower.notify_discontinuity().has_value());
    EXPECT_TRUE(sole(utterance(windower, 23.0F)).discontinuous_before);
}

// The window born after a notify is flagged and is not a continuation.
TEST(Notice, WindowAfterNotifyIsFlaggedAndNotContinued) {
    {
        auto windower = lws::test::make_windower();
        EXPECT_TRUE(feed_run(windower, 1.0F, 8, kSpeech).empty());
        EXPECT_TRUE(windower.notify_discontinuity().has_value());
        const lws::Segment seg = sole(utterance(windower, 9.0F));
        EXPECT_TRUE(seg.discontinuous_before);
        EXPECT_FALSE(seg.continues_previous);
    }
    {
        auto windower = lws::test::make_windower();
        EXPECT_EQ(frame_count(ceiling_cut(windower)), kCutFrames);
        const lws::Segment rest = segment_of(windower.notify_discontinuity());
        EXPECT_TRUE(rest.continues_previous);
        const lws::Segment seg = sole(utterance(windower, 438.0F));
        EXPECT_FALSE(seg.continues_previous);
        EXPECT_TRUE(seg.discontinuous_before);
        EXPECT_EQ(frame_count(seg), 29U);
        EXPECT_EQ(seg.start, 438U * kFrame);
    }
}

// The notify clears the hangover counter. A counter left at 10 would close the next window
// early with 19 frames.
TEST(Notice, NotifyClearsTheHangoverCounter) {
    auto windower = lws::test::make_windower();
    EXPECT_TRUE(feed_run(windower, 1.0F, 8, kSpeech).empty());
    EXPECT_TRUE(feed_run(windower, 9.0F, 10, kQuiet).empty());
    EXPECT_EQ(frame_count(segment_of(windower.notify_discontinuity())), 18U);
    const lws::Segment seg = sole(utterance(windower, 19.0F));
    EXPECT_EQ(frame_count(seg), 29U);
    EXPECT_TRUE(seg.discontinuous_before);
}

// The notify clears the probability history. A history left at 18 entries would cut 18 frames
// early.
TEST(Notice, NotifyClearsTheProbabilityHistory) {
    auto windower = lws::test::make_windower();
    EXPECT_TRUE(feed_run(windower, 1.0F, 8, kSpeech).empty());
    EXPECT_TRUE(feed_run(windower, 9.0F, 10, kQuiet).empty());
    EXPECT_TRUE(windower.notify_discontinuity().has_value());
    EXPECT_TRUE(feed_run(windower, 19.0F, kCeilingFrames - 1, kSpeech).empty());
    const lws::Segment cut =
        sole(feed_run(windower, 19.0F + static_cast<float>(kCeilingFrames - 1), 1, kSpeech));
    EXPECT_EQ(cut.reason, lws::CloseReason::Ceiling);
    EXPECT_EQ(frame_count(cut), kCutFrames);
    EXPECT_TRUE(cut.discontinuous_before);
    EXPECT_FALSE(cut.continues_previous);
}

// A force close with no window keeps the pending notice.
TEST(Notice, ForceCloseKeepsThePendingNotice) {
    auto windower = lws::test::make_windower();
    EXPECT_FALSE(windower.notify_discontinuity().has_value());
    EXPECT_FALSE(windower.force_close().has_value());
    EXPECT_TRUE(sole(utterance(windower, 1.0F)).discontinuous_before);
}

// A flush with no window keeps the pending notice.
TEST(Notice, FlushKeepsThePendingNotice) {
    auto windower = lws::test::make_windower();
    EXPECT_FALSE(windower.notify_discontinuity().has_value());
    EXPECT_FALSE(windower.flush().has_value());
    EXPECT_TRUE(sole(utterance(windower, 1.0F)).discontinuous_before);
}

// Idempotent: (a) idle, (b) open. Both leave one flag, consumed by one window.
TEST(Notice, RepeatedNotifyIsOneFlag) {
    {
        auto windower = lws::test::make_windower();
        EXPECT_FALSE(windower.notify_discontinuity().has_value());
        EXPECT_FALSE(windower.notify_discontinuity().has_value());
        EXPECT_TRUE(sole(utterance(windower, 1.0F)).discontinuous_before);
        EXPECT_FALSE(sole(utterance(windower, 31.0F)).discontinuous_before);
    }
    {
        auto windower = lws::test::make_windower();
        EXPECT_TRUE(feed_run(windower, 1.0F, 8, kSpeech).empty());
        EXPECT_TRUE(windower.notify_discontinuity().has_value());
        EXPECT_FALSE(windower.notify_discontinuity().has_value());
        EXPECT_TRUE(sole(utterance(windower, 9.0F)).discontinuous_before);
        EXPECT_FALSE(sole(utterance(windower, 39.0F)).discontinuous_before);
    }
}

// The notify restarts the onset streak (a) and drops the quiet pre-roll (b).
TEST(Notice, NotifyClearsThePreRollAndTheOnsetStreak) {
    {
        auto windower = lws::test::make_windower();
        EXPECT_TRUE(feed_run(windower, 1.0F, 7, kSpeech).empty());
        EXPECT_FALSE(windower.notify_discontinuity().has_value());
        EXPECT_TRUE(feed_run(windower, 8.0F, 1, kSpeech).empty());
        EXPECT_TRUE(feed_run(windower, 9.0F, 22, kQuiet).empty());
    }
    {
        auto windower = lws::test::make_windower();
        EXPECT_TRUE(feed_run(windower, 1.0F, 5, kQuiet).empty());
        EXPECT_FALSE(windower.notify_discontinuity().has_value());
        const lws::Segment seg = sole(utterance(windower, 6.0F));
        EXPECT_FLOAT_EQ(frame_id(seg, 0), 6.0F);
        EXPECT_TRUE(seg.discontinuous_before);
    }
}

// After a notify closed the window, a force close finds nothing and keeps the notice.
TEST(Notice, ForceCloseAfterNotifyEmitsNothingAndKeepsTheNotice) {
    auto windower = lws::test::make_windower();
    EXPECT_TRUE(feed_run(windower, 1.0F, 8, kSpeech).empty());
    const lws::Segment closed = segment_of(windower.notify_discontinuity());
    EXPECT_EQ(closed.reason, lws::CloseReason::Discontinuity);
    EXPECT_FALSE(closed.discontinuous_before);
    EXPECT_FALSE(windower.force_close().has_value());
    EXPECT_TRUE(sole(utterance(windower, 9.0F)).discontinuous_before);
}

// After a notify closed the window, a flush finds nothing and keeps the notice.
TEST(Notice, FlushAfterNotifyEmitsNothingAndKeepsTheNotice) {
    auto windower = lws::test::make_windower();
    EXPECT_TRUE(feed_run(windower, 1.0F, 8, kSpeech).empty());
    EXPECT_TRUE(windower.notify_discontinuity().has_value());
    EXPECT_FALSE(windower.flush().has_value());
    EXPECT_TRUE(sole(utterance(windower, 9.0F)).discontinuous_before);
}

// A ceiling remainder never carries the flag, although the cut part does when
// its window was born after a notice.
TEST(CeilingRemainder, NeverCarriesTheDiscontinuityFlag) {
    auto windower = lws::test::make_windower();
    EXPECT_FALSE(windower.notify_discontinuity().has_value());
    const lws::Segment cut = ceiling_cut(windower);
    EXPECT_EQ(cut.reason, lws::CloseReason::Ceiling);
    EXPECT_EQ(frame_count(cut), kCutFrames);
    EXPECT_TRUE(cut.discontinuous_before);
    const lws::Segment rest = sole(feed_run(windower, 438.0F, 22, kQuiet));
    EXPECT_EQ(rest.reason, lws::CloseReason::Hangover);
    EXPECT_TRUE(rest.continues_previous);
    EXPECT_FALSE(rest.discontinuous_before);
    EXPECT_EQ(frame_count(rest), kRemainderFrames + 21U);
}

// The leak test: a window born after a continuation is not itself a continuation.
TEST(Latch, NewWindowAfterAContinuationIsNotContinued) {
    auto windower = lws::test::make_windower();
    EXPECT_EQ(frame_count(ceiling_cut(windower)), kCutFrames);
    const lws::Segment rest = sole(feed_run(windower, 438.0F, 22, kQuiet));
    EXPECT_EQ(rest.reason, lws::CloseReason::Hangover);
    EXPECT_TRUE(rest.continues_previous);
    EXPECT_EQ(frame_count(rest), kRemainderFrames + 21U);
    const lws::Segment next = sole(utterance(windower, 460.0F));
    EXPECT_FALSE(next.continues_previous);
    EXPECT_FALSE(next.discontinuous_before);
}
