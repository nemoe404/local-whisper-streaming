#include "stream/stream_fakes.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "lws/options.hpp"
#include "lws/seam_merge.hpp"
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

// Twenty frames of silence, one long utterance that the 14 s ceiling cuts into three segments, and
// forty frames of silence. The frame at kMarker is in the silence, more than eight frames after the
// last segment closes.
constexpr std::size_t kMarker = 1050;
// Frames offered before the decoder is waited for: past the first cut, before the second.
constexpr std::size_t kFirstPart = 500;

std::vector<float> long_utterance() {
    return lws::test::indexed_stream(
        lws::test::values_of({{0.0F, 20}, {0.9F, 1000}, {0.0F, 30}, {0.0F, 1}, {0.0F, 9}}));
}

struct Outcome {
    bool completed = false;
    std::vector<TextEvent> events;
};

bool released(DecodeGate& gate) {
    gate.release();
    return true;
}

bool released(StallGate& stall) {
    stall.release();
    return true;
}

// Offers the stream to a streamer that has no gate and no stall, and drains it.
Outcome run_free(lws::Decoder decoder, const lws::Options& vad) {
    const auto stream = long_utterance();
    IndexedVadLog log;
    EventLog events;
    auto streamer = lws::test::make_test_streamer(log, events, std::move(decoder), 2048 * kFrame, 8,
                                                  nullptr, vad);
    const bool completed =
        streamer.start().has_value() && lws::test::push_pieces(streamer, stream, 4 * kFrame) &&
        streamer.flush().has_value() && streamer.stop(lws::StopMode::Drain).has_value();
    return Outcome{.completed = completed, .events = events.events()};
}

// The decoder is held on the first segment while the second and third are queued behind it in a
// queue that holds one, so the second is dropped.
Outcome run_with_a_drop() {
    auto stream = long_utterance();
    stream[kMarker * kFrame] = lws::test::kStallMarker;
    IndexedVadLog log;
    EventLog events;
    DecodeGate gate;
    StallGate stall;
    auto streamer = lws::test::make_test_streamer(log, events, lws::test::seam_decoder(&gate),
                                                  2048 * kFrame, 1, &stall);
    const std::span<const float> offered{stream};
    const bool completed =
        streamer.start().has_value() &&
        lws::test::push_pieces(streamer, offered.first(kFirstPart * kFrame), kFrame) &&
        gate.wait_entered(kBound) &&
        lws::test::push_pieces(streamer, offered.subspan(kFirstPart * kFrame), kFrame) &&
        stall.wait_entered(kBound) && released(gate) && released(stall) &&
        streamer.flush().has_value() && streamer.stop(lws::StopMode::Drain).has_value();
    gate.release();
    stall.release();
    return Outcome{.completed = completed, .events = events.events()};
}

std::vector<lws::Segment> oracle(const lws::Options& vad = lws::Options{}) {
    return lws::test::direct_segments(long_utterance(), kFrame, vad);
}

// What a segment gives when its text is not merged.
TextEvent raw_event(const lws::Segment& segment) {
    TextEvent event = lws::test::expected_text_event(segment);
    const auto range = lws::test::frame_range_of(event.text);
    event.text = lws::test::seam_text(range.first, range.last);
    return event;
}

TextEvent failure_event(const lws::Segment& segment) {
    return TextEvent{.kind = EventKind::DecodeFailure,
                     .t_start = segment.start,
                     .t_end = segment.start + segment.samples.size(),
                     .text = {},
                     .after_gap = false};
}

TextEvent dropped_event(const lws::Segment& segment) {
    TextEvent event = failure_event(segment);
    event.kind = EventKind::SegmentDropped;
    return event;
}

// The events of an uninterrupted chain: each continuation of a ceiling cut with the words it
// shares with the raw text of its predecessor removed.
std::vector<TextEvent> merged_chain(std::span<const lws::Segment> segments) {
    std::vector<TextEvent> events =
        segments | std::views::transform(raw_event) | std::ranges::to<std::vector>();
    std::ranges::for_each(std::views::iota(std::size_t{1}, segments.size()), [&](std::size_t i) {
        events[i].text =
            lws::seam_merge(raw_event(segments[i - 1]).text, raw_event(segments[i]).text);
    });
    return events;
}

void expect_each_continuation_lost_its_first_word(std::span<const TextEvent> got,
                                                  std::span<const lws::Segment> segments) {
    std::ranges::for_each(std::views::iota(std::size_t{1}, got.size()), [&](std::size_t i) {
        const std::string raw = raw_event(segments[i]).text;
        EXPECT_NE(got[i].text, raw);
        EXPECT_EQ("seam " + got[i].text, raw);
    });
}

void continuation_is_merged_with_its_predecessor() {
    const auto segments = oracle();
    ASSERT_EQ(segments.size(), 3U);
    const Outcome outcome = run_free(lws::test::seam_decoder(), lws::Options{});
    ASSERT_TRUE(outcome.completed);
    EXPECT_EQ(outcome.events, merged_chain(segments));
    ASSERT_EQ(outcome.events.size(), 3U);
    expect_each_continuation_lost_its_first_word(outcome.events, segments);
    EXPECT_EQ(outcome.events.front(), raw_event(segments.front()));
}

void a_report_between_two_segments_stops_the_merge() {
    const auto segments = oracle();
    ASSERT_EQ(segments.size(), 3U);
    const Outcome outcome = run_with_a_drop();
    ASSERT_TRUE(outcome.completed);
    const std::vector<TextEvent> expected{raw_event(segments[0]), dropped_event(segments[1]),
                                          raw_event(segments[2])};
    EXPECT_EQ(outcome.events, expected);
    // Merged against segment 2's text it would have lost its first word.
    EXPECT_NE(lws::seam_merge(raw_event(segments[1]).text, raw_event(segments[2]).text),
              raw_event(segments[2]).text);
}

void a_failed_decode_is_one_event_and_stops_the_merge() {
    const auto segments = oracle();
    ASSERT_EQ(segments.size(), 3U);
    const Outcome outcome = run_free(lws::test::seam_decoder(nullptr, 2), lws::Options{});
    ASSERT_TRUE(outcome.completed);
    const std::vector<TextEvent> expected{raw_event(segments[0]), failure_event(segments[1]),
                                          raw_event(segments[2])};
    EXPECT_EQ(outcome.events, expected);
}

void no_overlap_means_no_merge() {
    lws::Options vad;
    vad.overlap_ms = 0;
    const auto segments = oracle(vad);
    ASSERT_GE(segments.size(), 3U);
    const Outcome outcome = run_free(lws::test::seam_decoder(), vad);
    ASSERT_TRUE(outcome.completed);
    const std::vector<TextEvent> expected =
        segments | std::views::transform(raw_event) | std::ranges::to<std::vector>();
    EXPECT_EQ(outcome.events, expected);
}

} // namespace

TEST(StreamerSeam, ContinuationIsMergedWithItsPredecessor) {
    lws::test::run_bounded("seam-chain", kRunBound, continuation_is_merged_with_its_predecessor);
}

TEST(StreamerSeam, AReportBetweenTwoSegmentsStopsTheMerge) {
    lws::test::run_bounded("seam-report", kRunBound, a_report_between_two_segments_stops_the_merge);
}

TEST(StreamerSeam, AFailedDecodeIsOneEventAndStopsTheMerge) {
    lws::test::run_bounded("seam-failed-decode", kRunBound,
                           a_failed_decode_is_one_event_and_stops_the_merge);
}

TEST(StreamerSeam, NoOverlapMeansNoMerge) {
    lws::test::run_bounded("seam-no-overlap", kRunBound, no_overlap_means_no_merge);
}
