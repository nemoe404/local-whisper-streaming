#include "stream/stream_fakes.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <ranges>
#include <span>
#include <vector>

#include "lws/options.hpp"
#include "lws/segment.hpp"
#include "lws/streamer.hpp"
#include "lws/streamer_options.hpp"
#include "lws/text_event.hpp"

namespace {

using lws::EventKind;
using lws::TextEvent;
using lws::test::DecodeGate;
using lws::test::EventLog;
using lws::test::IndexedVadLog;
using lws::test::StallGate;
using std::chrono::milliseconds;

constexpr milliseconds kBound{10000};
constexpr milliseconds kRunBound{60000};
constexpr std::size_t kFrame = lws::kVadFrameSamples;

TextEvent notice_of(EventKind kind, std::uint64_t begin, std::uint64_t end) {
    return TextEvent{.kind = kind, .t_start = begin, .t_end = end, .text = {}, .after_gap = false};
}

// What a segment that was dropped from the queue is reported as.
TextEvent dropped_event(const lws::Segment& segment) {
    return notice_of(EventKind::SegmentDropped, segment.start,
                     segment.start + segment.samples.size());
}

// What a failed detect call is reported as: the frames it was given, from the first index its log
// entry carries.
TextEvent lost_by(const IndexedVadLog::Call& call) {
    return notice_of(EventKind::VadFailure, call.first_index * kFrame,
                     (call.first_index + call.frames) * kFrame);
}

// A Text event sits where the frames it holds sit in the offered stream.
void expect_text_on_its_frames(const TextEvent& event, bool after_gap) {
    EXPECT_EQ(event.kind, EventKind::Text);
    EXPECT_EQ(event.after_gap, after_gap);
    const auto range = lws::test::frame_range_of(event.text);
    EXPECT_EQ(event.t_start, range.first * kFrame) << event.text;
    EXPECT_EQ(event.t_end, (range.last + 1) * kFrame) << event.text;
}

std::size_t count_kind(std::span<const TextEvent> events, EventKind kind) {
    return static_cast<std::size_t>(std::ranges::count(events, kind, &TextEvent::kind));
}

// Offers the frames [from, to) of the stream one frame at a time. False when any was refused.
bool push_frames(lws::Streamer& streamer, std::span<const float> stream, std::size_t from,
                 std::size_t to) {
    return std::ranges::all_of(std::views::iota(from, to), [&](std::size_t index) {
        return streamer.push(stream.subspan(index * kFrame, kFrame)).has_value();
    });
}

// Offers the frames [from, to) and says whether every one was refused as an overrun.
bool push_refused(lws::Streamer& streamer, std::span<const float> stream, std::size_t from,
                  std::size_t to) {
    return std::ranges::all_of(std::views::iota(from, to), [&](std::size_t index) {
        const auto result = streamer.push(stream.subspan(index * kFrame, kFrame));
        return !result.has_value() && result.error() == lws::Error::Overrun;
    });
}

bool finish(lws::Streamer& streamer) {
    return streamer.flush().has_value() && streamer.stop(lws::StopMode::Drain).has_value();
}

// What a run produced. `completed` is false when a step of the choreography did not happen.
struct Outcome {
    bool completed = false;
    std::vector<TextEvent> events;
    std::vector<IndexedVadLog::Call> lost; // the detect calls the engine failed
};

// The wait and release steps of a choreographed run are chained with &&, so each one gives true.
bool released(DecodeGate& gate) {
    gate.release();
    return true;
}

bool released(StallGate& stall) {
    stall.release();
    return true;
}

Outcome outcome_of(bool completed, const EventLog& events, const IndexedVadLog& log) {
    return Outcome{
        .completed = completed, .events = events.events(), .lost = lws::test::failed_calls(log)};
}

// A detect call fails in the middle of an utterance.
constexpr std::size_t kFailedFrame = 45;

Outcome run_vad_failure(std::size_t piece) {
    auto stream = lws::test::indexed_stream(
        lws::test::values_of({{0.0F, 20}, {0.9F, 40}, {0.9F, 30}, {0.0F, 40}}));
    stream[kFailedFrame * kFrame] = lws::test::kFailMarker;
    IndexedVadLog log;
    EventLog events;
    auto streamer =
        lws::test::make_test_streamer(log, events, lws::test::position_decoder(), 256 * kFrame);
    const bool completed = streamer.start().has_value() &&
                           lws::test::push_pieces(streamer, stream, piece) && finish(streamer);
    return outcome_of(completed, events, log);
}

// The speech collected so far is delivered, the lost frames are reported once, and the speech
// after them is delivered on its own frames.
void expect_failure_events(std::span<const TextEvent> got, const IndexedVadLog::Call& lost) {
    expect_text_on_its_frames(got[0], false);
    EXPECT_EQ(got[1], lost_by(lost));
    expect_text_on_its_frames(got[2], false);
    // The window that was open ends exactly where the lost range begins.
    EXPECT_EQ(got[0].t_end, got[1].t_start);
    EXPECT_GE(got[2].t_start, got[1].t_end);
}

// The lost range holds the frame that failed.
void expect_range_holds_the_failed_frame(const TextEvent& lost) {
    EXPECT_LE(lost.t_start, kFailedFrame * kFrame);
    EXPECT_GT(lost.t_end, kFailedFrame * kFrame);
}

void vad_failure_reports_the_lost_frames(std::size_t piece) {
    const Outcome outcome = run_vad_failure(piece);
    ASSERT_TRUE(outcome.completed);
    ASSERT_EQ(outcome.lost.size(), 1U);
    ASSERT_EQ(outcome.events.size(), 3U);
    expect_failure_events(outcome.events, outcome.lost.front());
    expect_range_holds_the_failed_frame(outcome.events[1]);
}

// Six utterances, a marker well after the last one closed, and the frames that follow.
constexpr std::size_t kDropUtterances = 6;
constexpr std::size_t kDropFirstPart = 40 + 30 + 40;

struct DropStream {
    std::vector<float> samples;
    std::size_t marker = 0;
};

DropStream dropping_stream() {
    const std::vector<float> period = lws::test::values_of({{0.9F, 30}, {0.0F, 40}});
    std::vector<float> values(40, 0.0F);
    std::ranges::copy(std::views::repeat(period, kDropUtterances) | std::views::join,
                      std::back_inserter(values));
    values.insert(values.end(), 10, 0.0F);
    const std::size_t marker = values.size();
    values.insert(values.end(), 21, 0.0F);
    return DropStream{.samples = lws::test::indexed_stream(values), .marker = marker};
}

Outcome run_drops(std::vector<float>& stream, std::size_t marker) {
    const std::size_t frames = stream.size() / kFrame;
    stream[marker * kFrame] = lws::test::kStallMarker;
    IndexedVadLog log;
    EventLog events;
    DecodeGate gate;
    StallGate stall;
    auto streamer = lws::test::make_test_streamer(
        log, events, lws::test::gated_position_decoder(gate), 1024 * kFrame, 2, &stall);
    // Segment 1 is in the decoder before anything else is offered; the stall frame sits well after
    // the last segment closed, so every segment is queued when the stall is reached.
    const bool completed =
        streamer.start().has_value() && push_frames(streamer, stream, 0, kDropFirstPart) &&
        gate.wait_entered(kBound) && push_frames(streamer, stream, kDropFirstPart, frames) &&
        stall.wait_entered(kBound) && released(gate) && released(stall) && finish(streamer);
    gate.release();
    stall.release();
    return outcome_of(completed, events, log);
}

void slow_decoder_drops_the_oldest_whole_segments() {
    DropStream drop = dropping_stream();
    const auto segments = lws::test::direct_segments(drop.samples, kFrame);
    ASSERT_EQ(segments.size(), kDropUtterances);
    const Outcome outcome = run_drops(drop.samples, drop.marker);
    ASSERT_TRUE(outcome.completed);

    const std::vector<TextEvent> expected{lws::test::expected_text_event(segments[0]),
                                          dropped_event(segments[1]),
                                          dropped_event(segments[2]),
                                          dropped_event(segments[3]),
                                          lws::test::expected_text_event(segments[4]),
                                          lws::test::expected_text_event(segments[5])};
    EXPECT_EQ(outcome.events, expected);
}

// The stream of the combined run, by frame index: the first utterance and its silence, a second
// utterance whose silence holds a failing frame, a third one followed by a stall frame, the frames
// that fill the ring, the frames that are refused, and a fourth utterance.
class LossLayout {
public:
    static constexpr std::size_t kHead = 110;
    static constexpr std::size_t kFailedFrame = 179;
    static constexpr std::size_t kStallFrame = 262;
    static constexpr std::size_t kRingFrames = 320;
    static constexpr std::size_t kRefusedFrames = 8;
    static constexpr std::size_t kFirstRefused = kStallFrame + 1 + kRingFrames;
    static constexpr std::size_t kResume = kFirstRefused + kRefusedFrames;

    LossLayout()
        : clean_{lws::test::indexed_stream(
              lws::test::values_of({{0.0F, 40},
                                    {0.9F, 30},
                                    {0.0F, 40},
                                    {0.9F, 30},
                                    {0.0F, 60},
                                    {0.9F, 30},
                                    {0.0F, 33},
                                    {0.0F, kRingFrames + kRefusedFrames},
                                    {0.9F, 30},
                                    {0.0F, 40}}))},
          marked_{clean_} {
        marked_[kFailedFrame * kFrame] = lws::test::kFailMarker;
        marked_[kStallFrame * kFrame] = lws::test::kStallMarker;
    }

    // The stream the streamer is offered, with the failing frame and the stall frame in it.
    [[nodiscard]] std::span<const float> offered() const { return marked_; }
    [[nodiscard]] std::size_t frame_count() const { return clean_.size() / kFrame; }
    // The stream up to and including the stall frame with no marker in it: what a direct
    // Segmenter cuts there is what the three first utterances are.
    [[nodiscard]] std::span<const float> before_the_stall() const {
        return std::span<const float>{clean_}.first((kStallFrame + 1) * kFrame);
    }

private:
    std::vector<float> clean_;
    std::vector<float> marked_;
};

// Offers the stream in the order the choreography needs: the pump is held inside the detect call
// that holds the stall frame, so everything before it is queued; the first utterance is being
// decoded, the second was dropped for the third. Then the ring is filled and one span refused.
Outcome run_losses(const LossLayout& layout) {
    const std::span<const float> stream = layout.offered();
    IndexedVadLog log;
    EventLog events;
    DecodeGate gate;
    StallGate stall;
    auto streamer =
        lws::test::make_test_streamer(log, events, lws::test::gated_position_decoder(gate),
                                      LossLayout::kRingFrames * kFrame, 1, &stall);
    const bool completed =
        streamer.start().has_value() && push_frames(streamer, stream, 0, LossLayout::kHead) &&
        gate.wait_entered(kBound) &&
        push_frames(streamer, stream, LossLayout::kHead, LossLayout::kStallFrame + 1) &&
        stall.wait_entered(kBound) && released(gate) && events.wait_for_count(4, kBound) &&
        push_frames(streamer, stream, LossLayout::kStallFrame + 1, LossLayout::kFirstRefused) &&
        push_refused(streamer, stream, LossLayout::kFirstRefused, LossLayout::kResume) &&
        released(stall) && events.wait_for_count(5, kBound) &&
        push_frames(streamer, stream, LossLayout::kResume, layout.frame_count()) &&
        finish(streamer);
    gate.release();
    stall.release();
    return outcome_of(completed, events, log);
}

void expect_one_of_each_loss(std::span<const TextEvent> got) {
    EXPECT_EQ(count_kind(got, EventKind::VadFailure), 1U);
    EXPECT_EQ(count_kind(got, EventKind::Overrun), 1U);
    EXPECT_EQ(count_kind(got, EventKind::SegmentDropped), 1U);
    EXPECT_EQ(count_kind(got, EventKind::DecodeFailure), 0U);
}

void expect_each_loss_once(const LossLayout& layout, const Outcome& outcome) {
    const auto segments = lws::test::direct_segments(layout.before_the_stall(), kFrame);
    ASSERT_EQ(segments.size(), 3U);
    ASSERT_EQ(outcome.lost.size(), 1U);
    const std::uint64_t gap = LossLayout::kFirstRefused * kFrame;
    const std::vector<TextEvent> before_the_gap{
        lws::test::expected_text_event(segments[0]), dropped_event(segments[1]),
        lost_by(outcome.lost.front()), lws::test::expected_text_event(segments[2]),
        notice_of(EventKind::Overrun, gap, gap + (LossLayout::kRefusedFrames * kFrame))};
    ASSERT_EQ(outcome.events.size(), before_the_gap.size() + 1);
    EXPECT_TRUE(
        std::ranges::equal(std::span{outcome.events}.first(before_the_gap.size()), before_the_gap));
    expect_text_on_its_frames(outcome.events.back(), true);
    EXPECT_GE(outcome.events.back().t_start, LossLayout::kResume * kFrame);
    expect_one_of_each_loss(outcome.events);
}

void each_loss_is_reported_exactly_once() {
    const LossLayout layout;
    const Outcome outcome = run_losses(layout);
    ASSERT_TRUE(outcome.completed);
    expect_each_loss_once(layout, outcome);
}

} // namespace

TEST(StreamerNotices, VadFailureReportsTheLostFramesOnce) {
    constexpr std::array<std::size_t, 3> kPieces{160, 512, 4096};
    std::ranges::for_each(kPieces, [](std::size_t piece) {
        SCOPED_TRACE(piece);
        lws::test::run_bounded("vad-failure", kRunBound,
                               [piece] { vad_failure_reports_the_lost_frames(piece); });
    });
}

TEST(StreamerNotices, SlowDecoderDropsTheOldestWholeSegments) {
    lws::test::run_bounded("slow-decoder", kRunBound, slow_decoder_drops_the_oldest_whole_segments);
}

TEST(StreamerNotices, EachLossIsReportedExactlyOnce) {
    lws::test::run_bounded("each-loss-once", kRunBound, each_loss_is_reported_exactly_once);
}
