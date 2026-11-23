// Reset count over the whole Segmenter with fake engines: one at session start, one after a ring
// overrun, one on relink, none on a hangover close or a ceiling seam. The ring is a real SpscRing
// used for its discontinuity flag; the test is single-threaded. No ggml compute.
#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "fake_vad.hpp"
#include "lws/options.hpp"
#include "lws/segment.hpp"
#include "lws/spsc_ring.hpp"
#include "segmenter_support.hpp"
#include "shell/segmenter.hpp"

namespace {

using lws::test::FakeCall;
using lws::test::FakeVadLog;
using lws::test::make_segmenter;
using lws::test::push_chunked;
using lws::test::resets;
using lws::test::segment_of;
using lws::test::stream_of;

constexpr float kSpeech = 0.9F;
constexpr float kQuiet = 0.1F;
constexpr float kRejected = 0.75F;
constexpr std::size_t kFrame = lws::kVadFrameSamples;
constexpr std::size_t kRingFrames = 8;

// A ceiling of 2 s is 63 frames: more than the 10 pre-roll plus 8 onset frames, so valid.
lws::Options short_ceiling() { return lws::Options{.max_speech_duration_s = 2.0F}; }

// One utterance: 20 speech frames, then 30 sub-threshold frames (the hangover is 22).
std::vector<float> utterance() { return stream_of({{kSpeech, 20}, {kQuiet, 30}}); }

// What the Phase-6 pump will do for one wake-up: pop everything available, push it to the
// segmenter, then act on the ring's discontinuity flag.
struct Drained {
    std::vector<lws::Segment> segments;
    std::optional<lws::Segment> overrun_segment;
    bool flag = false;
};

Drained drain(lws::SpscRing& ring, lws::Segmenter& segmenter) {
    std::vector<float> popped(ring.available());
    const auto count = ring.pop(popped);
    popped.resize(count);
    Drained out;
    out.segments = segmenter.push(popped).value();
    out.flag = ring.take_discontinuity();
    if (out.flag) {
        out.overrun_segment = segmenter.notify_overrun();
    }
    return out;
}

bool offer(lws::SpscRing& ring, std::span<const float> samples) {
    return ring.push(samples).has_value();
}

struct OverrunRun {
    std::size_t segments_before_overrun = 0;
    bool flag_before_overrun = false;
    bool overrun_rejected = false;
    bool flag_after_overrun = false;
    std::optional<lws::Segment> closed;
    std::size_t resets_after_overrun = 0;
    lws::test::Pushed later;
    std::size_t resets_final = 0;
};

OverrunRun run_overrun() {
    FakeVadLog log;
    auto segmenter = make_segmenter(log);
    lws::SpscRing ring{kRingFrames * kFrame};
    OverrunRun run;

    // Two full rounds: the first completes the 8-frame onset, the second grows the window to 16.
    const auto round = stream_of({{kSpeech, kRingFrames}});
    std::ranges::for_each(std::array{0, 1}, [&](int) {
        const bool accepted = offer(ring, round);
        const auto drained = drain(ring, segmenter);
        run.segments_before_overrun += drained.segments.size();
        run.flag_before_overrun = run.flag_before_overrun || drained.flag || !accepted;
    });

    // 2048 samples fit; the next 3000 do not (2048 + 3000 > 4096), so none of them is stored.
    const auto fits = stream_of({{kSpeech, 4}});
    const auto rejected_stream = stream_of({{kRejected, 6}});
    const bool fits_accepted = offer(ring, fits);
    run.overrun_rejected =
        fits_accepted && !offer(ring, std::span<const float>{rejected_stream}.first(3000));

    // The consumer pops the 2048 accepted samples; the window then holds 20 frames.
    std::vector<float> popped(ring.available());
    popped.resize(ring.pop(popped));
    run.segments_before_overrun += segmenter.push(popped).value().size();
    run.flag_after_overrun = ring.take_discontinuity();
    if (run.flag_after_overrun) {
        run.closed = segmenter.notify_overrun();
    }
    run.resets_after_overrun = resets(log);

    run.later = push_chunked(segmenter, utterance());
    run.resets_final = resets(log);
    return run;
}

TEST(SegmenterReset, OneResetAtSessionStart) {
    FakeVadLog log;
    auto segmenter = make_segmenter(log);

    EXPECT_EQ(resets(log), 1U);
    const std::vector<FakeCall> expected{
        {FakeCall::Kind::Detect, kFrame},
        {FakeCall::Kind::Detect, kFrame + 1},
        {FakeCall::Kind::Reset, 0},
    };
    EXPECT_EQ(log.calls, expected);
}

TEST(SegmenterReset, NoResetOnHangoverClose) {
    FakeVadLog log;
    auto segmenter = make_segmenter(log);
    const auto one = utterance();
    std::vector<float> three = one;
    three.insert(three.end(), one.begin(), one.end());
    three.insert(three.end(), one.begin(), one.end());

    const auto pushed = push_chunked(segmenter, three);

    EXPECT_FALSE(pushed.failure.has_value());
    ASSERT_EQ(pushed.segments.size(), 3U);
    EXPECT_TRUE(std::ranges::all_of(pushed.segments, [](const lws::Segment& segment) {
        return segment.reason == lws::CloseReason::Hangover;
    }));
    EXPECT_EQ(resets(log), 1U);
}

TEST(SegmenterReset, NoResetOnCeilingSeam) {
    FakeVadLog log;
    auto segmenter = make_segmenter(log, short_ceiling());

    const auto pushed = push_chunked(segmenter, stream_of({{kSpeech, 200}}));

    EXPECT_FALSE(pushed.failure.has_value());
    const auto ceilings =
        std::ranges::count(pushed.segments, lws::CloseReason::Ceiling, &lws::Segment::reason);
    EXPECT_GE(ceilings, 2);
    EXPECT_TRUE(std::ranges::any_of(pushed.segments, &lws::Segment::continues_previous));
    EXPECT_EQ(resets(log), 1U);
}

TEST(SegmenterReset, OneResetAfterARingOverrun) {
    const auto run = run_overrun();

    EXPECT_EQ(run.segments_before_overrun, 0U);
    EXPECT_FALSE(run.flag_before_overrun);
    EXPECT_TRUE(run.overrun_rejected);
    EXPECT_TRUE(run.flag_after_overrun);
    EXPECT_EQ(run.resets_after_overrun, 2U);

    const auto closed = segment_of(run.closed);
    EXPECT_EQ(closed.reason, lws::CloseReason::Discontinuity);
    EXPECT_EQ(closed.start, 0U);
    EXPECT_EQ(closed.samples.size(), 20 * kFrame);
    EXPECT_FALSE(std::ranges::contains(closed.samples, kRejected));

    EXPECT_FALSE(run.later.failure.has_value());
    ASSERT_EQ(run.later.segments.size(), 1U);
    EXPECT_EQ(run.later.segments.front().reason, lws::CloseReason::Hangover);
    EXPECT_TRUE(run.later.segments.front().discontinuous_before);
    EXPECT_EQ(run.resets_final, 2U);
}

TEST(SegmenterReset, OneResetOnRelink) {
    FakeVadLog log;
    auto segmenter = make_segmenter(log);
    const auto speech = push_chunked(segmenter, stream_of({{kSpeech, 20}}));
    ASSERT_TRUE(speech.segments.empty());

    const auto closed = segment_of(segmenter.notify_relink());

    EXPECT_EQ(closed.reason, lws::CloseReason::Discontinuity);
    EXPECT_EQ(resets(log), 2U);
    EXPECT_EQ(log.calls.back().kind, FakeCall::Kind::Reset);
}

// The reset count after each event of one script: start, two hangover closes, a ceiling seam, a
// ring overrun, a relink, a flush.
struct ScriptRun {
    std::vector<std::size_t> counts;
    std::vector<FakeCall::Kind> last_call_after_event;
    std::optional<lws::Segment> flushed;
};

ScriptRun run_script() {
    FakeVadLog log;
    auto segmenter = make_segmenter(log, short_ceiling());
    ScriptRun run;
    const auto mark = [&] {
        run.counts.push_back(resets(log));
        run.last_call_after_event.push_back(log.calls.back().kind);
    };
    mark(); // session start
    static_cast<void>(push_chunked(segmenter, utterance()));
    mark(); // first hangover close
    static_cast<void>(push_chunked(segmenter, utterance()));
    mark(); // second hangover close
    static_cast<void>(push_chunked(segmenter, stream_of({{kSpeech, 100}})));
    mark(); // ceiling seam, window still open

    lws::SpscRing ring{kRingFrames * kFrame};
    static_cast<void>(offer(ring, stream_of({{kSpeech, 4}})));
    static_cast<void>(offer(ring, stream_of({{kSpeech, 10}})));
    static_cast<void>(drain(ring, segmenter));
    mark(); // ring overrun

    static_cast<void>(segmenter.notify_relink());
    mark(); // relink

    static_cast<void>(push_chunked(segmenter, stream_of({{kSpeech, 20}})));
    run.flushed = segmenter.flush();
    mark(); // end of stream
    return run;
}

TEST(SegmenterReset, WholeScriptGivesExactlyThreeResets) {
    const auto run = run_script();

    const std::vector<std::size_t> expected_counts{1, 1, 1, 1, 2, 3, 3};
    EXPECT_EQ(run.counts, expected_counts);
    // A reset is the last thing the segmenter did at the overrun and at the relink.
    EXPECT_EQ(run.last_call_after_event.at(4), FakeCall::Kind::Reset);
    EXPECT_EQ(run.last_call_after_event.at(5), FakeCall::Kind::Reset);
    EXPECT_EQ(segment_of(run.flushed).reason, lws::CloseReason::Flush);
}

} // namespace
