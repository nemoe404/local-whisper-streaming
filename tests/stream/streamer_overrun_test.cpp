#include "stream/stream_fakes.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <future>
#include <iterator>
#include <numeric>
#include <ranges>
#include <span>
#include <thread>
#include <vector>

#include <fmt/core.h>

#include "lws/options.hpp"
#include "lws/streamer.hpp"
#include "lws/streamer_options.hpp"
#include "lws/text_event.hpp"

namespace {

using lws::test::EventLog;
using lws::test::IndexedVadLog;
using lws::test::StallGate;
using std::chrono::milliseconds;

constexpr milliseconds kBound{10000};
constexpr std::size_t kFrame = lws::kVadFrameSamples;
// The ring of the stalled cases: more than the stream up to the marker, so nothing is rejected
// before the stall however slowly the pump runs.
constexpr std::size_t kRingFrames = 128;
// The most the pump takes from the ring in one step.
constexpr std::size_t kPieceSamples = 4 * kFrame;

// The offered stream of a stalled case. The frame at `marker` holds the stall marker, so the pump
// stops inside the VAD when it scores it.
struct Layout {
    std::vector<float> values;
    std::size_t marker = 0;
};

// Utterance A, thirty frames of silence, the marker, silence up to the second utterance (thirty
// frames) and forty frames of silence. The rejected frames fall inside the silence after the
// marker.
Layout overrun_after_an_utterance() {
    Layout layout{
        .values = lws::test::values_of(
            {{0.0F, 20}, {0.9F, 30}, {0.0F, 30}, {0.0F, 1}, {0.0F, 150}, {0.9F, 30}, {0.0F, 40}}),
        .marker = 80};
    return layout;
}

// Speech from frame 20 on with the marker inside it and the rejected frames inside it too, so the
// window is still open at the gap; forty frames of silence at the end.
Layout overrun_inside_an_utterance() {
    Layout layout{.values = lws::test::values_of({{0.0F, 20}, {0.9F, 180}, {0.0F, 40}}),
                  .marker = 40};
    return layout;
}

// A stream whose pump is held inside the VAD. The test pushes one frame at a time: the ring fills
// exactly, the first push that is refused marks where the gap starts, and every push after it is
// refused too because the pump cannot make room. Nothing else is pushed until the pump is let go.
class StalledStream {
public:
    explicit StalledStream(const Layout& layout)
        : stream_{lws::test::indexed_stream(layout.values)}, marker_{layout.marker},
          streamer_{lws::test::make_test_streamer(log_, events_, lws::test::position_decoder(),
                                                  kRingFrames * kFrame, 8, &gate_)} {
        stream_[marker_ * kFrame] = lws::test::kStallMarker;
    }

    // Starts, pushes up to and including the marker, waits for the stall, fills the ring and
    // pushes until `rejected` frames in all have been refused, each as an overrun.
    [[nodiscard]] bool fill(std::size_t rejected) {
        if (!streamer_.start().has_value() || !push_frames(0, marker_ + 1) ||
            !gate_.wait_entered(kBound)) {
            return false;
        }
        const auto first = std::ranges::find_if(
            std::views::iota(marker_ + 1, frame_count()),
            [this](std::size_t index) { return !push_frame(index).has_value(); });
        if (first == std::views::iota(marker_ + 1, frame_count()).end()) {
            return false;
        }
        first_rejected_ = *first;
        dropped_frames_ = rejected;
        return std::ranges::all_of(
            std::views::iota(first_rejected_ + 1, first_rejected_ + rejected),
            [this](std::size_t index) {
                const auto result = push_frame(index);
                return !result.has_value() && result.error() == lws::Error::Overrun;
            });
    }

    [[nodiscard]] bool push_frames(std::size_t from, std::size_t to) {
        return std::ranges::all_of(std::views::iota(from, to), [this](std::size_t index) {
            return push_frame(index).has_value();
        });
    }

    [[nodiscard]] std::size_t frame_count() const { return stream_.size() / kFrame; }
    // The offered stream position where the first refused frame starts, and how long the refused
    // stretch is.
    [[nodiscard]] std::uint64_t gap_start() const { return first_rejected_ * kFrame; }
    [[nodiscard]] std::uint64_t gap_length() const { return dropped_frames_ * kFrame; }
    // The first frame the producer offers again.
    [[nodiscard]] std::size_t resume_frame() const { return first_rejected_ + dropped_frames_; }

    lws::Streamer& streamer() { return streamer_; }
    StallGate& gate() { return gate_; }
    EventLog& events() { return events_; }
    [[nodiscard]] const IndexedVadLog& log() const { return log_; }

private:
    [[nodiscard]] std::expected<void, lws::Error> push_frame(std::size_t index) {
        return streamer_.push(std::span<const float>{stream_}.subspan(index * kFrame, kFrame));
    }

    IndexedVadLog log_;
    EventLog events_;
    StallGate gate_;
    std::vector<float> stream_;
    std::size_t marker_;
    std::size_t first_rejected_ = 0;
    std::size_t dropped_frames_ = 0;
    lws::Streamer streamer_;
};

// A Text event sits where the frames it holds sit in the offered stream.
void expect_text_on_its_frames(const lws::TextEvent& event, bool after_gap) {
    EXPECT_EQ(event.kind, lws::EventKind::Text);
    EXPECT_EQ(event.after_gap, after_gap);
    const auto range = lws::test::frame_range_of(event.text);
    EXPECT_EQ(event.t_start, range.first * kFrame) << event.text;
    EXPECT_EQ(event.t_end, (range.last + 1) * kFrame) << event.text;
}

std::size_t count_kind(std::span<const lws::TextEvent> events, lws::EventKind kind) {
    return static_cast<std::size_t>(std::ranges::count(events, kind, &lws::TextEvent::kind));
}

// Releases the pump, waits for the report of the loss, pushes the rest of the stream, flushes and
// drains.
void finish_after_the_gap(StalledStream& run) {
    run.gate().release();
    // The text of the speech before the gap and the report of the loss.
    ASSERT_TRUE(run.events().wait_for_count(2, kBound));
    ASSERT_TRUE(run.push_frames(run.resume_frame(), run.frame_count()));
    ASSERT_TRUE(run.streamer().flush().has_value());
    ASSERT_TRUE(run.streamer().stop(lws::StopMode::Drain).has_value());
}

lws::TextEvent overrun_event(std::uint64_t begin, std::uint64_t end) {
    return lws::TextEvent{.kind = lws::EventKind::Overrun,
                          .t_start = begin,
                          .t_end = end,
                          .text = {},
                          .after_gap = false};
}

// What every gap run delivers: the text before the gap, the one report of the loss over the
// refused stretch, and the text after it, marked as following lost audio and made of frames from
// after the refused ones only. The stream clock and the one extra VAD reset are checked too.
void expect_text_gap_text(const StalledStream& run, std::span<const lws::TextEvent> events) {
    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(count_kind(events, lws::EventKind::Overrun), 1U);
    expect_text_on_its_frames(events[0], false);
    EXPECT_EQ(events[1], overrun_event(run.gap_start(), run.gap_start() + run.gap_length()));
    expect_text_on_its_frames(events[2], true);
    EXPECT_GE(events[2].t_start, run.gap_start() + run.gap_length());
    // The reset at the start of the session and the one for the overrun.
    EXPECT_EQ(run.log().resets, 2U);
}

void overrun_between_two_utterances() {
    StalledStream run{overrun_after_an_utterance()};
    ASSERT_TRUE(run.fill(8));
    EXPECT_EQ(run.gap_length(), 4096U);
    finish_after_the_gap(run);

    const auto events = run.events().events();
    expect_text_gap_text(run, events);
    EXPECT_LE(events.front().t_end, run.gap_start());
}

void overrun_inside_open_window() {
    StalledStream run{overrun_inside_an_utterance()};
    ASSERT_TRUE(run.fill(8));
    finish_after_the_gap(run);

    const auto events = run.events().events();
    expect_text_gap_text(run, events);
    // The window that was open is closed where the gap starts.
    EXPECT_EQ(events.front().t_end, run.gap_start());
}

// True once a flush is refused, which means the stop has taken effect. Until then a flush is
// accepted and changes nothing here, because no window is open at the stalled position.
bool wait_until_stopping(StalledStream& run) {
    const auto attempts = std::views::iota(0, 5000);
    return std::ranges::find_if(attempts, [&run](std::int32_t) {
               std::this_thread::sleep_for(milliseconds{1});
               return !run.streamer().flush().has_value();
           }) != attempts.end();
}

// Asks for Drain from another thread while the pump is held, lets the pump go once the stop has
// taken effect, and says whether the stop returned in time.
bool drain_while_held(StalledStream& run) {
    auto stopped = std::async(std::launch::async, [&run] {
        return run.streamer().stop(lws::StopMode::Drain).has_value();
    });
    if (!wait_until_stopping(run)) {
        run.gate().release();
        return false;
    }
    run.gate().release();
    return stopped.wait_for(kBound) == std::future_status::ready && stopped.get();
}

// One refused frame, then Drain asked for while the pump is still held, then the pump is let go.
void overrun_just_before_a_drain() {
    StalledStream run{overrun_after_an_utterance()};
    ASSERT_TRUE(run.fill(1));
    ASSERT_TRUE(drain_while_held(run));

    const auto events = run.events().events();
    EXPECT_EQ(count_kind(events, lws::EventKind::Overrun), 1U);
    const auto overrun = std::ranges::find(events, lws::EventKind::Overrun, &lws::TextEvent::kind);
    ASSERT_NE(overrun, events.end());
    EXPECT_EQ(*overrun, overrun_event(run.gap_start(), run.gap_start() + kFrame));
}

// A free-running producer against a slow detector: the first reported gap against the offered
// position where the first push was refused.
constexpr std::size_t kMeasuredFrames = 3000;
constexpr std::size_t kMeasuredRingFrames = 16;
constexpr milliseconds kDetectDelay{1};
constexpr std::chrono::microseconds kProducerPause{50};

// The offered position of every frame the ring refused, in order.
struct Producer {
    std::vector<std::uint64_t> refused;
};

// Offers the stream one frame at a time. A short sleep after each offer lets the pump run between
// offers: without it the producer finishes before the pump's first look at the ring and the only
// thing measured is one gap placed after everything the ring held.
Producer push_with_a_short_pause(lws::Streamer& streamer, std::span<const float> stream) {
    Producer producer;
    std::uint64_t offered = 0;
    std::ranges::for_each(
        stream | std::views::chunk(static_cast<std::ptrdiff_t>(kFrame)), [&](auto frame) {
            const auto result = streamer.push(std::span<const float>{frame.begin(), frame.end()});
            if (!result.has_value()) {
                producer.refused.push_back(offered);
            }
            offered += kFrame;
            std::this_thread::sleep_for(kProducerPause);
        });
    return producer;
}

// For each Overrun event, its start minus the offered position of the first frame it reports,
// reading the refused frames in order and giving each event as many as its length covers.
std::vector<std::int64_t> placement_errors(std::span<const lws::TextEvent> overruns,
                                           std::span<const std::uint64_t> refused) {
    const std::vector<std::size_t> lengths =
        overruns | std::views::transform([](const lws::TextEvent& event) {
            return static_cast<std::size_t>((event.t_end - event.t_start) / kFrame);
        }) |
        std::ranges::to<std::vector>();
    std::vector<std::size_t> before(lengths.size());
    std::exclusive_scan(lengths.begin(), lengths.end(), before.begin(), std::size_t{0});
    return std::views::iota(std::size_t{0}, overruns.size()) |
           std::views::transform([&](std::size_t index) {
               return static_cast<std::int64_t>(overruns[index].t_start) -
                      static_cast<std::int64_t>(refused[before[index]]);
           }) |
           std::ranges::to<std::vector>();
}

std::vector<float> measured_stream() {
    const std::vector<float> period = lws::test::values_of({{0.0F, 20}, {0.9F, 30}, {0.0F, 40}});
    std::vector<float> values;
    values.reserve(kMeasuredFrames);
    std::ranges::copy(std::views::repeat(period, 40) | std::views::join |
                          std::views::take(kMeasuredFrames),
                      std::back_inserter(values));
    return lws::test::indexed_stream(values);
}

// The refused frames and the Overrun events of one free-running run.
struct Measured {
    Producer producer;
    std::vector<lws::TextEvent> overruns;
};

Measured run_measured_stream() {
    const std::vector<float> stream = measured_stream();
    IndexedVadLog log;
    EventLog events;
    lws::StreamerOptions options;
    options.ring_capacity_samples = kMeasuredRingFrames * kFrame;
    lws::Streamer streamer =
        lws::StreamerAccess::assemble(lws::test::indexed_engine(log, nullptr, kDetectDelay),
                                      lws::test::position_decoder(), options)
            .value();
    streamer.on_text([&events](const lws::TextEvent& event) noexcept { events.record(event); })
        .value();
    EXPECT_TRUE(streamer.start().has_value());
    Measured measured{.producer = push_with_a_short_pause(streamer, stream), .overruns = {}};
    EXPECT_TRUE(streamer.flush().has_value());
    EXPECT_TRUE(streamer.stop(lws::StopMode::Drain).has_value());
    measured.overruns = events.events() | std::views::filter([](const lws::TextEvent& event) {
                            return event.kind == lws::EventKind::Overrun;
                        }) |
                        std::ranges::to<std::vector>();
    return measured;
}

void gap_placement_is_measured() {
    const Measured measured = run_measured_stream();
    const auto& [producer, overruns] = measured;
    ASSERT_FALSE(producer.refused.empty()) << "the producer never outran the pump";
    ASSERT_FALSE(overruns.empty());
    const std::uint64_t reported = std::ranges::fold_left(
        overruns, std::uint64_t{0}, [](std::uint64_t sum, const lws::TextEvent& event) {
            return sum + (event.t_end - event.t_start);
        });
    const std::uint64_t rejected_samples = producer.refused.size() * kFrame;
    ASSERT_EQ(reported, rejected_samples);

    const std::vector<std::int64_t> errors = placement_errors(overruns, producer.refused);
    const auto [lowest, highest] = std::ranges::minmax(errors);
    EXPECT_GT(lowest, -static_cast<std::int64_t>(kMeasuredRingFrames * kFrame));
    EXPECT_LT(highest, static_cast<std::int64_t>(kFrame + kPieceSamples));
    fmt::println("GAP-PLACEMENT c={} push={} piece={} rejected={} events={}", errors.front(),
                 kFrame, kPieceSamples, rejected_samples, overruns.size());
    fmt::println("GAP-SPREAD min={} max={}", lowest, highest);
}

} // namespace

TEST(StreamerOverrun, OverrunIsReportedOnceAtItsPlace) {
    lws::test::run_bounded("overrun-between-utterances", milliseconds{30000},
                           overrun_between_two_utterances);
}

TEST(StreamerOverrun, OverrunInsideAnUtteranceClosesItAtTheGap) {
    lws::test::run_bounded("overrun-inside-utterance", milliseconds{30000},
                           overrun_inside_open_window);
}

TEST(StreamerOverrun, GapPlacementErrorIsMeasured) {
    lws::test::run_bounded("gap-placement", milliseconds{60000}, gap_placement_is_measured);
}

TEST(StreamerOverrun, OverrunJustBeforeDrainIsStillReported) {
    lws::test::run_bounded("overrun-before-drain", milliseconds{30000},
                           overrun_just_before_a_drain);
}
