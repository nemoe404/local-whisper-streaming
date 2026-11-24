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
