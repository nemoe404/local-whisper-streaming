#include "stream/stream_fakes.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <vector>

#include "lws/streamer.hpp"
#include "lws/text_event.hpp"
#include "vad/fake_vad.hpp"

namespace {

using lws::test::EventLog;
using lws::test::IndexedVadLog;
using std::chrono::milliseconds;

constexpr milliseconds kBound{10000};
constexpr std::size_t kFrame = lws::kVadFrameSamples;

// Two utterances with a long silence between them. A detect call that fails inside that silence
// loses the frames of its piece, so the second utterance sits later in the offered stream than the
// frames the windower saw before it.
std::vector<float> stream_with_a_lost_stretch() {
    return lws::test::indexed_stream(lws::test::values_of({{0.0F, 20},
                                                           {0.9F, 30},
                                                           {0.0F, 40},
                                                           {lws::test::kFailMarker, 1},
                                                           {0.0F, 39},
                                                           {0.9F, 30},
                                                           {0.0F, 40}}));
}

struct Run {
    std::vector<lws::TextEvent> texts;
    bool a_call_failed = false;
};

// The whole stream through a streamer with fake engines, then flush and a Drain stop.
Run run_stream(std::span<const float> stream, std::size_t piece) {
    IndexedVadLog log;
    EventLog events;
    lws::Streamer streamer =
        lws::test::make_test_streamer(log, events, lws::test::position_decoder(), stream.size());
    EXPECT_TRUE(streamer.start().has_value());
    EXPECT_TRUE(lws::test::push_pieces(streamer, stream, piece));
    EXPECT_TRUE(streamer.flush().has_value());
    EXPECT_TRUE(streamer.stop(lws::StopMode::Drain).has_value());
    Run run;
    run.texts = events.events() | std::views::filter([](const lws::TextEvent& event) {
                    return event.kind == lws::EventKind::Text;
                }) |
                std::ranges::to<std::vector>();
    run.a_call_failed = std::ranges::any_of(log.calls, &IndexedVadLog::Call::failed);
    return run;
}

// Each Text event's range equals the offered-stream range of the first and last frame it holds.
void expect_times_match_frames(std::span<const lws::TextEvent> texts) {
    std::ranges::for_each(texts, [](const lws::TextEvent& event) {
        const auto range = lws::test::frame_range_of(event.text);
        EXPECT_EQ(event.t_start, range.first * kFrame) << event.text;
        EXPECT_EQ(event.t_end, (range.last + 1) * kFrame) << event.text;
    });
}

void times_hold_after_lost_frames() {
    const std::vector<float> stream = stream_with_a_lost_stretch();
    std::vector<std::vector<lws::TextEvent>> by_size;
    std::ranges::for_each(std::array<std::size_t, 3>{160, 512, 4096}, [&](std::size_t piece) {
        const Run run = run_stream(stream, piece);
        EXPECT_TRUE(run.a_call_failed) << "piece " << piece;
        ASSERT_EQ(run.texts.size(), 2U) << "piece " << piece;
        expect_times_match_frames(run.texts);
        by_size.push_back(run.texts);
    });
    ASSERT_EQ(by_size.size(), 3U);
    EXPECT_EQ(by_size[0], by_size[1]);
    EXPECT_EQ(by_size[1], by_size[2]);
}

} // namespace

TEST(StreamerClock, TextTimesStayOnTheOfferedStreamAfterLostVadFrames) {
    lws::test::run_bounded("clock-lost-frames", kBound, times_hold_after_lost_frames);
}
