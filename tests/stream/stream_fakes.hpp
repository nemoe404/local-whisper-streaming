#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <ranges>
#include <span>
#include <stop_token>
#include <vector>

#include <fmt/format.h>

#include "lws/decode_policy.hpp"
#include "lws/options.hpp"
#include "lws/segment.hpp"
#include "lws/text_event.hpp"
#include "shell/decoder.hpp"
#include "shell/segmenter.hpp"
#include "shell/vad_engine.hpp"
#include "vad/fake_vad.hpp"
#include "vad/segmenter_support.hpp"

// Fakes for the streamer tests: a VAD engine and a decoder that run no model and can tell which
// frames of the offered stream they were given. Not matched by the tests/stream/*_test.cpp glob,
// and outside the clang-tidy header filter like the other support headers.
//
// An indexed stream writes each frame's value into sample 0 (a probability or a marker, as the
// fake VAD reads it) and the frame's index into samples 1 to 511, so a fake can read back which
// frames a segment holds and a test can check every time against the offered stream.
namespace lws::test {

// One frame per value. The index is exact as a float below 2 to the 24th.
inline std::vector<float> indexed_stream(std::span<const float> frame_values) {
    std::vector<float> stream;
    stream.reserve(frame_values.size() * kVadFrameSamples);
    std::ranges::for_each(
        std::views::iota(std::size_t{0}, frame_values.size()), [&](std::size_t index) {
            stream.push_back(frame_values[index]);
            stream.insert(stream.end(), kVadFrameSamples - 1, static_cast<float>(index));
        });
    return stream;
}

// The frame values of a list of bursts, to feed indexed_stream.
inline std::vector<float> values_of(std::initializer_list<Burst> bursts) {
    std::vector<float> values;
    std::ranges::for_each(bursts, [&](const Burst& burst) {
        values.insert(values.end(), burst.frames, burst.value);
    });
    return values;
}

// What the engine saw. The pump thread writes it; a test reads it only after stop returned.
struct IndexedVadLog {
    struct Call {
        std::uint64_t first_index; // index of the first frame of the call
        std::size_t frames;
        bool failed;
    };
    std::vector<Call> calls;
    std::size_t resets = 0;
};

// Splits the input into ceil(n / 512) chunks like the other fake engine and reads each chunk's
// sample 0: the fail marker fails the whole call, any other value is clamped to [0, 1] and is the
// chunk's probability. A call is logged with the index its first chunk carries (0 for a call too
// short to carry one, like the startup probe). The test owns the log and outlives the engine.
inline VadEngine indexed_engine(IndexedVadLog& log) {
    return VadEngine{
        .detect = [&log, probs = std::vector<float>{}](std::span<const float> samples) mutable
            -> std::expected<std::span<const float>, Error> {
            const std::size_t chunks = (samples.size() + kVadFrameSamples - 1) / kVadFrameSamples;
            const auto firsts =
                std::views::iota(std::size_t{0}, chunks) |
                std::views::transform([&](std::size_t i) { return samples[i * kVadFrameSamples]; });
            const bool failed =
                std::ranges::any_of(firsts, [](float v) { return v == kFailMarker; });
            const std::uint64_t first_index =
                samples.size() >= 2 ? static_cast<std::uint64_t>(samples[1]) : 0;
            log.calls.push_back({first_index, chunks, failed});
            if (failed) {
                return std::unexpected(Error::VadFailed);
            }
            probs.clear();
            std::ranges::transform(firsts, std::back_inserter(probs),
                                   [](float v) { return std::clamp(v, 0.0F, 1.0F); });
            return std::span<const float>{probs};
        },
        .reset = [&log] { ++log.resets; },
    };
}

inline std::string position_text(std::uint64_t first_index, std::uint64_t last_index) {
    return fmt::format("f{} l{}", first_index, last_index);
}

// A decoder whose text names the first and last frame index of the segment it was given, over one
// span from the segment's start to its end.
inline Decoder position_decoder() {
    struct State {
        std::size_t size = 0;
        std::uint64_t first = 0;
        std::uint64_t last = 0;
    };
    auto state = std::make_shared<State>();
    return Decoder{DecodeBackend{
        .run = [state](std::span<const float> samples, std::stop_token) -> std::int32_t {
            state->size = samples.size();
            state->first = static_cast<std::uint64_t>(samples[1]);
            state->last = static_cast<std::uint64_t>(samples[samples.size() - 511]);
            return 0;
        },
        .spans =
            [state] {
                const std::int64_t end_cs = static_cast<std::int64_t>((state->size + 159) / 160);
                return std::vector<RawSpan>{{.t0_cs = 0,
                                             .t1_cs = end_cs,
                                             .text = position_text(state->first, state->last)}};
            },
    }};
}

// What a Segmenter cuts from the stream fed in pieces of `chunk` samples and then flushed: the
// reference the streamer's events are compared against.
inline std::vector<Segment> direct_segments(std::span<const float> stream, std::size_t chunk) {
    IndexedVadLog log;
    auto segmenter = Segmenter::create(indexed_engine(log), Options{}).value();
    auto pushed = push_chunked(segmenter, stream, chunk);
    if (auto window = segmenter.flush()) {
        pushed.segments.push_back(std::move(*window));
    }
    return std::move(pushed.segments);
}

// Collects the events the callback delivers. record runs on the decoder thread; events is read
// after stop returned.
class EventLog {
public:
    void record(const TextEvent& event) noexcept {
        const std::scoped_lock lock{mutex_};
        events_.push_back(event);
    }

    [[nodiscard]] std::vector<TextEvent> events() const {
        const std::scoped_lock lock{mutex_};
        return events_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<TextEvent> events_;
};

} // namespace lws::test
