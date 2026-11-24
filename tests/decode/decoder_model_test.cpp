// The recorded clip through the real Silero engine and the Segmenter, then each closed segment
// through a Decoder on a real whisper model: text equal to the reference transcript, times counted
// from the segment's start, one whisper call per segment.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <ranges>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <fmt/base.h>
#include <gtest/gtest.h>

#include "decode/decode_support.hpp"
#include "lws/decode_policy.hpp"
#include "lws/options.hpp"
#include "lws/segment.hpp"
#include "shell/decoder.hpp"

namespace {

constexpr std::size_t kSegmentSamples = 178176;
constexpr std::uint64_t kFirstStart = 1024;
constexpr std::uint64_t kSecondStart = 206336;
constexpr std::uint64_t kFirstWordWindow = 16000;
constexpr std::uint64_t kLastEndSlack = 48000;

std::string joined_text(const std::vector<lws::TextSpan>& spans) {
    return spans | std::views::transform([](const lws::TextSpan& s) { return s.text + " "; }) |
           std::views::join | std::ranges::to<std::string>();
}

bool spans_inside_and_ordered(const std::vector<lws::TextSpan>& spans, std::uint64_t length) {
    const bool inside = std::ranges::all_of(
        spans, [&](const lws::TextSpan& s) { return s.begin <= s.end && s.end <= length; });
    const bool ordered = std::ranges::is_sorted(spans, {}, &lws::TextSpan::begin);
    return inside && ordered;
}

void print_segment(std::size_t ordinal, const lws::DecodeResult& result, std::size_t errors) {
    std::ranges::for_each(result.spans, [&](const lws::TextSpan& s) {
        fmt::println("DECODE-SPAN model={} segment={} begin={} end={} text={}",
                     LWS_WHISPER_MODEL_NAME, ordinal, s.begin, s.end, s.text);
    });
    fmt::println("DECODE-WORDS model={} segment={} errors={}", LWS_WHISPER_MODEL_NAME, ordinal,
                 errors);
}

} // namespace

namespace {

using Lines = std::vector<std::string>;

// The recorded stream's two segments, or a message naming what differs from the golden shape.
std::expected<std::vector<lws::Segment>, std::string> golden_segments() {
    auto segments = lws::test::recorded_segments();
    if (!segments) {
        return std::unexpected(segments.error());
    }
    const bool shaped = segments->size() == 2U && segments->at(0).start == kFirstStart &&
                        segments->at(1).start == kSecondStart &&
                        segments->at(0).samples.size() == kSegmentSamples &&
                        segments->at(1).samples.size() == kSegmentSamples;
    if (!shaped) {
        return std::unexpected("the recorded segments differ from the golden starts and sizes");
    }
    return segments;
}

// The names of the promises the decoded segment breaks.
Lines broken_promises(const lws::DecodeResult& result, std::uint64_t length, std::size_t errors) {
    const std::array<std::pair<bool, const char*>, 5> checks{{
        {result.status == lws::DecodeStatus::Decoded, "status is not Decoded"},
        {spans_inside_and_ordered(result.spans, length),
         "a span is outside the segment or out of order"},
        {result.spans.front().begin < kFirstWordWindow, "the first span starts late"},
        {result.spans.back().end + kLastEndSlack >= length, "the last span ends early"},
        {errors <= static_cast<std::size_t>(LWS_DECODE_MAX_WORD_ERRORS), "too many word errors"},
    }};
    return checks | std::views::filter([](const auto& c) { return !c.first; }) |
           std::views::transform([](const auto& c) { return std::string(c.second); }) |
           std::ranges::to<std::vector>();
}

// Decodes one recorded segment with a fresh, unstopped token and checks it against the reference.
void expect_segment_decodes(lws::Decoder& decoder, const lws::Segment& segment, std::size_t ordinal,
                            const lws::test::Words& reference) {
    const std::stop_source source;
    const auto result = decoder.decode(segment, source.get_token());
    ASSERT_TRUE(result.has_value()) << "segment " << ordinal;
    ASSERT_FALSE(result->spans.empty());
    const std::size_t errors =
        lws::test::word_errors(lws::test::normalised_words(joined_text(result->spans)), reference);
    print_segment(ordinal, *result, errors);
    EXPECT_EQ(decoder.whisper_calls(), ordinal);
    EXPECT_EQ(broken_promises(*result, segment.samples.size(), errors), Lines{})
        << "segment " << ordinal;
}

} // namespace

TEST(DecoderModel, RecordedSegmentsDecodeToTheTranscriptOneCallEach) {
    const auto segments = golden_segments();
    ASSERT_TRUE(segments.has_value()) << segments.error();
    const auto reference = lws::test::reference_words();
    ASSERT_TRUE(reference.has_value()) << reference.error();

    auto decoder = lws::Decoder::open(LWS_WHISPER_MODEL_PATH, lws::DecoderOptions{});
    ASSERT_TRUE(decoder.has_value());

    std::ranges::for_each(std::views::iota(std::size_t{0}, segments->size()), [&](std::size_t i) {
        expect_segment_decodes(*decoder, segments->at(i), i + 1, *reference);
    });
}

TEST(DecoderModel, MissingModelIsModelLoad) {
    const auto decoder =
        lws::Decoder::open("/nonexistent/lws-no-such-model.bin", lws::DecoderOptions{});
    ASSERT_FALSE(decoder.has_value());
    EXPECT_EQ(decoder.error(), lws::Error::ModelLoad);
}

TEST(DecoderModel, InvalidOptionsAreRefusedBeforeTheModelLoads) {
    lws::DecoderOptions options;
    options.n_threads = 0;
    const auto decoder = lws::Decoder::open("/nonexistent/lws-no-such-model.bin", options);
    ASSERT_FALSE(decoder.has_value());
    EXPECT_EQ(decoder.error(), lws::Error::InvalidDecoderOptions);
}
