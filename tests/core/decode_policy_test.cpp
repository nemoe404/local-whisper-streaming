// The core decides the floor, the time base and the meaning of a return code, so the shell never
// reads text from a failed or cancelled run. These tests pin those decisions as plain values.
#include "lws/decode_policy.hpp"

#include "lws/options.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

lws::RawSpan raw(std::int64_t t0, std::int64_t t1, std::string text) {
    return lws::RawSpan{.t0_cs = t0, .t1_cs = t1, .text = std::move(text)};
}

lws::DecoderOptions with_threads(std::int32_t n) {
    lws::DecoderOptions options;
    options.n_threads = n;
    return options;
}

lws::DecoderOptions with_language(std::string language) {
    lws::DecoderOptions options;
    options.language = std::move(language);
    return options;
}

lws::DecoderOptions with_temperature_inc(float value) {
    lws::DecoderOptions options;
    options.temperature_inc = value;
    return options;
}

void expect_invalid(const lws::DecoderOptions& options) {
    const auto result = lws::validate(options);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), lws::Error::InvalidDecoderOptions);
}

} // namespace

// The raw spans measured on the fixture clip become samples from the segment's first sample.
TEST(DecodePolicy, MeasuredClipSpansMapToSamplesFromTheSegmentStart) {
    const std::array spans{raw(0, 524, " By the time he reached his own street, "),
                           raw(524, 1060, " he was keenly alive.\n")};
    const auto mapped = lws::map_spans(spans, 172320);
    ASSERT_EQ(mapped.size(), 2U);
    EXPECT_EQ(mapped[0].begin, 0U);
    EXPECT_EQ(mapped[0].end, 83840U);
    EXPECT_EQ(mapped[0].text, "By the time he reached his own street,");
    EXPECT_EQ(mapped[1].begin, 83840U);
    EXPECT_EQ(mapped[1].end, 169600U);
    EXPECT_EQ(mapped[1].text, "he was keenly alive.");

    EXPECT_EQ(lws::classify_run(-6, true), lws::RunOutcome::Cancelled);
    EXPECT_EQ(lws::classify_run(-6, false), lws::RunOutcome::Failed);
    EXPECT_FALSE(lws::is_decodable(1599));
    EXPECT_TRUE(lws::is_decodable(1600));

    const auto options = lws::validate(lws::DecoderOptions{});
    ASSERT_TRUE(options.has_value());
    EXPECT_EQ(options->n_threads, 4);
    EXPECT_EQ(options->language, "en");
}

// 100 ms at 16 kHz is 1600 samples; one more sample and every shorter length are on each side.
TEST(DecodePolicy, FloorIsOneHundredMillisecondsAndTenMillisecondUnitIs160Samples) {
    EXPECT_EQ(lws::kMinDecodeSamples, 1600U);
    EXPECT_EQ(lws::kSamplesPerCentisecond, 160U);
    const std::array below{std::size_t{0}, std::size_t{512}, std::size_t{1536}, std::size_t{1599}};
    const std::array above{std::size_t{1600}, std::size_t{1601}};
    std::ranges::for_each(below, [](std::size_t n) { EXPECT_FALSE(lws::is_decodable(n)) << n; });
    std::ranges::for_each(above, [](std::size_t n) { EXPECT_TRUE(lws::is_decodable(n)) << n; });
}

TEST(DecodePolicy, CentisecondsConvertToSamplesAndSaturateInsteadOfOverflowing) {
    EXPECT_EQ(lws::cs_to_samples(0), 0U);
    EXPECT_EQ(lws::cs_to_samples(1), 160U);
    EXPECT_EQ(lws::cs_to_samples(524), 83840U);
    EXPECT_EQ(lws::cs_to_samples(-5), 0U);
    EXPECT_EQ(lws::cs_to_samples(std::numeric_limits<std::int64_t>::min()), 0U);
    EXPECT_EQ(lws::cs_to_samples(std::numeric_limits<std::int64_t>::max()),
              std::numeric_limits<std::uint64_t>::max());
}

// A stop is never an error, and a failure is never taken for a stop.
TEST(DecodePolicy, ReturnCodeIsClassifiedAgainstTheStopFlag) {
    using lws::RunOutcome;
    struct Row {
        std::int32_t rc;
        bool stop;
        RunOutcome expected;
    };
    const std::array rows{Row{.rc = 0, .stop = false, .expected = RunOutcome::Ok},
                          Row{.rc = 0, .stop = true, .expected = RunOutcome::Ok},
                          Row{.rc = -6, .stop = true, .expected = RunOutcome::Cancelled},
                          Row{.rc = -8, .stop = true, .expected = RunOutcome::Cancelled},
                          Row{.rc = -9, .stop = true, .expected = RunOutcome::Cancelled},
                          Row{.rc = -6, .stop = false, .expected = RunOutcome::Failed},
                          Row{.rc = -2, .stop = false, .expected = RunOutcome::Failed},
                          Row{.rc = 1, .stop = false, .expected = RunOutcome::Failed}};
    std::ranges::for_each(rows, [](const Row& row) {
        EXPECT_EQ(lws::classify_run(row.rc, row.stop), row.expected)
            << "rc " << row.rc << " stop " << row.stop;
    });
}

TEST(DecodePolicy, SpansAreClampedTrimmedFilteredAndKeepTheirOrder) {
    // A time past the end of the segment is clamped to its last sample.
    {
        const std::array spans{raw(0, 1000, "late")};
        const auto mapped = lws::map_spans(spans, 1800);
        ASSERT_EQ(mapped.size(), 1U);
        EXPECT_EQ(mapped[0].begin, 0U);
        EXPECT_EQ(mapped[0].end, 1800U);
    }
    // All six ASCII whitespace characters are trimmed from both ends, inner ones stay.
    {
        const std::array spans{raw(0, 10, " hello \n"), raw(10, 20, "\t\r\v\fa b\f\v\r\t")};
        const auto mapped = lws::map_spans(spans, 3200);
        ASSERT_EQ(mapped.size(), 2U);
        EXPECT_EQ(mapped[0].text, "hello");
        EXPECT_EQ(mapped[1].text, "a b");
    }
    // Empty and whitespace-only text is dropped.
    {
        const std::array spans{raw(0, 10, ""), raw(10, 20, "  \t"), raw(20, 30, "kept")};
        const auto mapped = lws::map_spans(spans, 4800);
        ASSERT_EQ(mapped.size(), 1U);
        EXPECT_EQ(mapped[0].text, "kept");
        EXPECT_EQ(mapped[0].begin, 3200U);
    }
    // An end before the begin collapses to the begin.
    {
        const std::array spans{raw(30, 10, "backwards")};
        const auto mapped = lws::map_spans(spans, 4800);
        ASSERT_EQ(mapped.size(), 1U);
        EXPECT_EQ(mapped[0].begin, 4800U);
        EXPECT_EQ(mapped[0].end, mapped[0].begin);
    }
    // A negative begin starts at the first sample.
    {
        const std::array spans{raw(-40, 5, "early")};
        const auto mapped = lws::map_spans(spans, 4800);
        ASSERT_EQ(mapped.size(), 1U);
        EXPECT_EQ(mapped[0].begin, 0U);
        EXPECT_EQ(mapped[0].end, 800U);
    }
    // Order of the input is the order of the output, whatever the times are.
    {
        const std::array spans{raw(50, 60, "third"), raw(0, 10, "first"), raw(20, 30, "second")};
        const auto mapped = lws::map_spans(spans, 9600);
        ASSERT_EQ(mapped.size(), 3U);
        EXPECT_EQ(mapped[0].text, "third");
        EXPECT_EQ(mapped[1].text, "first");
        EXPECT_EQ(mapped[2].text, "second");
    }
    // No input, no output.
    EXPECT_TRUE(lws::map_spans({}, 1600).empty());
}

namespace {

// Builds one list of raw spans from the generator: times from before the segment to well past it,
// text that is sometimes only blanks, and blanks around the words.
std::vector<lws::RawSpan> generated_list(std::mt19937& gen) {
    static const std::array<std::string, 6> words{"alpha", "bravo", "charlie",
                                                  "delta", "echo",  "foxtrot"};
    static const std::array<std::string, 4> blanks{"", " ", "\n", " \t "};
    std::uniform_int_distribution<std::int64_t> time_dist(-200, 4000);
    std::uniform_int_distribution<std::size_t> count_dist(0, 8);
    std::uniform_int_distribution<std::size_t> word_dist(0, words.size() - 1);
    std::uniform_int_distribution<std::size_t> blank_dist(0, blanks.size() - 1);
    std::uniform_int_distribution<int> blank_only_dist(0, 4);

    const auto make_span = [&](std::size_t) {
        std::string text = blanks.at(blank_dist(gen));
        if (blank_only_dist(gen) != 0) {
            text += words.at(word_dist(gen));
            text += blanks.at(blank_dist(gen));
        }
        const std::int64_t t0 = time_dist(gen);
        const std::int64_t t1 = time_dist(gen);
        return raw(t0, t1, std::move(text));
    };
    return std::views::iota(std::size_t{0}, count_dist(gen)) | std::views::transform(make_span) |
           std::ranges::to<std::vector>();
}

void expect_span_inside(const lws::TextSpan& span, std::size_t n_samples, int list_index) {
    constexpr std::string_view blank_chars = " \t\n\r\v\f";
    EXPECT_LE(span.begin, span.end) << "list " << list_index;
    EXPECT_LE(span.end, n_samples) << "list " << list_index;
    EXPECT_FALSE(span.text.empty()) << "list " << list_index;
    EXPECT_EQ(span.text.find_first_not_of(blank_chars), 0U) << "list " << list_index;
    EXPECT_EQ(span.text.find_last_not_of(blank_chars), span.text.size() - 1U)
        << "list " << list_index;
}

} // namespace

// Fifty lists drawn from a fixed seed: whatever whisper says, every mapped span lies inside the
// segment and carries trimmed text.
TEST(DecodePolicy, EveryMappedSpanOverGeneratedListsLiesInsideTheSegmentWithText) {
    // A fixed seed on purpose: the table must be the same on every run.
    // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp)
    std::mt19937 gen(20240607U);
    std::uniform_int_distribution<std::size_t> samples_dist(1600, 80000);

    std::ranges::for_each(std::views::iota(0, 50), [&](int list_index) {
        const std::vector<lws::RawSpan> spans = generated_list(gen);
        const std::size_t n = samples_dist(gen);
        const auto mapped = lws::map_spans(spans, n);
        EXPECT_LE(mapped.size(), spans.size()) << "list " << list_index;
        std::ranges::for_each(
            mapped, [&](const lws::TextSpan& span) { expect_span_inside(span, n, list_index); });
    });
}

TEST(DecodePolicy, DefaultsAreFourThreadsEnglishWhisperTemperatureStepAndGpu) {
    const lws::DecoderOptions defaults;
    EXPECT_EQ(defaults.n_threads, 4);
    EXPECT_EQ(defaults.language, "en");
    EXPECT_EQ(defaults.temperature_inc, 0.2F);
    EXPECT_TRUE(defaults.use_gpu);
    const auto result = lws::validate(defaults);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->n_threads, defaults.n_threads);
    EXPECT_EQ(result->language, defaults.language);
    EXPECT_EQ(result->temperature_inc, defaults.temperature_inc);
    EXPECT_EQ(result->use_gpu, defaults.use_gpu);
}

TEST(DecodePolicy, OptionsThatCannotRunAreInvalidDecoderOptions) {
    expect_invalid(with_threads(0));
    expect_invalid(with_threads(-1));
    expect_invalid(with_language(""));
    expect_invalid(with_temperature_inc(-0.1F));
    expect_invalid(with_temperature_inc(std::numeric_limits<float>::quiet_NaN()));
    expect_invalid(with_temperature_inc(std::numeric_limits<float>::infinity()));
}

// Zero is a legal step: it switches the temperature retry off.
TEST(DecodePolicy, ZeroTemperatureStepAndOneThreadAreAccepted) {
    EXPECT_TRUE(lws::validate(with_temperature_inc(0.0F)).has_value());
    EXPECT_TRUE(lws::validate(with_threads(1)).has_value());
}
