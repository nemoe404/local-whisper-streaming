// The two position accessors of the Segmenter with fake engines: what the windower has been fed and
// what the reader still holds. The stream clock is built from them. No ggml compute.
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include "fake_vad.hpp"
#include "lws/options.hpp"
#include "segmenter_support.hpp"
#include "shell/segmenter.hpp"

namespace {

using lws::test::FakeVadLog;
using lws::test::kFailMarker;
using lws::test::make_segmenter;
using lws::test::push_chunked;
using lws::test::stream_of;

constexpr std::size_t kFrame = lws::kVadFrameSamples;

} // namespace

TEST(SegmenterPosition, StartsAtZero) {
    FakeVadLog log;
    const auto segmenter = make_segmenter(log);
    EXPECT_EQ(segmenter.fed_samples(), 0U);
    EXPECT_EQ(segmenter.pending(), 0U);
}

TEST(SegmenterPosition, WholeFramesAreFedAndTheRestIsPending) {
    FakeVadLog log;
    auto segmenter = make_segmenter(log);
    const std::vector<float> silence(1000, 0.0F);
    ASSERT_TRUE(segmenter.push(silence).has_value());
    EXPECT_EQ(segmenter.fed_samples(), kFrame);
    EXPECT_EQ(segmenter.pending(), 1000 - kFrame);
}

TEST(SegmenterPosition, AnOverrunEmptiesTheReaderAndKeepsTheFedCount) {
    FakeVadLog log;
    auto segmenter = make_segmenter(log);
    const std::vector<float> silence(1000, 0.0F);
    ASSERT_TRUE(segmenter.push(silence).has_value());
    std::ignore = segmenter.notify_overrun();
    EXPECT_EQ(segmenter.fed_samples(), kFrame);
    EXPECT_EQ(segmenter.pending(), 0U);
}

TEST(SegmenterPosition, AFailedCallFeedsNothingAndKeepsTheLeftover) {
    FakeVadLog log;
    auto segmenter = make_segmenter(log);
    const std::vector<float> first(600, 0.0F);
    ASSERT_TRUE(segmenter.push(first).has_value());
    ASSERT_EQ(segmenter.fed_samples(), kFrame);
    ASSERT_EQ(segmenter.pending(), first.size() - kFrame);

    // 88 samples are held and 1500 are given: three whole frames and a partial one of 52. The
    // second frame starts at sample 424 of the given samples, and its marker fails the call.
    std::vector<float> given(1500, 0.0F);
    given[kFrame - (first.size() - kFrame)] = kFailMarker;
    const auto failed = segmenter.push(given);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(segmenter.fed_samples(), kFrame);
    EXPECT_EQ(segmenter.pending(), (first.size() - kFrame + given.size()) % kFrame);
}

TEST(SegmenterPosition, AFlushChangesNeitherCount) {
    FakeVadLog log;
    auto segmenter = make_segmenter(log);
    auto speech = stream_of({{0.9F, 20}});
    speech.insert(speech.end(), 100, 0.9F);
    const auto pushed = push_chunked(segmenter, speech, 700);
    ASSERT_FALSE(pushed.failure.has_value());
    const std::uint64_t fed = segmenter.fed_samples();
    const std::size_t pending = segmenter.pending();
    ASSERT_EQ(fed, 20 * kFrame);
    ASSERT_EQ(pending, 100U);
    EXPECT_TRUE(segmenter.flush().has_value());
    EXPECT_EQ(segmenter.fed_samples(), fed);
    EXPECT_EQ(segmenter.pending(), pending);
}
