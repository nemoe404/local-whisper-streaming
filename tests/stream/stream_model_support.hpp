#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "lws/options.hpp"
#include "lws/streamer.hpp"
#include "lws/streamer_options.hpp"
#include "lws/text_event.hpp"
#include "stream/stream_fakes.hpp"
#include "vad/fixture_io.hpp"

// Helpers for the streamer tests that load the real Silero and whisper models: the recorded clip
// in the shape the tests push, the options that point at the models, and one function that runs a
// whole stream through a streamer. Not matched by the tests/stream/*_test.cpp glob, and outside the
// clang-tidy header filter like the other support headers. Errors are strings.
namespace lws::test {

// One copy of the recorded stream is the clip followed by 64 frames of zeros. The clip holds one
// utterance, so each copy gives one segment. The segment starts are those the real Segmenter cuts
// from the one-copy and two-copy streams (frame-aligned, so the second is not a whole copy after
// the first); at most two copies are supported.
inline constexpr std::size_t kRecordedSegmentSamples = 178176;
inline constexpr std::array<std::uint64_t, 2> kRecordedStarts{1024, 206336};

// The offered-stream range the segment of copy `index` (counting from 0) lies in.
struct GoldenRange {
    std::uint64_t begin;
    std::uint64_t end;
};

inline GoldenRange golden_range(std::size_t index) {
    const std::uint64_t begin = kRecordedStarts.at(index);
    return {.begin = begin, .end = begin + kRecordedSegmentSamples};
}

inline std::expected<std::vector<float>, std::string> recorded_stream(std::size_t copies) {
    const auto clip = load_fixture_wav("clean_en_sample.wav");
    if (!clip) {
        return std::unexpected(clip.error());
    }
    const std::vector<float> silence(64 * kVadFrameSamples, 0.0F);
    const std::array<std::span<const float>, 2> parts{*clip, silence};
    const std::vector<float> copy = parts | std::views::join | std::ranges::to<std::vector>();
    return std::views::repeat(std::span<const float>{copy}, copies) | std::views::join |
           std::ranges::to<std::vector>();
}

// The real models, default tuning, a ring that holds the whole stream and a queue of 8.
inline StreamerOptions model_options(std::size_t stream_samples) {
    StreamerOptions options;
    options.model_path = LWS_WHISPER_MODEL_PATH;
    options.vad_model_path = LWS_SILERO_MODEL_PATH;
    options.ring_capacity_samples = stream_samples + kVadFrameSamples;
    options.queue_segments = 8;
    return options;
}

// The events of a stream and the wall time from start to the return of stop.
struct StreamRun {
    std::vector<TextEvent> events;
    double elapsed_ms = 0.0;
};

// A fresh streamer on the real models: push in pieces, flush, stop(Drain).
inline std::expected<StreamRun, std::string> stream_through(std::span<const float> stream,
                                                            std::size_t piece) {
    auto streamer = Streamer::create(model_options(stream.size()));
    if (!streamer) {
        return std::unexpected("Streamer::create failed");
    }
    EventLog log;
    if (!streamer->on_text([&log](const TextEvent& event) noexcept { log.record(event); })) {
        return std::unexpected("on_text failed");
    }
    const auto begin = std::chrono::steady_clock::now();
    const bool ran = streamer->start().has_value() && push_pieces(*streamer, stream, piece) &&
                     streamer->flush().has_value() && streamer->stop(StopMode::Drain).has_value();
    const std::chrono::duration<double, std::milli> elapsed =
        std::chrono::steady_clock::now() - begin;
    if (!ran) {
        return std::unexpected("a control call or a push was refused");
    }
    return StreamRun{.events = log.events(), .elapsed_ms = elapsed.count()};
}

} // namespace lws::test
