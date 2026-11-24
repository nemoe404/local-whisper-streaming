// The decoder's control flow against a fake backend: the length floor, one run call per decodable
// segment, how a run result is classified, and the time base of the returned spans.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <stop_token>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include "decode/fake_decoder.hpp"
#include "lws/decode_policy.hpp"
#include "lws/options.hpp"
#include "lws/segment.hpp"
#include "shell/decoder.hpp"

namespace {

lws::Segment silent_segment(std::size_t n_samples) {
    return lws::Segment{.samples = std::vector<float>(n_samples, 0.0F)};
}

lws::RawSpan raw_span(std::int64_t t0_cs, std::int64_t t1_cs) {
    return lws::RawSpan{.t0_cs = t0_cs, .t1_cs = t1_cs, .text = "word"};
}

constexpr std::int32_t kEncodeFailed = -6;
constexpr std::int32_t kDecodeFailed = -8;
constexpr std::int32_t kGraphFailed = -9;

} // namespace

namespace {

// What a decode of `n` silent samples did: status, spans returned, run calls, span reads and the
// decoder's own call count.
using Observed =
    std::tuple<lws::DecodeStatus, std::size_t, std::size_t, std::size_t, std::uint64_t>;

Observed observe_decode(std::size_t n) {
    lws::test::FakeDecodeLog log;
    lws::Decoder decoder{lws::test::fake_backend(log, {.spans = {raw_span(0, 10)}})};
    const auto result = decoder.decode(silent_segment(n), std::stop_source{}.get_token());
    const auto status = result ? result->status : lws::DecodeStatus::Cancelled;
    const std::size_t spans = result ? result->spans.size() : 99U;
    return {status, spans, log.run_sizes.size(), log.span_reads, decoder.whisper_calls()};
}

} // namespace

TEST(DecoderControl, SegmentsBelowTheFloorNeverReachTheBackend) {
    constexpr std::array<std::size_t, 4> kShort{0, 512, 1536, 1599};
    std::ranges::for_each(kShort, [](std::size_t n) {
        EXPECT_EQ(observe_decode(n), (Observed{lws::DecodeStatus::TooShort, 0U, 0U, 0U, 0U})) << n;
    });
}

TEST(DecoderControl, SegmentsAtOrAboveTheFloorMakeOneCall) {
    constexpr std::array<std::size_t, 2> kLongEnough{1600, 1601};
    std::ranges::for_each(kLongEnough, [](std::size_t n) {
        lws::test::FakeDecodeLog log;
        lws::Decoder decoder{lws::test::fake_backend(log, {})};
        const auto result = decoder.decode(silent_segment(n), std::stop_source{}.get_token());
        ASSERT_TRUE(result.has_value()) << n;
        EXPECT_EQ(result->status, lws::DecodeStatus::Decoded) << n;
        EXPECT_EQ(log.run_sizes, std::vector<std::size_t>{n});
        EXPECT_EQ(decoder.whisper_calls(), 1U) << n;
    });
}

TEST(DecoderControl, EveryDecodableSegmentMakesExactlyOneCallInOrder) {
    const std::vector<std::size_t> sizes{1600, 2000, 16000, 48000, 1601};
    lws::test::FakeDecodeLog log;
    lws::Decoder decoder{lws::test::fake_backend(log, {})};
    const std::stop_source source;
    std::ranges::for_each(sizes, [&](std::size_t n) {
        const auto result = decoder.decode(silent_segment(n), source.get_token());
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->status, lws::DecodeStatus::Decoded);
    });
    EXPECT_EQ(log.run_sizes, sizes);
    EXPECT_EQ(decoder.whisper_calls(), sizes.size());
}

TEST(DecoderControl, ARunAbortedByStopIsCancelledWithNoSpansRead) {
    constexpr std::array<std::int32_t, 3> kCodes{kEncodeFailed, kDecodeFailed, kGraphFailed};
    std::ranges::for_each(kCodes, [](std::int32_t rc) {
        const std::stop_source source;
        lws::test::FakeDecodeLog log;
        lws::Decoder decoder{lws::test::fake_backend(
            log, {.rc = rc, .spans = {raw_span(0, 10)}, .raise_stop = &source})};
        const auto result = decoder.decode(silent_segment(3200), source.get_token());
        ASSERT_TRUE(result.has_value()) << rc;
        EXPECT_EQ(result->status, lws::DecodeStatus::Cancelled) << rc;
        EXPECT_TRUE(result->spans.empty()) << rc;
        EXPECT_EQ(log.span_reads, 0U) << rc;
    });
}

TEST(DecoderControl, AFailedRunWithoutStopIsDecodeFailedWithNoSpansRead) {
    constexpr std::array<std::int32_t, 3> kCodes{kEncodeFailed, kDecodeFailed, kGraphFailed};
    std::ranges::for_each(kCodes, [](std::int32_t rc) {
        lws::test::FakeDecodeLog log;
        lws::Decoder decoder{lws::test::fake_backend(log, {.rc = rc, .spans = {raw_span(0, 10)}})};
        const auto result = decoder.decode(silent_segment(3200), std::stop_source{}.get_token());
        ASSERT_FALSE(result.has_value()) << rc;
        EXPECT_EQ(result.error(), lws::Error::DecodeFailed) << rc;
        EXPECT_EQ(log.span_reads, 0U) << rc;
    });
}

TEST(DecoderControl, AFinishedRunWithALateStopIsStillDecoded) {
    const std::stop_source source;
    lws::test::FakeDecodeLog log;
    lws::Decoder decoder{
        lws::test::fake_backend(log, {.spans = {raw_span(0, 10)}, .raise_stop = &source})};
    const auto result = decoder.decode(silent_segment(3200), source.get_token());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status, lws::DecodeStatus::Decoded);
    ASSERT_EQ(result->spans.size(), 1U);
    EXPECT_EQ(result->spans.front().begin, 0U);
    EXPECT_EQ(result->spans.front().end, 1600U);
    EXPECT_EQ(log.span_reads, 1U);
}

TEST(DecoderControl, ATokenStoppedBeforeDecodeStillMakesTheOneCall) {
    const std::stop_source source;
    source.request_stop();
    lws::test::FakeDecodeLog log;
    lws::Decoder decoder{
        lws::test::fake_backend(log, {.spans = {raw_span(0, 10)}, .abort_when_stopped = true})};
    const auto result = decoder.decode(silent_segment(3200), source.get_token());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status, lws::DecodeStatus::Cancelled);
    EXPECT_TRUE(result->spans.empty());
    EXPECT_EQ(log.run_sizes, std::vector<std::size_t>{3200});
    EXPECT_EQ(log.span_reads, 0U);
}

TEST(DecoderControl, SpansCountFromTheSegmentsOwnFirstSample) {
    lws::test::FakeDecodeLog log;
    lws::Decoder decoder{lws::test::fake_backend(log, {.spans = {raw_span(0, 524)}})};
    lws::Segment segment = silent_segment(100000);
    segment.start = 1000000;
    const auto result = decoder.decode(segment, std::stop_source{}.get_token());
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->spans.size(), 1U);
    EXPECT_EQ(result->spans.front().begin, 0U);
    EXPECT_EQ(result->spans.front().end, 83840U);
}
