#include "core/stream_clock.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "core/segment_fifo.hpp"
#include "lws/segment.hpp"
#include "lws/text_event.hpp"

namespace {

using lws::EventKind;
using lws::Notice;
using lws::StreamPosition;

// 1000 samples taken, none lost: the windower has been fed 512 and the reader holds 488.
constexpr StreamPosition kNoLoss{.popped = 1000, .skew = 0, .pending = 488, .fed = 512};
// A 300-sample gap at 1000 and the reset after it: the reader is empty and the skew is 300.
constexpr StreamPosition kAfterGap{.popped = 1000, .skew = 300, .pending = 0, .fed = 512};
// A failed call of 2048 samples with nothing fed before it.
constexpr StreamPosition kAllLost{.popped = 2048, .skew = 0, .pending = 0, .fed = 0};

static_assert(lws::stream_offset(kNoLoss) == 0);
static_assert(lws::stream_offset(kAfterGap) == 788);
static_assert(lws::stream_offset(kAllLost) == 2048);
static_assert(lws::next_frame_position(kNoLoss) == 512);
static_assert(lws::next_frame_position(kAfterGap) == 1300);

} // namespace

TEST(StreamClock, TheThreeWorkedOffsets) {
    EXPECT_EQ(lws::stream_offset(kNoLoss), 0U);
    EXPECT_EQ(lws::stream_offset(kAfterGap), 788U);
    EXPECT_EQ(lws::stream_offset(kAllLost), 2048U);
}

TEST(StreamClock, TheNextFrameSitsAtPoppedPlusSkewMinusPending) {
    EXPECT_EQ(lws::next_frame_position(kNoLoss), 512U);
    // Stream sample 1300 is windower sample 512, so the next frame after the gap is at 1300.
    EXPECT_EQ(lws::next_frame_position(kAfterGap), 1300U);
    EXPECT_EQ(lws::next_frame_position(kAfterGap), 512U + lws::stream_offset(kAfterGap));
}

TEST(StreamClock, StampedAddsTheOffsetToTheStartAndKeepsTheRest) {
    const lws::Segment segment{.samples = std::vector<float>(1024, 0.5F),
                               .start = 4096,
                               .reason = lws::CloseReason::Ceiling,
                               .continues_previous = true,
                               .discontinuous_before = true};
    const lws::Segment out = lws::stamped(segment, 788);
    EXPECT_EQ(out.start, 4096U + 788U);
    EXPECT_EQ(out.samples, segment.samples);
    EXPECT_EQ(out.reason, lws::CloseReason::Ceiling);
    EXPECT_TRUE(out.continues_previous);
    EXPECT_TRUE(out.discontinuous_before);
}

TEST(StreamClock, OverrunNoticeCoversTheDroppedSamplesNotYetReported) {
    EXPECT_EQ(lws::overrun_notice(1000, 0, 300),
              (Notice{.kind = EventKind::Overrun, .begin = 1000, .end = 1300}));
    // A second gap: 300 were reported before, the total is 800 now.
    EXPECT_EQ(lws::overrun_notice(5000, 300, 800),
              (Notice{.kind = EventKind::Overrun, .begin = 5300, .end = 5800}));
}

TEST(StreamClock, VadFailureNoticeCoversTheFramesTheCallLost) {
    // Before the call: 1000 samples taken, 512 fed, 488 held. The call is given 2048 samples and
    // fails, so the reader keeps the same partial frame and the windower is fed nothing.
    constexpr StreamPosition before{.popped = 1000, .skew = 0, .pending = 488, .fed = 512};
    constexpr StreamPosition after{.popped = 3048, .skew = 0, .pending = 488, .fed = 512};
    const Notice notice = lws::vad_failure_notice(before, after);
    EXPECT_EQ(notice, (Notice{.kind = EventKind::VadFailure, .begin = 512, .end = 2560}));
    // The written-out form: the length is pending before plus the samples given, minus pending
    // after; the start is popped before plus skew minus pending before.
    constexpr std::uint64_t given = 2048;
    EXPECT_EQ(notice.begin, before.popped + before.skew - before.pending);
    EXPECT_EQ(notice.end - notice.begin, before.pending + given - after.pending);
}

TEST(StreamClock, VadFailureNoticeWhenThePartialFrameChanges) {
    // 100 samples held, 1000 given: the call fails, its two whole frames are dropped and the other
    // 76 samples are kept.
    constexpr StreamPosition before{.popped = 612, .skew = 0, .pending = 100, .fed = 512};
    constexpr StreamPosition after{.popped = 1612, .skew = 0, .pending = 76, .fed = 512};
    const Notice notice = lws::vad_failure_notice(before, after);
    EXPECT_EQ(notice.begin, 512U);
    EXPECT_EQ(notice.end - notice.begin, 100U + 1000U - 76U);
}
