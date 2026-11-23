// The recorded fixture through the real Silero engine and the Segmenter: two copies of the clip
// with digital silence around them, segmented into committed golden boundaries, checked
// independently against the ONNX reference probabilities.
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "fixture_io.hpp"
#include "lws/options.hpp"
#include "lws/segment.hpp"
#include "shell/chunk_reader.hpp"
#include "shell/segmenter.hpp"
#include "shell/vad_engine.hpp"

namespace {

constexpr const char* kMissingModelHint =
    "model missing: run scripts/fetch-models.sh silero-v5.1.2";

constexpr std::size_t kSilenceSamples = 64 * lws::kVadFrameSamples; // 2.048 s
constexpr std::size_t kStreamSamples = 410176;
constexpr std::size_t kFeedChunk = lws::kVadFrameSamples;
constexpr std::uint64_t kClipSamples = 172320;
constexpr std::uint64_t kSecondCopyStart = kClipSamples + kSilenceSamples;
constexpr float kThreshold = 0.5F;
constexpr const char* kGoldenName = "clean_en_sample.golden.txt";

using Segments = std::vector<lws::Segment>;
using Lines = std::vector<std::string>;

bool model_present() { return std::filesystem::exists(LWS_SILERO_MODEL_PATH); }

// The clip, 2.048 s of zeros, the clip again, 2.048 s of zeros.
std::vector<float> golden_stream(std::span<const float> wav) {
    const std::vector<float> silence(kSilenceSamples, 0.0F);
    const std::array<std::span<const float>, 4> parts{wav, silence, wav, silence};
    return parts | std::views::join | std::ranges::to<std::vector>();
}

// Consecutive pieces of `chunk` samples; the last one may be shorter.
auto pieces_of(std::span<const float> stream, std::size_t chunk) {
    const std::size_t count = (stream.size() + chunk - 1) / chunk;
    return std::views::iota(std::size_t{0}, count) | std::views::transform([=](std::size_t i) {
               return stream.subspan(i * chunk, std::min(chunk, stream.size() - (i * chunk)));
           });
}

std::expected<Segments, std::string>
segment_stream(std::span<const float> stream, std::size_t chunk, const lws::Options& options) {
    auto engine = lws::VadEngine::open(LWS_SILERO_MODEL_PATH);
    if (!engine) {
        return std::unexpected("model load failed");
    }
    auto segmenter = lws::Segmenter::create(std::move(*engine), options);
    if (!segmenter) {
        return std::unexpected("segmenter create failed");
    }
    Segments segments;
    std::optional<std::string> failure;
    std::ranges::for_each(pieces_of(stream, chunk), [&](std::span<const float> piece) {
        if (failure) {
            return;
        }
        auto closed = segmenter->push(piece);
        if (!closed) {
            failure =
                "push failed with error " + std::to_string(static_cast<int>(closed.error().error));
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

std::string_view reason_name(lws::CloseReason reason) {
    switch (reason) {
    case lws::CloseReason::Hangover:
        return "Hangover";
    case lws::CloseReason::Ceiling:
        return "Ceiling";
    case lws::CloseReason::Flush:
        return "Flush";
    case lws::CloseReason::Discontinuity:
        return "Discontinuity";
    }
    return "Unknown";
}

Lines to_golden_lines(const Segments& segments) {
    return segments | std::views::transform([](const lws::Segment& s) {
               return std::to_string(s.start) + " " + std::to_string(s.samples.size()) + " " +
                      std::string(reason_name(s.reason)) + " " +
                      std::to_string(s.continues_previous ? 1 : 0) + " " +
                      std::to_string(s.discontinuous_before ? 1 : 0);
           }) |
           std::ranges::to<std::vector>();
}

std::string joined(const Lines& lines) {
    return std::accumulate(
        lines.begin(), lines.end(), std::string{},
        [](std::string acc, const std::string& line) { return std::move(acc) + line + "\n"; });
}

// The golden file's data lines: every line that is not empty and does not start with '#'.
std::expected<Lines, std::string> load_golden_lines() {
    const std::string path = std::string(LWS_FIXTURE_DIR) + "/" + kGoldenName;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::unexpected("cannot open " + path);
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return std::string_view(text) | std::views::split('\n') |
           std::views::transform([](auto part) { return std::string(part.begin(), part.end()); }) |
           std::views::filter(
               [](const std::string& line) { return !line.empty() && line[0] != '#'; }) |
           std::ranges::to<std::vector>();
}

struct Interval {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
};

Interval interval_of(const lws::Segment& s) {
    return {.begin = s.start, .end = s.start + s.samples.size()};
}

// Overlapping or touching intervals joined.
std::vector<Interval> merged(std::vector<Interval> intervals) {
    std::ranges::sort(intervals, {}, &Interval::begin);
    return std::ranges::fold_left(intervals, std::vector<Interval>{},
                                  [](std::vector<Interval> acc, const Interval& next) {
                                      if (!acc.empty() && next.begin <= acc.back().end) {
                                          acc.back().end = std::max(acc.back().end, next.end);
                                      } else {
                                          acc.push_back(next);
                                      }
                                      return acc;
                                  });
}

bool covers(const std::vector<Interval>& union_of, std::uint64_t begin, std::uint64_t end) {
    return std::ranges::any_of(union_of,
                               [&](const Interval& i) { return i.begin <= begin && end <= i.end; });
}

bool sample_in_a_segment(const Segments& segments, std::uint64_t sample) {
    return std::ranges::any_of(segments, [&](const lws::Segment& s) {
        const Interval i = interval_of(s);
        return i.begin <= sample && sample < i.end;
    });
}

// What the cover check reads for one copy of the clip.
struct CopyReport {
    bool covers_speech = false;
    bool last_is_hangover = false;
    std::uint64_t first_start = 0;
    std::uint64_t last_end = 0;
};

CopyReport report_for_copy(const Segments& segments, std::uint64_t copy_begin,
                           std::uint64_t speech_begin, std::uint64_t speech_end) {
    const auto overlapping = segments | std::views::filter([&](const lws::Segment& s) {
                                 const Interval i = interval_of(s);
                                 return i.end > copy_begin && i.begin < copy_begin + kClipSamples;
                             }) |
                             std::ranges::to<std::vector>();
    if (overlapping.empty()) {
        return {};
    }
    const auto spans =
        overlapping | std::views::transform(interval_of) | std::ranges::to<std::vector>();
    return {.covers_speech = covers(merged(spans), speech_begin, speech_end),
            .last_is_hangover = overlapping.back().reason == lws::CloseReason::Hangover,
            .first_start = overlapping.front().start,
            .last_end = interval_of(overlapping.back()).end};
}

bool is_speech(float p) { return p >= kThreshold; }

// First and last frame of the reference trace at or above the threshold.
struct SpeechSpan {
    std::size_t first = 0;
    std::size_t last = 0;
};

std::optional<SpeechSpan> speech_span_of(std::span<const float> probs) {
    const auto first = std::ranges::find_if(probs, is_speech);
    const auto last = std::ranges::find_last_if(probs, is_speech);
    if (first == probs.end()) {
        return std::nullopt;
    }
    return SpeechSpan{.first = static_cast<std::size_t>(first - probs.begin()),
                      .last = static_cast<std::size_t>(last.begin() - probs.begin())};
}

// Everything the cover check reads, so the test body only compares.
struct CoverFacts {
    CopyReport first_copy;
    CopyReport second_copy;
    std::size_t segment_count = 0;
    std::uint64_t covered_samples = 0;
    std::int64_t gap_samples = 0;
    SpeechSpan span;
    bool middle_of_silence_clear = false;
    bool stream_tail_clear = false;
};

CoverFacts cover_facts(const Segments& segments, SpeechSpan span) {
    // One frame of tolerance at each end: the shift the accepted left-context difference allows.
    const std::uint64_t speech_begin = (span.first + 1) * lws::kVadFrameSamples;
    const std::uint64_t speech_end = span.last * lws::kVadFrameSamples;
    const CopyReport first = report_for_copy(segments, 0, speech_begin, speech_end);
    const CopyReport second = report_for_copy(
        segments, kSecondCopyStart, kSecondCopyStart + speech_begin, kSecondCopyStart + speech_end);
    const auto spans =
        segments | std::views::transform(interval_of) | std::ranges::to<std::vector>();
    const auto covered = std::ranges::fold_left(
        merged(spans), std::uint64_t{0},
        [](std::uint64_t acc, const Interval& i) { return acc + (i.end - i.begin); });
    return {.first_copy = first,
            .second_copy = second,
            .segment_count = segments.size(),
            .covered_samples = covered,
            .gap_samples = static_cast<std::int64_t>(second.first_start) -
                           static_cast<std::int64_t>(first.last_end),
            .span = span,
            .middle_of_silence_clear =
                !sample_in_a_segment(segments, kClipSamples + (kSilenceSamples / 2)),
            .stream_tail_clear = std::ranges::none_of(
                std::views::iota(kStreamSamples - 8000, kStreamSamples),
                [&](std::uint64_t sample) { return sample_in_a_segment(segments, sample); })};
}

Lines failed_checks(const CoverFacts& facts) {
    const std::array<std::pair<bool, const char*>, 7> checks{{
        {facts.first_copy.covers_speech, "first copy speech not covered"},
        {facts.second_copy.covers_speech, "second copy speech not covered"},
        {facts.first_copy.last_is_hangover, "first copy does not end on a hangover"},
        {facts.second_copy.last_is_hangover, "second copy does not end on a hangover"},
        {facts.gap_samples >= 16000, "less than 16000 samples between the copies' segments"},
        {facts.middle_of_silence_clear, "the middle of the first silence is inside a segment"},
        {facts.stream_tail_clear, "the last 8000 samples are inside a segment"},
    }};
    return checks | std::views::filter([](const auto& c) { return !c.first; }) |
           std::views::transform([](const auto& c) { return std::string(c.second); }) |
           std::ranges::to<std::vector>();
}

void print_facts(const CoverFacts& facts) {
    std::cout << "SEGMENTS n=" << facts.segment_count
              << " covered_samples=" << facts.covered_samples << " of=" << kStreamSamples
              << " reference_speech_frames=" << facts.span.first << ".." << facts.span.last
              << " gap_samples=" << facts.gap_samples << "\n";
}

// The reference trace's speech span, then the facts for the segments the code under test made.
std::expected<CoverFacts, std::string> measure_cover(std::span<const float> wav) {
    const auto reference = lws::test::load_probability_trace("clean_en_sample.silero-ref.txt");
    if (!reference) {
        return std::unexpected(reference.error());
    }
    const auto span = speech_span_of(*reference);
    if (!span) {
        return std::unexpected("the reference trace has no frame at or above the threshold");
    }
    const auto segments = segment_stream(golden_stream(wav), kFeedChunk, lws::Options{});
    if (!segments) {
        return std::unexpected(segments.error());
    }
    return cover_facts(*segments, *span);
}

std::expected<lws::VadEngine, std::string> open_engine() {
    auto engine = lws::VadEngine::open(LWS_SILERO_MODEL_PATH);
    if (!engine) {
        return std::unexpected("model load failed");
    }
    return std::move(*engine);
}

// What whisper's own reset variant does: the state is cleared before every detect call.
lws::VadEngine resetting_every_call(lws::VadEngine inner) {
    auto shared = std::make_shared<lws::VadEngine>(std::move(inner));
    return {.detect =
                [shared](std::span<const float> samples) {
                    shared->reset();
                    return shared->detect(samples);
                },
            .reset = [shared] { shared->reset(); }};
}

// Every probability the reader scores over the stream fed in pieces of `chunk` samples.
std::expected<std::vector<float>, std::string>
probabilities_of(lws::VadEngine engine, std::span<const float> stream, std::size_t chunk) {
    auto reader = lws::ChunkReader::create(std::move(engine));
    if (!reader) {
        return std::unexpected("reader create failed");
    }
    std::vector<float> probs;
    std::optional<std::string> failure;
    std::ranges::for_each(pieces_of(stream, chunk), [&](std::span<const float> piece) {
        if (failure) {
            return;
        }
        const auto scored = reader->feed(piece);
        if (!scored) {
            failure = "feed failed";
            return;
        }
        probs.insert(probs.end(), scored->probs.begin(), scored->probs.end());
    });
    if (failure) {
        return std::unexpected(*failure);
    }
    return probs;
}

std::expected<std::vector<float>, std::string> real_probabilities(std::span<const float> stream,
                                                                  std::size_t chunk) {
    auto engine = open_engine();
    if (!engine) {
        return std::unexpected(engine.error());
    }
    return probabilities_of(std::move(*engine), stream, chunk);
}

std::vector<std::uint32_t> bits_of(const std::vector<float>& values) {
    return values | std::views::transform([](float v) { return std::bit_cast<std::uint32_t>(v); }) |
           std::ranges::to<std::vector>();
}

bool same_segment(const lws::Segment& a, const lws::Segment& b) {
    return a.start == b.start && a.reason == b.reason &&
           a.continues_previous == b.continues_previous &&
           a.discontinuous_before == b.discontinuous_before &&
           bits_of(a.samples) == bits_of(b.samples);
}

bool same_segments(const Segments& a, const Segments& b) {
    return std::ranges::equal(a, b, same_segment);
}

// The chunk sizes whose segments differ from `expected` in any field or sample bit, or from the
// golden lines; a run that fails counts as differing.
std::vector<std::size_t> differing_chunk_sizes(std::span<const float> stream,
                                               std::span<const std::size_t> chunks,
                                               const Segments& expected, const Lines& golden) {
    return chunks | std::views::filter([&](std::size_t chunk) {
               const auto run = segment_stream(stream, chunk, lws::Options{});
               return !run || !same_segments(*run, expected) || to_golden_lines(*run) != golden;
           }) |
           std::ranges::to<std::vector>();
}

std::size_t differing_frames(const std::vector<float>& a, const std::vector<float>& b) {
    const auto bits_a = bits_of(a);
    const auto bits_b = bits_of(b);
    return static_cast<std::size_t>(std::ranges::count(
        std::views::zip_transform([](std::uint32_t x, std::uint32_t y) { return x != y; }, bits_a,
                                  bits_b),
        true));
}

} // namespace

TEST(SegmentationModel, GoldenBoundaries) {
    if (!model_present()) {
        GTEST_SKIP() << kMissingModelHint;
    }
    const auto wav = lws::test::load_fixture_wav("clean_en_sample.wav");
    ASSERT_TRUE(wav.has_value()) << wav.error();
    const auto stream = golden_stream(*wav);
    ASSERT_EQ(stream.size(), kStreamSamples);

    const auto segments = segment_stream(stream, kFeedChunk, lws::Options{});
    ASSERT_TRUE(segments.has_value()) << segments.error();
    const Lines computed = to_golden_lines(*segments);
    const auto golden = load_golden_lines();
    EXPECT_TRUE(golden.has_value() && *golden == computed)
        << "golden file differs or is absent; computed lines in the golden format:\n"
        << joined(computed);
}

TEST(SegmentationModel, SegmentsCoverReferenceSpeechAndSkipSilence) {
    if (!model_present()) {
        GTEST_SKIP() << kMissingModelHint;
    }
    const auto wav = lws::test::load_fixture_wav("clean_en_sample.wav");
    ASSERT_TRUE(wav.has_value()) << wav.error();
    ASSERT_EQ(wav->size(), kClipSamples);
    const auto facts = measure_cover(*wav);
    ASSERT_TRUE(facts.has_value()) << facts.error();
    print_facts(*facts);
    EXPECT_EQ(failed_checks(*facts), Lines{});
}

TEST(SegmentationModel, SegmentsIdenticalAcrossChunkSizes) {
    if (!model_present()) {
        GTEST_SKIP() << kMissingModelHint;
    }
    const auto wav = lws::test::load_fixture_wav("clean_en_sample.wav");
    ASSERT_TRUE(wav.has_value()) << wav.error();
    const auto stream = golden_stream(*wav);
    const auto base = segment_stream(stream, kFeedChunk, lws::Options{});
    ASSERT_TRUE(base.has_value()) << base.error();
    const auto golden = load_golden_lines();
    ASSERT_TRUE(golden.has_value()) << golden.error();
    ASSERT_EQ(to_golden_lines(*base), *golden);

    constexpr std::array<std::size_t, 3> kOtherChunks{160, 1000, 4096};
    const auto differing = differing_chunk_sizes(stream, kOtherChunks, *base, *golden);
    std::cout << "CHUNK-INVARIANCE segments=" << base->size()
              << " chunk_sizes=160,512,1000,4096 differing=" << differing.size() << "\n";
    EXPECT_EQ(differing, std::vector<std::size_t>{});
}

TEST(SegmentationModel, ProbabilitiesIdenticalAcrossChunkSizes) {
    if (!model_present()) {
        GTEST_SKIP() << kMissingModelHint;
    }
    const auto wav = lws::test::load_fixture_wav("clean_en_sample.wav");
    ASSERT_TRUE(wav.has_value()) << wav.error();
    const auto stream = golden_stream(*wav);
    const auto base = real_probabilities(stream, kFeedChunk);
    ASSERT_TRUE(base.has_value()) << base.error();
    ASSERT_EQ(base->size(), kStreamSamples / lws::kVadFrameSamples);

    constexpr std::array<std::size_t, 3> kOtherChunks{160, 1000, 4096};
    const auto differing = kOtherChunks | std::views::filter([&](std::size_t chunk) {
                               const auto probs = real_probabilities(stream, chunk);
                               return !probs || bits_of(*probs) != bits_of(*base);
                           }) |
                           std::ranges::to<std::vector>();
    std::cout << "PROB-INVARIANCE frames=" << base->size()
              << " chunk_sizes=160,512,1000,4096 differing=" << differing.size() << "\n";
    EXPECT_EQ(differing, std::vector<std::size_t>{});
}

TEST(SegmentationModel, ResetEveryCallBreaksInvariance) {
    if (!model_present()) {
        GTEST_SKIP() << kMissingModelHint;
    }
    const auto wav = lws::test::load_fixture_wav("clean_en_sample.wav");
    ASSERT_TRUE(wav.has_value()) << wav.error();
    const auto stream = golden_stream(*wav);
    const auto probs_for = [&](std::size_t chunk) {
        auto engine = open_engine();
        return engine ? probabilities_of(resetting_every_call(std::move(*engine)), stream, chunk)
                      : std::expected<std::vector<float>, std::string>(
                            std::unexpected(engine.error()));
    };
    const auto small = probs_for(512);
    const auto large = probs_for(4096);
    ASSERT_TRUE(small.has_value()) << small.error();
    ASSERT_TRUE(large.has_value()) << large.error();
    ASSERT_EQ(small->size(), large->size());
    const std::size_t differing = differing_frames(*small, *large);
    std::cout << "RESET-CONTROL frames=" << small->size() << " differing_frames=" << differing
              << " chunks=512,4096\n";
    EXPECT_GE(differing, std::size_t{1});
}
