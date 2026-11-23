#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <ranges>
#include <span>
#include <vector>

#include "shell/vad_engine.hpp"

// Fake VAD engine for tests that must not run ggml. Not matched by the tests/vad/*_test.cpp glob,
// and outside the clang-tidy header filter like tests/shell/ring_test_support.hpp, so it holds only
// these helpers.
namespace lws::test {

struct FakeCall {
    enum class Kind : std::uint8_t { Detect, Reset };
    Kind kind;
    std::size_t samples;

    friend bool operator==(const FakeCall&, const FakeCall&) = default;
};

struct FakeVadLog {
    std::vector<FakeCall> calls;
};

// A frame whose first sample is this makes the whole detect call fail with Error::VadFailed.
inline constexpr float kFailMarker = -2.0F;
// A frame whose first sample is this makes the call return one probability fewer than chunks.
inline constexpr float kShortMarker = -3.0F;

inline std::size_t resets(const FakeVadLog& log) {
    return static_cast<std::size_t>(
        std::ranges::count(log.calls, FakeCall::Kind::Reset, &FakeCall::kind));
}

inline std::vector<std::size_t> detect_sizes(const FakeVadLog& log) {
    return log.calls | std::views::filter([](const FakeCall& call) {
               return call.kind == FakeCall::Kind::Detect;
           }) |
           std::views::transform(&FakeCall::samples) | std::ranges::to<std::vector>();
}

// Splits the input into ceil(n / window) chunks like whisper does (the last may be short) and reads
// each chunk's first sample: a marker fails or shortens the call, anything else is the chunk's
// probability clamped to [0, 1]. The probabilities live in a vector the closure owns and replaces
// on every call, like whisper's borrowed buffer. The test owns the log and outlives the engine.
inline VadEngine fake_engine(FakeVadLog& log, std::size_t window = kVadFrameSamples) {
    return VadEngine{
        .detect = [&log, window,
                   probs = std::vector<float>{}](std::span<const float> samples) mutable
            -> std::expected<std::span<const float>, Error> {
            log.calls.push_back({FakeCall::Kind::Detect, samples.size()});
            const std::size_t chunks = (samples.size() + window - 1) / window;
            const auto firsts =
                std::views::iota(std::size_t{0}, chunks) |
                std::views::transform([&](std::size_t i) { return samples[i * window]; });
            if (std::ranges::any_of(firsts, [](float v) { return v == kFailMarker; })) {
                return std::unexpected(Error::VadFailed);
            }
            probs.clear();
            std::ranges::transform(firsts, std::back_inserter(probs),
                                   [](float v) { return std::clamp(v, 0.0F, 1.0F); });
            if (std::ranges::any_of(firsts, [](float v) { return v == kShortMarker; })) {
                probs.pop_back();
            }
            return std::span<const float>{probs};
        },
        .reset = [&log] { log.calls.push_back({FakeCall::Kind::Reset, 0}); },
    };
}

// frame_values.size() frames of kVadFrameSamples samples, each frame filled with its value, so a
// test writes a frame's probability (or a marker) into the audio itself and the fake stays
// independent of how the stream is chunked.
inline std::vector<float> scripted_stream(std::span<const float> frame_values) {
    std::vector<float> stream;
    stream.reserve(frame_values.size() * kVadFrameSamples);
    std::ranges::for_each(frame_values,
                          [&](float v) { stream.insert(stream.end(), kVadFrameSamples, v); });
    return stream;
}

} // namespace lws::test
