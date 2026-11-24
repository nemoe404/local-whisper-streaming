// The recorded clip through Streamer::create with the real Silero and whisper models: one Text
// event per utterance in stream time and in order, an Immediate stop that cuts a real decode, and
// a missing model file reported as ModelLoad.
#include "stream/stream_model_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/core.h>

#include "decode/decode_support.hpp"
#include "lws/options.hpp"
#include "lws/streamer.hpp"
#include "lws/text_event.hpp"

namespace {

using lws::test::EventLog;
using std::chrono::milliseconds;
using Lines = std::vector<std::string>;

constexpr std::size_t kCopies = 2;
constexpr std::size_t kPiece = 1600;
// The two load-failure cases push nothing, so the ring size only has to be valid.
constexpr std::size_t kLoadOnlyStream = 160000;
constexpr milliseconds kFirstEventBound{90000};
constexpr milliseconds kImmediateBound{2000};

// The names of the promises the events break: one Text event per copy, each inside its golden
// range, in order, and within the word-error bound of the reference.
Lines broken_promises(const std::vector<lws::TextEvent>& events, std::size_t copies,
                      const lws::test::Words& reference) {
    Lines broken;
    if (events.size() != copies) {
        broken.emplace_back("the number of events is not the number of utterances");
        return broken;
    }
    std::ranges::for_each(std::views::iota(std::size_t{0}, copies), [&](std::size_t i) {
        const lws::TextEvent& event = events[i];
        const lws::test::GoldenRange range = lws::test::golden_range(i);
        const std::size_t errors =
            lws::test::word_errors(lws::test::normalised_words(event.text), reference);
        fmt::println("STREAM-EVENT model={} index={} t_start={} t_end={} text={}",
                     LWS_WHISPER_MODEL_NAME, i + 1, event.t_start, event.t_end, event.text);
        fmt::println("STREAM-WORDS model={} index={} errors={}", LWS_WHISPER_MODEL_NAME, i + 1,
                     errors);
        const std::array<std::pair<bool, const char*>, 4> checks{{
            {event.kind == lws::EventKind::Text, "an event is not a Text event"},
            {event.t_start >= range.begin && event.t_end <= range.end &&
                 event.t_start < event.t_end,
             "an event lies outside its utterance"},
            {!event.after_gap, "an event follows a gap"},
            {std::cmp_less_equal(errors, LWS_DECODE_MAX_WORD_ERRORS), "too many word errors"},
        }};
        std::ranges::for_each(checks, [&](const auto& check) {
            if (!check.first) {
                broken.emplace_back(check.second);
            }
        });
    });
    return broken;
}

void print_wall(const char* test, double elapsed_ms) {
    fmt::println("STREAM-WALL model={} test={} elapsed_ms={:.0f}", LWS_WHISPER_MODEL_NAME, test,
                 elapsed_ms);
}

} // namespace

TEST(StreamerModel, RecordedStreamDrainsToTheTranscriptInOrder) {
    const auto stream = lws::test::recorded_stream(kCopies);
    ASSERT_TRUE(stream.has_value()) << stream.error();
    const auto reference = lws::test::reference_words();
    ASSERT_TRUE(reference.has_value()) << reference.error();

    const auto run = lws::test::stream_through(*stream, kPiece);
    ASSERT_TRUE(run.has_value()) << run.error();
    print_wall("RecordedStreamDrainsToTheTranscriptInOrder", run->elapsed_ms);
    EXPECT_EQ(broken_promises(run->events, kCopies, *reference), Lines{});
}

TEST(StreamerModel, ImmediateCutsTheSecondDecode) {
    const auto stream = lws::test::recorded_stream(kCopies);
    ASSERT_TRUE(stream.has_value()) << stream.error();
    auto streamer = lws::Streamer::create(lws::test::model_options(stream->size()));
    ASSERT_TRUE(streamer.has_value());
    EventLog events;
    ASSERT_TRUE(streamer->on_text([&events](const lws::TextEvent& e) noexcept { events.record(e); })
                    .has_value());
    ASSERT_TRUE(streamer->start().has_value());
    ASSERT_TRUE(lws::test::push_pieces(*streamer, *stream, kPiece));
    ASSERT_TRUE(streamer->flush().has_value());
    ASSERT_TRUE(events.wait_for_count(1, kFirstEventBound));

    std::this_thread::sleep_for(milliseconds{200});
    const auto before = std::chrono::steady_clock::now();
    const bool stopped = streamer->stop(lws::StopMode::Immediate).has_value();
    const auto latency =
        std::chrono::duration_cast<milliseconds>(std::chrono::steady_clock::now() - before);
    fmt::println("STOP-IMMEDIATE-MODEL model={} latency_ms={}", LWS_WHISPER_MODEL_NAME,
                 latency.count());

    EXPECT_TRUE(stopped);
    EXPECT_LT(latency, kImmediateBound);
    EXPECT_EQ(events.events().size(), 1U);
}

TEST(StreamerModel, MissingVadModelIsModelLoad) {
    lws::StreamerOptions options = lws::test::model_options(kLoadOnlyStream);
    options.vad_model_path = "/nonexistent/lws-no-such-vad-model.bin";
    const auto streamer = lws::Streamer::create(options);
    ASSERT_FALSE(streamer.has_value());
    EXPECT_EQ(streamer.error(), lws::Error::ModelLoad);
}

TEST(StreamerModel, MissingWhisperModelIsModelLoad) {
    lws::StreamerOptions options = lws::test::model_options(kLoadOnlyStream);
    options.model_path = "/nonexistent/lws-no-such-whisper-model.bin";
    const auto streamer = lws::Streamer::create(options);
    ASSERT_FALSE(streamer.has_value());
    EXPECT_EQ(streamer.error(), lws::Error::ModelLoad);
}
