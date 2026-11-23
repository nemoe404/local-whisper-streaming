#pragma once

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include "fake_vad.hpp"
#include "lws/options.hpp"
#include "lws/segment.hpp"
#include "shell/segmenter.hpp"

// Helpers shared by the Segmenter tests. Not matched by the tests/vad/*_test.cpp glob, and outside
// the clang-tidy header filter like fake_vad.hpp.
namespace lws::test {

// `frames` consecutive frames whose samples all equal `value`.
struct Burst {
    constexpr Burst(float v, std::size_t n) noexcept : value{v}, frames{n} {}
    float value;
    std::size_t frames;
};

inline std::vector<float> stream_of(std::initializer_list<Burst> bursts) {
    std::vector<float> stream;
    std::ranges::for_each(bursts, [&](const Burst& burst) {
        const auto frames = scripted_stream(std::vector<float>(burst.frames, burst.value));
        stream.insert(stream.end(), frames.begin(), frames.end());
    });
    return stream;
}

// What pushing a stream in pieces produced: every closed segment, and the first failure (the
// pushes after a failure are skipped).
struct Pushed {
    std::vector<Segment> segments;
    std::optional<VadFailure> failure;
};

inline Pushed push_chunked(Segmenter& segmenter, std::span<const float> stream,
                           std::size_t chunk = 1000) {
    Pushed pushed;
    std::ranges::for_each(
        stream | std::views::chunk(static_cast<std::ptrdiff_t>(chunk)), [&](auto piece) {
            if (pushed.failure) {
                return;
            }
            auto result = segmenter.push(std::span<const float>{piece.begin(), piece.end()});
            if (result) {
                std::ranges::move(*result, std::back_inserter(pushed.segments));
            } else {
                pushed.failure = std::move(result.error());
            }
        });
    return pushed;
}

// The failure a test expects, or a throw that gtest reports as a failed test. Taking the optional
// by value gives clang-tidy a checked access.
inline VadFailure failure_of(std::optional<VadFailure> failure) {
    return std::move(failure).value();
}

inline Segment segment_of(std::optional<Segment> segment) { return std::move(segment).value(); }

// A segmenter over a fake engine; options that fail to validate throw and fail the test loudly.
inline Segmenter make_segmenter(FakeVadLog& log, const Options& options = Options{}) {
    return Segmenter::create(fake_engine(log), options).value();
}

} // namespace lws::test
