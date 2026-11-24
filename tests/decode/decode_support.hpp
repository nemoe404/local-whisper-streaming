#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lws/options.hpp"
#include "lws/segment.hpp"
#include "shell/segmenter.hpp"
#include "shell/vad_engine.hpp"
#include "vad/fixture_io.hpp"

// Helpers shared by the decode tests that load a model: the recorded clip in the shapes the tests
// feed, a word-level error count against the reference transcript, and the segments the real
// Segmenter cuts from the recorded stream. Not matched by the tests/decode/*_test.cpp glob, and
// outside the clang-tidy header filter like the other support headers. Errors are strings.
namespace lws::test {

using Words = std::vector<std::string>;

// ASCII uppercase; letters, digits and apostrophes are kept, every other character separates words
// and empty pieces are dropped.
inline Words normalised_words(std::string_view text) {
    const std::string mapped =
        text | std::views::transform([](char c) {
            if (c >= 'a' && c <= 'z') {
                return static_cast<char>(c - 'a' + 'A');
            }
            const bool kept = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '\'';
            return kept ? c : ' ';
        }) |
        std::ranges::to<std::string>();
    return std::string_view(mapped) | std::views::split(' ') |
           std::views::transform([](auto part) { return std::string(part.begin(), part.end()); }) |
           std::views::filter([](const std::string& word) { return !word.empty(); }) |
           std::ranges::to<Words>();
}

// Word-level Levenshtein distance. One row of the table per word of `a`.
inline std::size_t word_errors(const Words& a, const Words& b) {
    using Row = std::vector<std::size_t>;
    Row first(b.size() + 1);
    std::ranges::copy(std::views::iota(std::size_t{0}, first.size()), first.begin());
    const Row last =
        std::ranges::fold_left(a, first, [&](const Row& previous, const std::string& word) {
            Row next(previous.size());
            next.front() = previous.front() + 1;
            std::ranges::for_each(
                std::views::iota(std::size_t{1}, next.size()), [&](std::size_t j) {
                    const std::size_t substitute = previous[j - 1] + (word == b[j - 1] ? 0U : 1U);
                    next[j] = std::min({previous[j] + 1, next[j - 1] + 1, substitute});
                });
            return next;
        });
    return last.back();
}

// The words of the committed reference transcript of the recorded clip.
inline std::expected<Words, std::string> reference_words() {
    const auto text = detail::read_whole_file("clean_en_sample.ref.txt");
    if (!text) {
        return std::unexpected(text.error());
    }
    return normalised_words(*text);
}

// clip, 8000 zeros, clip, 8000 zeros, clip.
inline std::vector<float> three_copies(std::span<const float> clip) {
    const std::vector<float> gap(8000, 0.0F);
    const std::array<std::span<const float>, 5> parts{clip, gap, clip, gap, clip};
    return parts | std::views::join | std::ranges::to<std::vector>();
}

namespace detail {

// Consecutive pieces of `chunk` samples; the last one may be shorter.
inline auto pieces_of(std::span<const float> stream, std::size_t chunk) {
    const std::size_t count = (stream.size() + chunk - 1) / chunk;
    return std::views::iota(std::size_t{0}, count) | std::views::transform([=](std::size_t i) {
               return stream.subspan(i * chunk, std::min(chunk, stream.size() - (i * chunk)));
           });
}

} // namespace detail

// The clip, 32768 zeros, the clip, 32768 zeros, through the real Silero engine and a Segmenter
// with default options, fed in 512-sample pieces and then flushed. Returns the closed segments.
inline std::expected<std::vector<Segment>, std::string> recorded_segments() {
    const auto clip = load_fixture_wav("clean_en_sample.wav");
    if (!clip) {
        return std::unexpected(clip.error());
    }
    const std::vector<float> silence(64 * kVadFrameSamples, 0.0F);
    const std::array<std::span<const float>, 4> parts{*clip, silence, *clip, silence};
    const std::vector<float> stream = parts | std::views::join | std::ranges::to<std::vector>();

    auto engine = VadEngine::open(LWS_SILERO_MODEL_PATH);
    if (!engine) {
        return std::unexpected("model load failed");
    }
    auto segmenter = Segmenter::create(std::move(*engine), Options{});
    if (!segmenter) {
        return std::unexpected("segmenter create failed");
    }
    std::vector<Segment> segments;
    std::optional<std::string> failure;
    std::ranges::for_each(detail::pieces_of(stream, kVadFrameSamples),
                          [&](std::span<const float> piece) {
                              if (failure) {
                                  return;
                              }
                              auto closed = segmenter->push(piece);
                              if (!closed) {
                                  failure = "push failed";
                                  return;
                              }
                              std::ranges::move(*closed, std::back_inserter(segments));
                          });
    if (failure) {
        return std::unexpected(*failure);
    }
    if (auto last = segmenter->flush()) {
        segments.push_back(std::move(*last));
    }
    return segments;
}

} // namespace lws::test
